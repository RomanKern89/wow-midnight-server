/*
 * Companion Bots — Phase 3.2 pbot async reload machinery (TrinityCore master, retail 12.0.7).
 *
 * Implements DESIGN_PHASE3.md SS9 PART A (A.1 loader recipe verbatim in shape, A.2 reload sequence).
 *
 * KEY DEVIATION FROM A.1 (tc-src wins, per task): A.1's recipe says
 *   std::make_shared<LoginQueryHolder>(accountId, botGuid)
 * but LoginQueryHolder is a FILE-LOCAL class DEFINED INSIDE CharacterHandler.cpp — WorldSession.h
 * only forward-declares it (WorldSession.h:46), so it is an INCOMPLETE TYPE in every other TU and
 * cannot be constructed here. The PlayerLoginQueryIndex slot enum, however, IS public (Player.h:957).
 * So we build our OWN CharacterDatabaseQueryHolder-derived holder (PbotLoginQueryHolder) that fills
 * the exact same slots. Player::LoadFromDB takes CharacterDatabaseQueryHolder const& (Player.h:1868),
 * NOT LoginQueryHolder, so any holder with the right slots filled works — no core patch needed.
 *   -> DRIFT WATCHPOINT: s_loginStmts below is a faithful snapshot of LoginQueryHolder::Initialize
 *      (CharacterHandler.cpp:83-366). If the engine adds a login query slot, this table won't fill it
 *      and the bot silently won't load that data (soft failure). If a CHAR_SEL_* is renamed/removed,
 *      this TU fails to compile (loud, desired). The cleaner long-term fix is a tiny core patch that
 *      relocates LoginQueryHolder's DECLARATION to a shared header — flagged to the team lead.
 *
 * FORBIDDEN LIST (SS8.6) — still absolute here: our OWN AsyncCallbackProcessor is polled from
 * PbotWorldScript::OnUpdate. We NEVER call WorldSession::AddQueryHolderCallback / Update /
 * ProcessQueryCallbacks.
 */

#include "pbot_loader.h"
#include "pbot_mgr.h"

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnv.h"     // CharacterDatabase, CharacterDatabaseStatements, QueryHolder types
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"          // Player, LoadFromDB, PlayerLoginQueryIndex, MAX_PLAYER_LOGIN_QUERY
#include "QueryHolder.h"     // SQLQueryHolderCallback
#include "StringFormat.h"
#include "World.h"           // sWorld, CONFIG_DECLINED_NAMES_USED
#include "WorldSession.h"

#include <memory>
#include <unordered_set>
#include <vector>

namespace
{
    // Review finding CRITICAL: in-flight reload bookkeeping. A bot is "pending" from the moment its
    // reload is submitted (EnqueueReload) until its async DB work completes (OnHolderReady). During
    // that window it is NOT in PbotMgr::_bots (not live) yet its character row exists, so a
    // roster-enumerating RetireAll could otherwise delete the row mid-load. _retireOnLoad holds bots
    // a retire was requested for while still pending; OnHolderReady honors it after the load resolves.
    std::unordered_set<ObjectGuid> s_pendingBots;
    std::unordered_set<ObjectGuid> s_retireOnLoad;
    // ---- replica login holder (see file header) ---------------------------------------------
    struct LoginStmt
    {
        CharacterDatabaseStatements stmt;
        PlayerLoginQueryIndex       slot;
    };

    // Faithful snapshot of LoginQueryHolder::Initialize (CharacterHandler.cpp:83-366), engine order.
    // The single config-gated slot (DECLINED_NAMES) is handled separately below, exactly as the
    // engine does (CharacterHandler.cpp:230-235).
    constexpr LoginStmt s_loginStmts[] =
    {
        { CHAR_SEL_CHARACTER,                                       PLAYER_LOGIN_QUERY_LOAD_FROM },
        { CHAR_SEL_CHARACTER_CUSTOMIZATIONS,                        PLAYER_LOGIN_QUERY_LOAD_CUSTOMIZATIONS },
        { CHAR_SEL_GROUP_MEMBER,                                    PLAYER_LOGIN_QUERY_LOAD_GROUP },
        { CHAR_SEL_CHARACTER_AURAS,                                 PLAYER_LOGIN_QUERY_LOAD_AURAS },
        { CHAR_SEL_CHARACTER_AURA_EFFECTS,                          PLAYER_LOGIN_QUERY_LOAD_AURA_EFFECTS },
        { CHAR_SEL_CHARACTER_AURA_STORED_LOCATIONS,                 PLAYER_LOGIN_QUERY_LOAD_AURA_STORED_LOCATIONS },
        { CHAR_SEL_CHARACTER_SPELL,                                 PLAYER_LOGIN_QUERY_LOAD_SPELLS },
        { CHAR_SEL_CHARACTER_SPELL_FAVORITES,                       PLAYER_LOGIN_QUERY_LOAD_SPELL_FAVORITES },
        { CHAR_SEL_CHARACTER_QUESTSTATUS,                           PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_OBJECTIVES,                PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_OBJECTIVES },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_OBJECTIVES_CRITERIA,       PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_OBJECTIVES_CRITERIA },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_OBJECTIVES_CRITERIA_PROGRESS, PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_OBJECTIVES_CRITERIA_PROGRESS },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_OBJECTIVES_SPAWN_TRACKING, PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_OBJECTIVES_SPAWN_TRACKING },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_DAILY,                     PLAYER_LOGIN_QUERY_LOAD_DAILY_QUEST_STATUS },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_WEEKLY,                    PLAYER_LOGIN_QUERY_LOAD_WEEKLY_QUEST_STATUS },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_MONTHLY,                   PLAYER_LOGIN_QUERY_LOAD_MONTHLY_QUEST_STATUS },
        { CHAR_SEL_CHARACTER_QUESTSTATUS_SEASONAL,                  PLAYER_LOGIN_QUERY_LOAD_SEASONAL_QUEST_STATUS },
        { CHAR_SEL_CHARACTER_REPUTATION,                            PLAYER_LOGIN_QUERY_LOAD_REPUTATION },
        { CHAR_SEL_CHARACTER_INVENTORY,                             PLAYER_LOGIN_QUERY_LOAD_INVENTORY },
        { CHAR_SEL_ITEM_INSTANCE_ARTIFACT,                         PLAYER_LOGIN_QUERY_LOAD_ARTIFACTS },
        { CHAR_SEL_ITEM_INSTANCE_AZERITE,                          PLAYER_LOGIN_QUERY_LOAD_AZERITE },
        { CHAR_SEL_ITEM_INSTANCE_AZERITE_MILESTONE_POWER,          PLAYER_LOGIN_QUERY_LOAD_AZERITE_MILESTONE_POWERS },
        { CHAR_SEL_ITEM_INSTANCE_AZERITE_UNLOCKED_ESSENCE,         PLAYER_LOGIN_QUERY_LOAD_AZERITE_UNLOCKED_ESSENCES },
        { CHAR_SEL_ITEM_INSTANCE_AZERITE_EMPOWERED,                PLAYER_LOGIN_QUERY_LOAD_AZERITE_EMPOWERED },
        { CHAR_SEL_MAIL,                                            PLAYER_LOGIN_QUERY_LOAD_MAILS },
        { CHAR_SEL_MAILITEMS,                                       PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS },
        { CHAR_SEL_MAILITEMS_ARTIFACT,                             PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS_ARTIFACT },
        { CHAR_SEL_MAILITEMS_AZERITE,                             PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS_AZERITE },
        { CHAR_SEL_MAILITEMS_AZERITE_MILESTONE_POWER,            PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS_AZERITE_MILESTONE_POWER },
        { CHAR_SEL_MAILITEMS_AZERITE_UNLOCKED_ESSENCE,           PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS_AZERITE_UNLOCKED_ESSENCE },
        { CHAR_SEL_MAILITEMS_AZERITE_EMPOWERED,                  PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS_AZERITE_EMPOWERED },
        { CHAR_SEL_CHARACTER_SOCIALLIST,                           PLAYER_LOGIN_QUERY_LOAD_SOCIAL_LIST },
        { CHAR_SEL_CHARACTER_HOMEBIND,                             PLAYER_LOGIN_QUERY_LOAD_HOME_BIND },
        { CHAR_SEL_CHARACTER_SPELLCOOLDOWNS,                       PLAYER_LOGIN_QUERY_LOAD_SPELL_COOLDOWNS },
        { CHAR_SEL_CHARACTER_SPELL_CHARGES,                        PLAYER_LOGIN_QUERY_LOAD_SPELL_CHARGES },
        { CHAR_SEL_GUILD_MEMBER,                                   PLAYER_LOGIN_QUERY_LOAD_GUILD },
        { CHAR_SEL_CHARACTER_ARENAINFO,                            PLAYER_LOGIN_QUERY_LOAD_ARENA_INFO },
        { CHAR_SEL_CHARACTER_ACHIEVEMENTS,                         PLAYER_LOGIN_QUERY_LOAD_ACHIEVEMENTS },
        { CHAR_SEL_CHARACTER_CRITERIAPROGRESS,                     PLAYER_LOGIN_QUERY_LOAD_CRITERIA_PROGRESS },
        { CHAR_SEL_CHARACTER_EQUIPMENTSETS,                        PLAYER_LOGIN_QUERY_LOAD_EQUIPMENT_SETS },
        { CHAR_SEL_CHARACTER_TRANSMOG_OUTFITS,                     PLAYER_LOGIN_QUERY_LOAD_TRANSMOG_OUTFITS },
        { CHAR_SEL_CHARACTER_TRANSMOG_OUTFIT,                      PLAYER_LOGIN_QUERY_LOAD_TRANSMOG_OUTFIT },
        { CHAR_SEL_CHARACTER_TRANSMOG_OUTFIT_SITUATION,           PLAYER_LOGIN_QUERY_LOAD_TRANSMOG_OUTFIT_SITUATION },
        { CHAR_SEL_CHARACTER_TRANSMOG_OUTFIT_SLOT,                PLAYER_LOGIN_QUERY_LOAD_TRANSMOG_OUTFIT_SLOT },
        { CHAR_SEL_CHAR_CUF_PROFILES,                              PLAYER_LOGIN_QUERY_LOAD_CUF_PROFILES },
        { CHAR_SEL_CHARACTER_BGDATA,                               PLAYER_LOGIN_QUERY_LOAD_BG_DATA },
        { CHAR_SEL_CHARACTER_GLYPHS,                               PLAYER_LOGIN_QUERY_LOAD_GLYPHS },
        { CHAR_SEL_CHARACTER_TALENTS,                              PLAYER_LOGIN_QUERY_LOAD_TALENTS },
        { CHAR_SEL_CHARACTER_PVP_TALENTS,                          PLAYER_LOGIN_QUERY_LOAD_PVP_TALENTS },
        { CHAR_SEL_PLAYER_ACCOUNT_DATA,                            PLAYER_LOGIN_QUERY_LOAD_ACCOUNT_DATA },
        { CHAR_SEL_CHARACTER_SKILLS,                               PLAYER_LOGIN_QUERY_LOAD_SKILLS },
        { CHAR_SEL_CHARACTER_RANDOMBG,                             PLAYER_LOGIN_QUERY_LOAD_RANDOM_BG },
        { CHAR_SEL_CHARACTER_BANNED,                               PLAYER_LOGIN_QUERY_LOAD_BANNED },
        { CHAR_SEL_CHARACTER_QUESTSTATUSREW,                       PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_REW },
        { CHAR_SEL_PLAYER_CURRENCY,                                PLAYER_LOGIN_QUERY_LOAD_CURRENCY },
        { CHAR_SEL_CORPSE_LOCATION,                                PLAYER_LOGIN_QUERY_LOAD_CORPSE_LOCATION },
        { CHAR_SEL_CHAR_PETS,                                      PLAYER_LOGIN_QUERY_LOAD_PET_SLOTS },
        { CHAR_SEL_CHARACTER_GARRISON,                             PLAYER_LOGIN_QUERY_LOAD_GARRISON },
        { CHAR_SEL_CHARACTER_GARRISON_BLUEPRINTS,                  PLAYER_LOGIN_QUERY_LOAD_GARRISON_BLUEPRINTS },
        { CHAR_SEL_CHARACTER_GARRISON_BUILDINGS,                   PLAYER_LOGIN_QUERY_LOAD_GARRISON_BUILDINGS },
        { CHAR_SEL_CHARACTER_GARRISON_FOLLOWERS,                   PLAYER_LOGIN_QUERY_LOAD_GARRISON_FOLLOWERS },
        { CHAR_SEL_CHARACTER_GARRISON_FOLLOWER_ABILITIES,          PLAYER_LOGIN_QUERY_LOAD_GARRISON_FOLLOWER_ABILITIES },
        { CHAR_SEL_CHAR_TRAIT_ENTRIES,                             PLAYER_LOGIN_QUERY_LOAD_TRAIT_ENTRIES },
        { CHAR_SEL_CHAR_TRAIT_CONFIGS,                             PLAYER_LOGIN_QUERY_LOAD_TRAIT_CONFIGS },
        { CHAR_SEL_PLAYER_DATA_ELEMENTS_CHARACTER,                 PLAYER_LOGIN_QUERY_LOAD_DATA_ELEMENTS },
        { CHAR_SEL_PLAYER_DATA_FLAGS_CHARACTER,                    PLAYER_LOGIN_QUERY_LOAD_DATA_FLAGS },
        { CHAR_SEL_CHARACTER_BANK_TAB_SETTINGS,                    PLAYER_LOGIN_QUERY_LOAD_BANK_TAB_SETTINGS },
    };

    class PbotLoginQueryHolder : public CharacterDatabaseQueryHolder
    {
    public:
        explicit PbotLoginQueryHolder(ObjectGuid guid) : _guid(guid) { }
        ObjectGuid GetGuid() const { return _guid; }

        bool Initialize()
        {
            SetSize(MAX_PLAYER_LOGIN_QUERY);
            ObjectGuid::LowType const lowGuid = _guid.GetCounter();
            bool res = true;
            for (LoginStmt const& e : s_loginStmts)
            {
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(e.stmt);
                stmt->setUInt64(0, lowGuid);
                res &= SetPreparedQuery(e.slot, stmt);
            }
            if (sWorld->getBoolConfig(CONFIG_DECLINED_NAMES_USED))
            {
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARACTER_DECLINEDNAMES);
                stmt->setUInt64(0, lowGuid);
                res &= SetPreparedQuery(PLAYER_LOGIN_QUERY_LOAD_DECLINED_NAMES, stmt);
            }
            return res;
        }

    private:
        ObjectGuid _guid;
    };

    // ---- our OWN async processor, polled from PbotWorldScript::OnUpdate (SS8.6-safe) --------
    AsyncCallbackProcessor<SQLQueryHolderCallback> s_pendingLoads;

    // Completion handler: build the Player, LoadFromDB from the now-filled holder, and hand off to
    // PbotMgr for placement + registration. Runs on the world thread from ProcessReadyCallbacks.
    void OnHolderReady(ObjectGuid ownerGuid, ObjectGuid botGuid, WorldSession* session, uint8 classId,
        SQLQueryHolderBase const& holderBase)
    {
        // No longer in flight — clear the pending flag FIRST so any RetireBot re-dispatched below takes
        // a race-free path (bot is either live or fully torn down by the time we call it).
        s_pendingBots.erase(botGuid);
        bool const retireRequested = s_retireOnLoad.erase(botGuid) > 0;

        // The real dynamic type is PbotLoginQueryHolder (IS-A CharacterDatabaseQueryHolder); downcast
        // to the base LoadFromDB expects, exactly like CharacterHandler.cpp:1138 casts to LoginQueryHolder.
        CharacterDatabaseQueryHolder const& holder = static_cast<CharacterDatabaseQueryHolder const&>(holderBase);

        Player* bot = new Player(session);
        bot->GetMotionMaster()->Initialize();

        if (!bot->LoadFromDB(botGuid, holder))
        {
            // Character row missing/corrupt, or account mismatch (Player.cpp:18121). Drop the stale
            // roster row so we stop retrying it every login (A.2 step 5). m_social is still null here
            // (LoadFromDB failed before Player.cpp:18684) — same as the Create-failure teardown path.
            bot->CleanupsBeforeDelete();
            delete bot;
            delete session;
            PbotMgr::DeleteRosterRow(botGuid);
            TC_LOG_ERROR("scripts.bots", "PbotLoader: LoadFromDB failed for bot {} (owner {}); dropped roster row.",
                botGuid.ToString(), ownerGuid.ToString());
            // A retire was requested for a bot whose row is already gone — nothing left to retire.
            return;
        }

        // A.1 RESOLVED QUESTION — do NOT call InitializeEmptySocial here. Unlike Player::Create (which
        // never touches m_social, hence the SpawnBot fix), LoadFromDB DID load it:
        // Player.cpp:18684 m_social = sSocialMgr->LoadFromDB(PLAYER_LOGIN_QUERY_LOAD_SOCIAL_LIST, guid).
        // Calling InitializeEmptySocial now would OVERWRITE the loaded friends list with an empty one.

        if (!PbotMgr::FinalizeReloadedBot(ownerGuid, bot, session, classId))
        {
            // Owner logged off during the async window, or a duplicate reload raced us. Tear down the
            // freshly-loaded Player WITHOUT deleting its character row — the roster row stays so it
            // reloads on the owner's next login. Not in world/ObjectAccessor yet, so no LogoutPlayer.
            session->SetPlayer(nullptr);
            bot->ResetMap();
            bot->CleanupsBeforeDelete();
            delete bot;
            delete session;
            // Honor a deferred retire (bot is offline now, no pending load): non-live RetireBot path is
            // safe — pending flag already cleared above.
            if (retireRequested)
                PbotMgr::RetireBot(botGuid);
            return;
        }

        // Bot is now live and registered. Honor a retire that was requested while it was still loading:
        // the normal LIVE RetireBot path (LogoutPlayer + DeleteFromDB) runs, race-free.
        if (retireRequested)
            PbotMgr::RetireBot(botGuid);
    }

    void EnqueueReload(ObjectGuid ownerGuid, ObjectGuid botGuid, uint32 accountId, uint8 classId)
    {
        // Reload MUST use the roster's stored account id (A.1, Player.cpp:18121 account-match check),
        // never a fresh transient-pool account.
        WorldSession* session = PbotMgr::CreateBotSession(accountId);
        if (!session)
        {
            TC_LOG_ERROR("scripts.bots", "PbotLoader: could not build session (account {}) for bot {}.",
                accountId, botGuid.ToString());
            return;
        }

        auto holder = std::make_shared<PbotLoginQueryHolder>(botGuid);
        if (!holder->Initialize())
        {
            delete session;
            PbotMgr::DeleteRosterRow(botGuid);
            TC_LOG_ERROR("scripts.bots", "PbotLoader: holder Initialize failed for bot {}; dropped roster row.",
                botGuid.ToString());
            return;
        }

        // shared_ptr<PbotLoginQueryHolder> -> shared_ptr<SQLQueryHolder<CharacterDatabaseConnection>>
        // is a safe upcast (public derivation). This is the ONLY submission primitive we use.
        SQLQueryHolderCallback callback = CharacterDatabase.DelayQueryHolder(holder);
        callback.AfterComplete([ownerGuid, botGuid, session, classId](SQLQueryHolderBase const& holderBase)
        {
            OnHolderReady(ownerGuid, botGuid, session, classId, holderBase);
        });
        s_pendingLoads.AddCallback(std::move(callback));
        s_pendingBots.insert(botGuid); // in flight until OnHolderReady clears it (review CRITICAL)
    }
}

// ---- public interface -----------------------------------------------------------------------

void PbotLoader::EnqueueRosterReload(ObjectGuid ownerGuid, ObjectGuid botGuid, uint32 accountId, uint8 classId)
{
    if (PbotMgr::IsBotLive(botGuid) || s_pendingBots.count(botGuid))
        return;

    EnqueueReload(ownerGuid, botGuid, accountId, classId);
}

void PbotLoader::ReloadOwnerRoster(ObjectGuid ownerGuid)
{
    for (PbotRosterEntry const& e : PbotMgr::QueryRosterForOwner(ownerGuid))
    {
        if (PbotMgr::IsBotLive(e.BotGuid))
            continue; // double-spawn guard (A.3): already reloaded (OnStartup, or a login race)
        if (s_pendingBots.count(e.BotGuid))
            continue; // review MEDIUM #2: a reload for this bot is already in flight, don't double-enqueue
        EnqueueReload(ownerGuid, e.BotGuid, e.AccountId, e.ClassId);
    }
}

bool PbotLoader::IsReloadPending(ObjectGuid botGuid)
{
    return s_pendingBots.count(botGuid) > 0;
}

void PbotLoader::RequestRetireOnLoad(ObjectGuid botGuid)
{
    s_retireOnLoad.insert(botGuid);
}

void PbotLoader::SweepOrphansAtStartup()
{
    // A.4 risk 1: retire roster rows whose owner character no longer exists (deleted out of band).
    //
    // Owner existence is checked AUTHORITATIVELY against the characters table, NOT sCharacterCache:
    // the ordering of sCharacterCache's load relative to WorldScript::OnStartup is not verifiable from
    // this sparse checkout, and a not-yet-populated cache would falsely report EVERY owner missing and
    // mass-retire (permanently delete) every persisted bot. A direct DB read is timing-independent.
    for (ObjectGuid ownerGuid : PbotMgr::QueryAllRosterOwners())
    {
        // Owner 0 is not a missing owner — it is a WORLD bot, which has no owner by definition.
        // Without this guard the sweep looks for character guid 0, finds nothing, and permanently
        // deletes the entire world population on the first startup after they became persistent.
        //
        // Test the COUNTER, not IsEmpty(). QueryAllRosterOwners builds these with
        // ObjectGuid::Create<HighGuid::Player>(0), which is a Player guid whose counter is zero —
        // its type bits are set, so IsEmpty() is FALSE for it. The first version of this guard used
        // IsEmpty(), never fired once, and the sweep deleted all sixty bots on the very next boot:
        // "orphan sweep retiring bot Player-1-00000042 (owner Player-1-00000000 gone)".
        if (!ownerGuid.GetCounter())
            continue;

        QueryResult ownerExists = CharacterDatabase.Query(Trinity::StringFormat(
            "SELECT 1 FROM characters WHERE guid = {}", ownerGuid.GetCounter()).c_str());
        if (ownerExists)
            continue; // owner still exists — keep their roster

        for (PbotRosterEntry const& e : PbotMgr::QueryRosterForOwner(ownerGuid))
        {
            TC_LOG_INFO("scripts.bots", "PbotLoader: orphan sweep retiring bot {} (owner {} gone).",
                e.BotGuid.ToString(), ownerGuid.ToString());
            PbotMgr::RetireBot(e.BotGuid);
        }
    }
}

void PbotLoader::PumpPendingLoads()
{
    // Non-blocking: invokes each callback whose async DB work has completed and removes it.
    s_pendingLoads.ProcessReadyCallbacks();
}
