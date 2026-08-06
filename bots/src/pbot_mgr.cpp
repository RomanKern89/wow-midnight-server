/*
 * Companion Bots — Phase 3.1 PbotMgr implementation (TrinityCore master, retail 12.0.7).
 *
 * Fabricates socket-less fake-player bots per DESIGN_PHASE3 SS8.4 (spawn) and SS8.5 (dismiss/
 * shutdown), enforcing the SS8.6 FORBIDDEN LIST. Every engine call sequence here mirrors an existing
 * CharacterHandler.cpp / WorldSession.cpp call site verbatim; deviations from the spec text (where
 * tc-src disagreed with the spec) are called out inline with the source reason.
 *
 * Threading: the world update is single-threaded; the registry maps are plain (no locking), same
 * discipline as the Phase 1 BotMgr registry.
 */

#include "pbot_mgr.h"
#include "pbot_ai.h"
#include "pbot_gear.h" // PbotIdentity: ClassId / PickRaceForTeam / IsValidCombo / PickName
#include "pbot_party.h" // PbotParty: group membership + level match (Phase 5)
#include "pbot_gather.h" // PbotGather::GrantGatheringSkills (Phase 5)
#include "pbot_equip.h"  // PbotEquip::GearUp — level-appropriate gear
#include "pbot_loader.h" // PbotLoader::IsReloadPending / RequestRetireOnLoad (review CRITICAL race guard)
#include "pbot_profession.h" // one craft per bot, so the world makes things and not only gathers them

#include "AccountMgr.h"
#include "BattlenetAccountMgr.h" // shared bot battlenet account (item-appearance foreign key)
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "ClientBuildInfo.h"
#include "DatabaseEnv.h"     // WorldDatabase (pbot_roster ops), CharacterDatabase, QueryResult/Field
#include "Duration.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"  // sMapMgr->CreateMap for ownerless world bots (Phase 6)
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PhasingHandler.h"  // a bot that never logs in never gets a phase shift — and is blind
#include "Player.h"
#include "Realm.h"
#include "RealmList.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---- static member storage ------------------------------------------------------------------
std::unordered_map<ObjectGuid, PbotMgr::BotRecord> PbotMgr::_bots;
std::unordered_multimap<ObjectGuid, ObjectGuid>    PbotMgr::_byOwner;
std::deque<PbotPopulateRequest>                    PbotMgr::_populateQueue;
uint32                                             PbotMgr::_populateTimerMs = 0;
std::deque<PbotRosterEntry>                        PbotMgr::_reloadQueue;
uint32                                             PbotMgr::_reloadTimerMs = 0;

// CRITICAL review fix #1: Player::m_social is only ever assigned inside the client-login pipeline
// (Player::LoadFromDB), which pbots deliberately never run — leaving it null. Player::GetSocial()
// is un-null-checked and at least GroupHandler.cpp:103 / Guild.cpp:1690 dereference it for any
// name-targeted /invite or /ginvite, so a null social = any player can crash the server by
// inviting a bot by name. Player is declared `final` on master (no subclass hook), so the fix is a
// minimal CORE PATCH: Player::InitializeEmptySocial() added to Player.h/.cpp (assigns an empty,
// valid PlayerSocial via sSocialMgr->LoadFromDB(null result, guid) — the same object login builds).
// Called below right after Player::Create() succeeds, before the bot becomes reachable.

namespace
{
    // Class mapping, race selection, combo validation, and name-pool picking all live in coder C's
    // PbotIdentity (pbot_gear.h) — SpawnBot consumes them directly, keeping identity data in one
    // place (SS8.1/SS8.7). This TU only owns the account-provisioning bookkeeping below.

    std::mt19937& Rng()
    {
        static std::mt19937 engine{ std::random_device{}() };
        return engine;
    }

    // ---- account provisioning (DESIGN_PHASE3 SS8.4, lazy on-demand) -------------------------
    // One dedicated account per LIVE bot. Released accounts are reused within the process run;
    // otherwise a fresh PBOT<N> username is minted. We skip usernames that already exist (e.g. from
    // a prior run — pbots are transient per SS8.8, so a leftover PBOT account may exist) to preserve
    // the one-live-session-per-account invariant.
    std::vector<uint32> g_freeBotAccounts;
    uint32 g_accountProbe = 0;

    std::string MakeBotPassword()
    {
        static char const alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
        std::string pw;
        pw.reserve(PBOT_ACCOUNT_PASSWORD_LEN);
        for (uint32 i = 0; i < PBOT_ACCOUNT_PASSWORD_LEN; ++i)
            pw.push_back(alphabet[dist(Rng())]);
        return pw;
    }

    // One battlenet account shared by every bot, created on first use.
    //
    // Bot sessions used to pass battlenetAccountId 0. That is not merely "no account": the moment a
    // bot picks up an item, CollectionMgr writes an appearance row keyed by that id, and
    // battlenet_item_appearances has a foreign key to battlenet_accounts. Every item every bot ever
    // looted therefore produced
    //     [1452] Cannot add or update a child row: a foreign key constraint fails
    // and DBErrors.log grew by gigabytes in minutes, burying real database errors.
    //
    // The bots share ONE account because the collection it keys is cosmetic appearance data that
    // means nothing for a bot; what matters is that the id refers to a row that exists. The game
    // accounts are deliberately NOT linked to it (Battlenet::AccountMgr::LinkWithGameAccount) —
    // linking is indexed and limited, and nothing about a socket-less bot needs it.
    constexpr char const* PBOT_BNET_EMAIL = "PBOT@BOTS.LOCAL";   // stored uppercased by the engine

    uint32 AcquireBotBattlenetAccount()
    {
        static uint32 cached = 0;
        if (cached)
            return cached;

        cached = Battlenet::AccountMgr::GetId(PBOT_BNET_EMAIL);
        if (!cached)
        {
            Battlenet::AccountMgr::CreateBattlenetAccount(PBOT_BNET_EMAIL, MakeBotPassword(),
                                                          /*withGameAccount*/ false, nullptr);
            cached = Battlenet::AccountMgr::GetId(PBOT_BNET_EMAIL);

            if (cached)
                TC_LOG_INFO("scripts.bots", "PbotMgr: created shared bot battlenet account {} ({})",
                    cached, PBOT_BNET_EMAIL);
            else
                TC_LOG_ERROR("scripts.bots", "PbotMgr: could not create the shared bot battlenet "
                    "account — item appearance writes will keep failing the foreign key check");
        }
        return cached;
    }

    uint32 AcquireBotAccount()
    {
        if (!g_freeBotAccounts.empty())
        {
            uint32 id = g_freeBotAccounts.back();
            g_freeBotAccounts.pop_back();
            return id;
        }

        for (uint32 tries = 0; tries < PBOT_ACCOUNT_MAX_PROBE; ++tries)
        {
            ++g_accountProbe;
            std::string uname = std::string(PBOT_ACCOUNT_PREFIX) + std::to_string(g_accountProbe);
            if (uname.size() > MAX_ACCOUNT_STR)
                return 0; // username space exhausted for this scheme

            if (AccountMgr::GetId(uname) != 0)
                continue; // already exists — keep one-live-session-per-account, try the next index

            if (sAccountMgr->CreateAccount(uname, MakeBotPassword()) != AccountOpResult::AOR_OK)
                continue;

            uint32 id = AccountMgr::GetId(uname);
            if (id)
                return id;
        }
        return 0;
    }

    void ReleaseBotAccount(uint32 accountId)
    {
        if (accountId)
            g_freeBotAccounts.push_back(accountId);
    }

    // ---- pbot_roster raw SQL (DESIGN_PHASE3 SS9 A.2) ----------------------------------------
    // Bookkeeping table in the WORLD database (see world_pbot_roster.sql). All parameters are
    // integers (guid counters, account id, class id) so direct string formatting carries no
    // injection surface. Writes use DirectExecute so a subsequent COUNT (per-owner cap, A.4 risk 4)
    // observes them immediately, closing the rapid-double-spawn race.

    void RosterInsert(ObjectGuid botGuid, ObjectGuid ownerGuid, uint32 accountId, uint8 classId)
    {
        WorldDatabase.DirectExecute(Trinity::StringFormat(
            "INSERT INTO pbot_roster (bot_guid, owner_guid, account_id, class) VALUES ({}, {}, {}, {})",
            botGuid.GetCounter(), ownerGuid.GetCounter(), accountId, uint32(classId)).c_str());
    }

    void RosterDelete(ObjectGuid botGuid)
    {
        WorldDatabase.DirectExecute(Trinity::StringFormat(
            "DELETE FROM pbot_roster WHERE bot_guid = {}", botGuid.GetCounter()).c_str());
    }

    uint32 RosterCountForOwner(ObjectGuid ownerGuid)
    {
        QueryResult result = WorldDatabase.Query(Trinity::StringFormat(
            "SELECT COUNT(*) FROM pbot_roster WHERE owner_guid = {}", ownerGuid.GetCounter()).c_str());
        if (!result)
            return 0;
        return result->Fetch()[0].GetUInt32();
    }

    uint32 RosterAccountOf(ObjectGuid botGuid)
    {
        QueryResult result = WorldDatabase.Query(Trinity::StringFormat(
            "SELECT account_id FROM pbot_roster WHERE bot_guid = {}", botGuid.GetCounter()).c_str());
        if (!result)
            return 0;
        return result->Fetch()[0].GetUInt32();
    }
}

// ---- public API -----------------------------------------------------------------------------

Player* PbotMgr::SpawnBot(Player* owner, PbotClass classId, PbotSpawnError* err)
{
    auto fail = [err](std::string reason) -> Player*
    {
        if (err)
            err->Reason = std::move(reason);
        return nullptr;
    };

    if (!owner)
        return fail("No owner.");
    if (!owner->IsInWorld() || !owner->GetMap())
        return fail("Owner is not in the world.");

    uint8 classIdRaw = PbotIdentity::ClassId(classId);
    if (!classIdRaw)
        return fail("Unknown bot class.");

    // Per-owner cap (DESIGN_PHASE3 SS6/SS8, extended for SS9 A.4 risk 4): count BOTH live bots and
    // rostered-but-not-yet-reloaded ones. Since SpawnBot writes a roster row for every bot it creates
    // (and DismissBot no longer removes it), the roster count is the authoritative superset of live
    // bots; max() with the live count is belt-and-suspenders against a transient DB read failure.
    uint32 ownedCount = std::max<uint32>(uint32(_byOwner.count(owner->GetGUID())),
        RosterCountForOwner(owner->GetGUID()));
    if (ownedCount >= PBOT_MAX_PER_OWNER)
        return fail("You already have the maximum number of companion bots.");

    // Server-wide cap (review finding #6).
    if (uint32(_bots.size()) >= PBOT_GLOBAL_MAX)
        return fail("The server-wide companion bot limit has been reached, try again later.");

    // Faction-matched race (Phase 4B): any race on the owner's side that can actually BE this
    // class on this realm, chosen at random so a squad looks like a real party rather than five
    // clones. PickRaceForTeam derives its candidate set from the engine's own creation data, so a
    // non-zero result is by construction a combination Player::Create accepts; IsValidCombo below
    // is a belt-and-braces re-check on the specific pair we ended up with.
    uint8 race = PbotIdentity::PickRaceForTeam(Player::TeamForRace(owner->GetRace()), classIdRaw);
    if (!race || !PbotIdentity::IsValidCombo(race, classIdRaw))
        return fail("No playable race on your faction can be that class.");

    // Pick a collision-checked, name-rule-valid name BEFORE allocating any account/session, so a
    // name-exhaustion failure costs nothing to unwind (PickName returns "" only when exhausted).
    std::string botName = PbotIdentity::PickName();
    if (botName.empty())
        return fail("No free bot name is available.");

    // ---- 1. Account ------------------------------------------------------------------------
    uint32 accountId = AcquireBotAccount();
    if (!accountId)
        return fail("Could not provision a bot account.");

    // ---- 2. Socket-less WorldSession (SS8.4 arg table). NEVER sWorld->AddSession() it. --------
    // Extracted to CreateBotSession so the reload path (pbot_loader.cpp) builds an identical session.
    WorldSession* session = CreateBotSession(accountId);

    // ---- 3-8. Player construction through session wiring (shared with the world-bot path) -----
    Player* bot = CreateBotCharacter(session, accountId, race, classIdRaw, botName);
    if (!bot)
    {
        delete session;
        ReleaseBotAccount(accountId);
        return fail("Player::Create failed (race/class/appearance rejected).");
    }

    // ---- 9. Place on the OWNER's map next to the owner (shared with the reload path) ---------
    if (!PlaceInWorldNextToOwner(owner, bot))
    {
        // Not yet in world/ObjectAccessor. Detach and tear down without LogoutPlayer (which assumes
        // an added-to-map player). Null the session's player first so the session dtor won't touch
        // the Player we are about to delete. Fresh account with no roster row yet -> release it.
        session->SetPlayer(nullptr);
        bot->ResetMap();
        bot->CleanupsBeforeDelete();
        delete bot;
        delete session;
        ReleaseBotAccount(accountId);
        return fail("AddPlayerToMap failed.");
    }

    // ---- 10. Persist the roster row so the bot survives a restart (SS9 A.2). Store the engine
    //          unit class id; DirectExecute so the per-owner cap COUNT above is immediately correct.
    RosterInsert(bot->GetGUID(), owner->GetGUID(), accountId, bot->GetClass());

    // ---- 11. Phase 5: make it a real party member. Level first, then group — a bot created at
    //          level 1 beside a high-level owner cannot survive anything the owner fights, and
    //          without group membership it never receives shared experience to climb out.
    //          Grouping is best-effort: a full party is a reason to fight ungrouped, not to fail
    //          the spawn (PbotParty::JoinOwnerGroup logs and returns false).
    PbotParty::SyncLevelToOwner(owner, bot);
    PbotParty::JoinOwnerGroup(owner, bot);

    // ---- 12. Register + spin up the controller. NEVER sWorld->AddSession() anywhere above. ---
    RegisterBotRecord(owner, bot, session, classId);

    TC_LOG_INFO("scripts.bots", "PbotMgr: spawned pbot {} (race {} class {} level {}) for owner {} on account {}.",
        bot->GetGUID().ToString(), uint32(race), uint32(classIdRaw), uint32(bot->GetLevel()),
        owner->GetGUID().ToString(), accountId);
    return bot;
}

// ---- shared spawn/reload internals ----------------------------------------------------------

Player* PbotMgr::CreateBotCharacter(WorldSession* session, uint32 accountId, uint8 raceId,
                                    uint8 classIdRaw, std::string const& name)
{
    // ---- 3. Player + MotionMaster (same order as CharacterHandler.cpp:967-972) --------------
    Player* bot = new Player(session);
    bot->GetMotionMaster()->Initialize();

    // ---- 4. Hand-built CharacterCreateInfo (never parsed from a packet) ---------------------
    WorldPackets::Character::CharacterCreateInfo createInfo;
    createInfo.Name  = name;
    createInfo.Race  = raceId;
    createInfo.Class = classIdRaw;
    createInfo.Sex   = GENDER_MALE;
    // Customizations intentionally left EMPTY (SS8.4 step 3, RESOLVED against tc-src): an empty list
    // PASSES WorldSession::ValidateAppearance (CharacterHandler.cpp:589) — the validator only loops
    // over the provided choices, so an empty list runs zero iterations and returns true, gated solely
    // by GetCustomiztionOptions(race, gender) != null, which holds for every playable race/gender in
    // a standard 12.0.7 DB2. The bot therefore renders with the race/gender default appearance
    // (plain but fully visible). If a richer look is wanted later, populate a minimal valid
    // ChrCustomizationChoice set here — it is not required for Create() to succeed.

    // ---- 5. Create (synchronous; also grants starter gear/spells/action bars internally) ----
    if (!bot->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &createInfo))
    {
        // session->_player was never set (Player ctor doesn't call SetPlayer), so tearing down the
        // Player here is safe; the caller still owns the session and the account.
        bot->CleanupsBeforeDelete();
        delete bot;
        return nullptr;
    }

    // CRITICAL review fix #1: non-null empty social BEFORE the bot becomes reachable by other
    // players (AddPlayerToMap/ObjectAccessor in the caller).
    bot->InitializeEmptySocial();

    // ---- 6. Persist (SS8.4 step 5). Simple bool overload is sufficient for a bot. ------------
    bot->SaveToDB(true);

    // ---- 7. Name cache so name-based lookups resolve the bot (mirrors CharacterHandler:1006) --
    sCharacterCache->AddCharacterCacheEntry(bot->GetGUID(), accountId, bot->GetName(),
        bot->GetNativeGender(), bot->GetRace(), bot->GetClass(), bot->GetLevel(), false);

    // ---- 8. Wire session -> player (SS8.4 step 7) -------------------------------------------
    session->SetPlayer(bot);
    return bot;
}

// ---- Phase 6: ownerless world bots ------------------------------------------------------------

Player* PbotMgr::SpawnWorldBot(uint32 mapId, Position const& pos, PbotClass classId, uint8 level,
                               uint32 team, PbotSpawnError* err)
{
    auto fail = [err](std::string reason) -> Player*
    {
        if (err)
            err->Reason = std::move(reason);
        return nullptr;
    };

    if (uint32(_bots.size()) >= PBOT_GLOBAL_MAX)
        return fail("The server-wide bot limit has been reached.");

    uint8 const classIdRaw = PbotIdentity::ClassId(classId);
    uint8 const race = PbotIdentity::PickRaceForTeam(team, classIdRaw);
    if (!race)
        return fail("No playable race on that faction can be that class.");

    std::string botName = PbotIdentity::PickName();
    if (botName.empty())
        return fail("No free bot name is available.");

    uint32 accountId = AcquireBotAccount();
    if (!accountId)
        return fail("Could not provision a bot account.");

    WorldSession* session = CreateBotSession(accountId);

    Player* bot = CreateBotCharacter(session, accountId, race, classIdRaw, botName);
    if (!bot)
    {
        delete session;
        ReleaseBotAccount(accountId);
        return fail("Player::Create failed (race/class/appearance rejected).");
    }

    // Resolve (creating if necessary) the world map, using the bot itself as the anchoring player.
    // For a non-instance map MapManager::CreateMap only consults the player for its team on
    // faction-split maps, so this is the same path a real character logging in would take — and it
    // is what makes a console spawn possible when no map is loaded because nobody is online.
    Map* map = sMapMgr->CreateMap(mapId, bot);
    if (!map)
    {
        session->SetPlayer(nullptr);
        bot->CleanupsBeforeDelete();
        delete bot;
        delete session;
        ReleaseBotAccount(accountId);
        return fail("Could not resolve or create that map.");
    }

    // Same placement recipe as PlaceInWorldNextToOwner, but anchored to an explicit point instead
    // of to an owner.
    bot->ResetMap();
    bot->Relocate(pos);
    bot->SetMap(map);
    if (!map->AddPlayerToMap(bot))
    {
        session->SetPlayer(nullptr);
        bot->ResetMap();
        bot->CleanupsBeforeDelete();
        delete bot;
        delete session;
        ReleaseBotAccount(accountId);
        return fail("AddPlayerToMap failed.");
    }
    ObjectAccessor::AddObject(bot);

    // ★★★ WITHOUT THIS THE BOT CANNOT SEE THE WORLD.
    //
    // A player's phase shift is initialised in Player::SendInitialPacketsAfterAddToMap, which is
    // part of the LOGIN path — and a bot is constructed and put on a map directly, so nothing ever
    // set it. An unphased observer fails WorldObject::CanSeeOrDetect against every creature, and
    // CanSeeOrDetect is the FIRST gate in IsValidAttackTarget. The result is a bot that can be
    // attacked but can never choose to attack: measured, 1248 creatures in range and 0 acceptable,
    // every one rejected as "not attackable", with canSee reported NO for each.
    //
    // That single missing call is why the bots barely levelled — they were not passive by design,
    // they were blind.
    PhasingHandler::OnMapChange(bot);

    // Level AFTER the bot is on a map, matching the order the owner-bound path uses. GiveLevel
    // fires level-change script hooks, and running those on a player with no map is a class of bug
    // not worth inviting for the sake of a few milliseconds.
    if (level > bot->GetLevel())
    {
        bot->GiveLevel(level);
        bot->LearnDefaultSkills();
        bot->LearnSpecializationSpells();
        bot->InitTalentForLevel();
        bot->SetFullHealth();
        bot->SetFullPower(bot->GetPowerType());
    }

    // A world bot lives in the world, so it gathers what the world offers. There is no trainer
    // interaction for it to learn this the long way (Phase 5).
    PbotGather::GrantGatheringSkills(bot);

    // And one craft to turn that raw material into something. Gathering without making leaves an
    // economy that only ever produces reagents.
    PbotProfession::GrantCraft(bot);
    PbotProfession::LearnRecipes(bot);

    // Flag for PvP. Without this two world bots of opposing factions standing next to each other
    // simply ignore one another: IsValidAttackTarget refuses an unflagged player outside a PvP
    // zone, so "the factions fight each other" never happens in the open world no matter what the
    // AI wants. A bot that lives in the world permanently is exactly the kind of character that
    // would be flagged.
    bot->SetPvP(true);
    bot->UpdatePvPState();

    // Gear AFTER the level is set, so the picks match the level it will actually fight at. Without
    // this the bot wears the level-1 starter kit no matter how high its level, and dies to mobs of
    // its own level — measured, not assumed.
    PbotEquip::GearUp(bot);

    // Registered under an EMPTY owner guid — that is precisely what marks it as a world bot, both
    // here and in PbotAI::IsWorldBot().
    BotRecord rec;
    rec.OwnerGuid = ObjectGuid::Empty;
    rec.BotGuid   = bot->GetGUID();
    rec.Session   = session;
    rec.Class     = classId;
    rec.Ai        = std::make_unique<PbotAI>(bot->GetGUID(), ObjectGuid::Empty);
    rec.Ai->SetHome(pos, mapId);

    _bots.emplace(bot->GetGUID(), std::move(rec));
    _byOwner.emplace(ObjectGuid::Empty, bot->GetGUID());

    // Rostered under owner 0. World bots were originally left OUT of the roster because they were
    // meant to be scenery — spawned, wandered, discarded. They are not that any more: they level,
    // quest, earn money, repair their gear, and are about to run professions and an auction. So the
    // roster row is what makes them survive a restart, exactly as it does for an owner's companions.
    //
    // Measured before this change: a restart took 64 characters down to a freshly rebuilt set, with
    // total played time falling from 124,077 seconds to 7,857 — everything they had ever done, gone.
    // (Total LEVEL barely moved, because replacements are created at the level of their zone. Read
    // played time, not level, when asking whether a character survived.)
    RosterInsert(bot->GetGUID(), ObjectGuid::Empty, accountId, uint8(bot->GetClass()));

    TC_LOG_INFO("scripts.bots", "PbotMgr: spawned WORLD bot {} ({} race {} class {} level {}) on map {}.",
        bot->GetName(), bot->GetGUID().ToString(), uint32(race), uint32(classIdRaw),
        uint32(bot->GetLevel()), map->GetId());
    return bot;
}

uint32 PbotMgr::ReloadWorldBots()
{
    // Owner 0 is the world population. Reuses the same async reload the companion path uses, so
    // there is one loading mechanism to be correct rather than two.
    // Staged, exactly like the spawn path — NOT all at once.
    //
    // Reloading the whole population in one go killed the server: the query holders all completed
    // within a short window, sixty players were pushed into the world back to back, and the world
    // thread stalled long enough for the anti-freeze watchdog to fire —
    // "World Thread hangs for 60004 ms, forcing a crash!". The spawn path learned this lesson
    // already and drip-feeds its bots; the reload path has to drip-feed too.
    uint32 queued = 0;
    for (PbotRosterEntry const& e : QueryRosterForOwner(ObjectGuid::Empty))
    {
        if (IsBotLive(e.BotGuid) || PbotLoader::IsReloadPending(e.BotGuid))
            continue;

        _reloadQueue.push_back(e);
        ++queued;
    }

    if (queued)
        TC_LOG_INFO("scripts.bots", "PbotMgr: bringing {} persisted world bots back.", queued);

    return queued;
}

uint32 PbotMgr::CountWorldBots()
{
    return uint32(_byOwner.count(ObjectGuid::Empty));
}

uint32 PbotMgr::SweepTransientBots()
{
    // Runs once at startup, before anybody can log in, so there is no live bot to race with.
    //
    // The account pool is the subtle half. AcquireBotAccount refuses any PBOT username that already
    // exists in auth, and ReleaseBotAccount only pushes the id onto an in-memory free list — so
    // after a restart every account ever minted is both "taken" (the row exists) and unknown to the
    // pool (the list starts empty). Left alone, the account index climbs forever and the auth table
    // fills with unusable rows. Re-deriving the free list from "PBOT accounts with no character"
    // fixes that permanently and costs three queries at boot.
    QueryResult accounts = LoginDatabase.Query("SELECT id FROM account WHERE username LIKE 'PBOT%'");
    if (!accounts)
        return 0;

    std::vector<uint32> botAccounts;
    std::string accountList;
    do
    {
        uint32 const id = (*accounts)[0].GetUInt32();
        botAccounts.push_back(id);
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(id);
    } while (accounts->NextRow());

    if (botAccounts.empty())
        return 0;

    // Rostered bots are owner-bound companions and must survive; anything else on a bot account is
    // a leftover world bot.
    std::unordered_set<uint64> rostered;
    if (QueryResult roster = WorldDatabase.Query("SELECT bot_guid FROM pbot_roster"))
        do
            rostered.insert((*roster)[0].GetUInt64());
        while (roster->NextRow());

    // guid -> account, for every character living on a bot account.
    std::unordered_map<uint32, uint32> charactersPerAccount;   // account -> surviving count
    for (uint32 id : botAccounts)
        charactersPerAccount[id] = 0;

    uint32 removed = 0;
    std::string const charQuery = "SELECT guid, account FROM characters WHERE account IN (" + accountList + ")";
    if (QueryResult chars = CharacterDatabase.Query(charQuery.c_str()))
    {
        do
        {
            uint64 const guidLow = (*chars)[0].GetUInt64();
            uint32 const accountId = (*chars)[1].GetUInt32();

            if (rostered.count(guidLow))
            {
                ++charactersPerAccount[accountId];   // companion bot: keep it, keep its account
                continue;
            }

            Player::DeleteFromDB(ObjectGuid::Create<HighGuid::Player>(guidLow), accountId,
                /*updateRealmChars*/ false, /*deleteFinally*/ true);
            ++removed;
        } while (chars->NextRow());
    }

    uint32 reclaimed = 0;
    for (uint32 id : botAccounts)
    {
        if (charactersPerAccount[id] == 0)
        {
            ReleaseBotAccount(id);
            ++reclaimed;
        }
    }

    TC_LOG_INFO("scripts.bots", "PbotMgr: startup sweep removed {} orphan bot characters and reclaimed {} bot accounts.",
        removed, reclaimed);
    return removed;
}

std::vector<ObjectGuid> PbotMgr::GetWorldBots()
{
    std::vector<ObjectGuid> result;
    for (auto range = _byOwner.equal_range(ObjectGuid::Empty); range.first != range.second; ++range.first)
        result.push_back(range.first->second);
    return result;
}

void PbotMgr::QueuePopulate(std::vector<PbotPopulateRequest> const& requests)
{
    for (PbotPopulateRequest const& req : requests)
        _populateQueue.push_back(req);
}

uint32 PbotMgr::PopulateQueueSize()
{
    return uint32(_populateQueue.size());
}

void PbotMgr::TickReloadQueue(uint32 diff)
{
    if (_reloadQueue.empty())
        return;

    if (_reloadTimerMs > diff)
    {
        _reloadTimerMs -= diff;
        return;
    }
    _reloadTimerMs = PBOT_POPULATE_INTERVAL_MS;

    PbotRosterEntry const e = _reloadQueue.front();
    _reloadQueue.pop_front();

    if (IsBotLive(e.BotGuid) || PbotLoader::IsReloadPending(e.BotGuid))
        return;   // something else got to it while it waited its turn

    PbotLoader::EnqueueRosterReload(ObjectGuid::Empty, e.BotGuid, e.AccountId, e.ClassId);
}

void PbotMgr::TickPopulateQueue(uint32 diff)
{
    if (_populateQueue.empty())
        return;

    if (_populateTimerMs > diff)
    {
        _populateTimerMs -= diff;
        return;
    }
    _populateTimerMs = PBOT_POPULATE_INTERVAL_MS;

    PbotPopulateRequest const req = _populateQueue.front();
    _populateQueue.pop_front();

    // Respect the global ceiling here rather than at queue time: bots may have been retired (or
    // spawned by another path) between queueing and now.
    if (_bots.size() >= PBOT_GLOBAL_MAX)
    {
        TC_LOG_ERROR("scripts.bots", "PbotMgr: population queue dropped a bot — global cap {} reached "
            "({} still queued)", PBOT_GLOBAL_MAX, uint32(_populateQueue.size()));
        return;
    }

    PbotSpawnError err;
    if (!SpawnWorldBot(req.MapId, req.Pos, req.Class, req.Level, req.Team, &err))
        TC_LOG_ERROR("scripts.bots", "PbotMgr: queued world bot for map {} failed: {}",
            req.MapId, err.Reason.empty() ? "unknown error" : err.Reason);
}

uint32 PbotMgr::RemoveAllWorldBots()
{
    // Drop anything still waiting to be created, or "clear" would be undone a second later by the
    // rest of the pending batch.
    _populateQueue.clear();

    // Snapshot first: RetireBot mutates both registries.
    std::vector<ObjectGuid> guids;
    for (auto range = _byOwner.equal_range(ObjectGuid::Empty); range.first != range.second; ++range.first)
        guids.push_back(range.first->second);

    uint32 removed = 0;
    for (ObjectGuid const& guid : guids)
    {
        // Retire, not dismiss: world bots write no roster row, so a dismissed one would leave an
        // orphaned character row and a permanently consumed bot account behind.
        if (RetireBot(guid))
            ++removed;
    }
    return removed;
}

WorldSession* PbotMgr::CreateBotSession(uint32 accountId)
{
    std::string accountName;
    AccountMgr::GetName(accountId, accountName);

    uint8 expansion = uint8(sWorld->getIntConfig(CONFIG_EXPANSION));
    uint32 build = 0;
    if (std::shared_ptr<Realm const> realm = sRealmList->GetCurrentRealm())
        build = realm->Build;

    // SS8.4 arg table. NEVER sWorld->AddSession() this session (SS8.6 rule 1).
    WorldSession* session = new WorldSession(
        accountId,                              // id
        std::string(accountName),               // name (rvalue)
        AcquireBotBattlenetAccount(),           // battlenetAccountId — see the note there; 0 made
                                                // every looted item fail a foreign key check
        std::string(PBOT_BNET_EMAIL),           // battlenetAccountEmail (rvalue)
        std::shared_ptr<WorldSocket>(),         // sock: the whole point — null socket (SS1)
        SEC_PLAYER,                             // sec
        expansion,                              // expansion (realm max, avoids race/class gating)
        0,                                      // mute_time
        std::string("Win"),                     // os (rvalue, never read without a client)
        Minutes(0),                             // timezoneOffset
        build,                                  // build (what the realm advertises)
        ClientBuild::VariantId{},               // clientBuildVariant (plain struct, all zero)
        LOCALE_enUS,                            // locale
        0,                                      // recruiter
        false);                                 // isARecruiter

    // ---- THE IDLE-KICK TRAP (fixed 2026-08-01 after a reproducible SIGSEGV) ------------------
    //
    // Map::Update (Map.cpp:662-672) calls session->Update() for EVERY player on the map. There is
    // no opt-out, so a bot session IS updated by the engine even though we never update it
    // ourselves — the forbidden-list rule "never call WorldSession::Update on a bot session" is
    // one we cannot actually enforce.
    //
    // The very first statement of WorldSession::Update is an idle kick:
    //     if (IsConnectionIdle() && !HasPermission(RBAC_PERM_IGNORE_IDLE_CONNECTION))
    //         m_Socket[CONNECTION_TYPE_REALM]->CloseSocket();
    // and that dereference is NOT null-guarded (every other socket use in that function is).
    // WorldSession's constructor leaves m_timeOutTime at 0, so IsConnectionIdle() is true
    // immediately — meaning the first map tick after a bot enters the world crashes the server on
    // a null socket. This is why bots have to be kept "not idle" rather than merely never updated.
    //
    // Write the sentinel directly rather than calling ResetTimeOutTime(): that helper grants only
    // SocketTimeOutTimeActive (60s) of protection, so it turns a permanent guarantee into a
    // 60-second fuse that has to be re-lit every tick — and the first version of this fix crashed
    // exactly 60 seconds after its last refresh. See PBOT_SESSION_NEVER_IDLE in pbot_common.h.
    session->m_timeOutTime = PBOT_SESSION_NEVER_IDLE;

    return session;
}

bool PbotMgr::PlaceInWorldNextToOwner(Player* owner, Player* bot)
{
    // DEVIATION FROM SS8.4 step 8 (tc-src wins): both Player::Create() (start map) and
    // Player::LoadFromDB() (saved map) leave the bot SetMap()'d to a map that is NOT the owner's, and
    // WorldObject::SetMap (Object.cpp:1151) ABORT()s if m_currMap is already a different map. The
    // in-world-safe primitive is ResetMap() (valid while !IsInWorld, which holds pre-AddPlayerToMap)
    // to detach, then SetMap() to the owner's live map. Also avoids the far-teleport ACK handshake we
    // can't drive without WorldSession::Update(). NOTE (smoke test): phasing/instance owners untested.
    Position spawnPos = owner->GetPosition();
    uint32 slot = uint32(_byOwner.count(owner->GetGUID()));
    float angle = PBOT_SPAWN_ANGLES[slot % PBOT_MAX_PER_OWNER];
    spawnPos.RelocateOffset(Position(std::cos(angle) * PBOT_SPAWN_SPREAD, std::sin(angle) * PBOT_SPAWN_SPREAD, 0.0f));

    bot->ResetMap();
    bot->Relocate(spawnPos);
    bot->SetMap(owner->GetMap());

    if (!bot->GetMap()->AddPlayerToMap(bot))
        return false;

    ObjectAccessor::AddObject(bot); // same call/order as CharacterHandler.cpp:1291

    // Same reason as in SpawnWorldBot: the login path is where a player's phase shift is set, and a
    // bot never walks that path. Skip it and the bot is blind — it can be attacked but can never
    // see anything to attack, gather or interact with.
    PhasingHandler::OnMapChange(bot);
    return true;
}

void PbotMgr::RegisterBotRecord(Player* owner, Player* bot, WorldSession* session, PbotClass cls)
{
    BotRecord rec;
    rec.OwnerGuid = owner->GetGUID();
    rec.BotGuid   = bot->GetGUID();
    rec.Session   = session;
    rec.Class     = cls;
    rec.Ai        = std::make_unique<PbotAI>(bot->GetGUID(), owner->GetGUID());

    _bots.emplace(bot->GetGUID(), std::move(rec));
    _byOwner.emplace(owner->GetGUID(), bot->GetGUID());
}

void PbotMgr::TeardownLiveRecord(std::unordered_map<ObjectGuid, BotRecord>::iterator it)
{
    BotRecord rec = std::move(it->second);
    ObjectGuid botGuid = it->first;
    ObjectGuid ownerGuid = rec.OwnerGuid;

    // Remove registry entries FIRST so nothing (OnUpdate, a re-entrant dismiss) can observe a
    // half-torn-down record while LogoutPlayer runs. NOTE (review finding #5): LogoutPlayer below
    // fires sScriptMgr->OnPlayerLogout for the BOT itself, which re-enters
    // PbotPlayerScript::OnLogout -> DismissAll(botPlayer). That is a harmless no-op (bots are never
    // keys in _byOwner), but do NOT "simplify" that hook into anything that could recurse here.
    _bots.erase(it);
    for (auto range = _byOwner.equal_range(ownerGuid); range.first != range.second; ++range.first)
    {
        if (range.first->second == botGuid)
        {
            _byOwner.erase(range.first);
            break;
        }
    }

    // Phase 5: drop out of the owner's party BEFORE logout. A logged-out member keeps its slot,
    // so without this a squad that is dismissed and re-spawned a few times would leave the owner
    // with a party full of offline bots and no room for the new ones.
    if (Player* botPlayer = ObjectAccessor::FindPlayer(botGuid))
        PbotParty::LeaveGroup(botPlayer);

    // SS8.5: LogoutPlayer(true) does the full correct teardown (pet/group/guild, SaveToDB,
    // CleanupsBeforeDelete, RemovePlayerFromMap(_player, true) which DELETES the Player, then
    // SetPlayer(nullptr)). It operates purely on the session's _player and needs no live socket.
    if (rec.Session)
    {
        rec.Session->LogoutPlayer(true);
        // The Player is already deleted; the WorldSession object itself is NOT (SS8.6 rule 5) —
        // delete it to avoid a per-dismiss leak. The dtor is null-socket-safe (_player is null now).
        delete rec.Session;
    }
    // rec.Ai (unique_ptr<PbotAI>) is freed as rec goes out of scope.
}

bool PbotMgr::DismissBot(ObjectGuid botGuid)
{
    // TEMPORARY removal (SS9 A.2): save + unload the live Player, but deliberately KEEP the character
    // row, the account, and the pbot_roster row so the bot reloads on the owner's next login.
    auto it = _bots.find(botGuid);
    if (it == _bots.end())
        return false;

    TeardownLiveRecord(it);
    return true;
}

bool PbotMgr::RetireBot(ObjectGuid botGuid)
{
    // PERMANENT removal (SS9 A.2). Works whether the bot is currently live or only rostered (offline).
    uint32 accountId = 0;

    auto it = _bots.find(botGuid);
    if (it != _bots.end())
    {
        accountId = it->second.Session ? it->second.Session->GetAccountId() : 0;
        TeardownLiveRecord(it); // save + unload the live Player first (same as DismissBot)
    }
    else
    {
        // Review finding CRITICAL: a reload for this bot may be in flight (submitted, not yet live).
        // Deleting the character row now would race the async LoadFromDB → orphaned live Player +
        // double account use. Defer: PbotLoader re-dispatches to RetireBot once the load resolves and
        // the pending flag is cleared, so the retire then takes a race-free path.
        if (PbotLoader::IsReloadPending(botGuid))
        {
            PbotLoader::RequestRetireOnLoad(botGuid);
            return true;
        }
        accountId = RosterAccountOf(botGuid); // offline: recover the bound account from the roster
    }

    if (!accountId)
        return false; // unknown bot: neither live nor rostered

    // Delete the character row + CharacterCache entry (DeleteFromDB deleteFinally=true does both),
    // drop the roster row, and release the account for reuse. RetireBot is the ONLY caller of
    // ReleaseBotAccount (A.4 risk 2), so a still-rostered account is never handed to a new character.
    Player::DeleteFromDB(botGuid, accountId, /*updateRealmChars*/ false, /*deleteFinally*/ true);
    RosterDelete(botGuid);
    ReleaseBotAccount(accountId);

    TC_LOG_INFO("scripts.bots", "PbotMgr: retired pbot {} (account {}).", botGuid.ToString(), accountId);
    return true;
}

void PbotMgr::RetireAll(Player* owner)
{
    if (!owner)
        return;

    // Every bot the owner has: rostered (live + offline) plus, defensively, any live bot missing a
    // roster row (shouldn't happen — SpawnBot always writes one).
    std::vector<ObjectGuid> guids;
    for (PbotRosterEntry const& e : QueryRosterForOwner(owner->GetGUID()))
        guids.push_back(e.BotGuid);
    for (ObjectGuid g : GetBotsOf(owner))
        if (std::find(guids.begin(), guids.end(), g) == guids.end())
            guids.push_back(g);

    for (ObjectGuid g : guids)
        RetireBot(g);
}

// ---- reload-path support (pbot_loader.cpp) --------------------------------------------------

bool PbotMgr::IsBotLive(ObjectGuid botGuid)
{
    return _bots.find(botGuid) != _bots.end();
}

// Puts a reloaded world bot back on the map it was saved on, at the position it was saved at.
//
// Mirrors the placement half of SpawnWorldBot rather than reusing it: that function creates a
// character, and this one already has one with its own history. The steps that matter are the same
// three that had to be got right for the spawn path — get a loaded map, add to it, then tell the
// phasing system, or the bot stands in a world it cannot see.
bool PbotMgr::PlaceWorldBotAtSavedPosition(Player* bot)
{
    Map* map = sMapMgr->CreateMap(bot->GetMapId(), bot);
    if (!map)
    {
        TC_LOG_ERROR("scripts.bots", "PbotMgr: cannot reload world bot {} — map {} would not load.",
            bot->GetName(), bot->GetMapId());
        return false;
    }

    bot->SetMap(map);
    bot->Relocate(bot->GetPosition());

    if (!bot->GetMap()->AddPlayerToMap(bot))
        return false;

    ObjectAccessor::AddObject(bot);
    PhasingHandler::OnMapChange(bot);
    return true;
}

// Registry bookkeeping for a world bot that came back from the database. The AI is rebuilt fresh —
// its runtime state (where it was walking, what it was fighting) is deliberately NOT persisted, and
// should not be: only what the CHARACTER earned survives a restart, which is what a player would
// expect. The home anchor is set to where the bot stands, so it resumes life where it left it.
bool PbotMgr::FinalizeReloadedWorldBot(Player* bot, WorldSession* session, uint8 unitClassId)
{
    PbotClass cls = PbotClass::Warrior;
    if (unitClassId >= uint8(PbotClass::Warrior) && unitClassId <= uint8(PbotClass::Evoker))
        cls = PbotClass(unitClassId);

    bot->SetPvP(true);
    bot->UpdatePvPState();

    BotRecord rec;
    rec.OwnerGuid = ObjectGuid::Empty;
    rec.BotGuid   = bot->GetGUID();
    rec.Session   = session;
    rec.Class     = cls;
    rec.Ai        = std::make_unique<PbotAI>(bot->GetGUID(), ObjectGuid::Empty);
    rec.Ai->SetHome(bot->GetPosition(), bot->GetMapId());

    _bots.emplace(bot->GetGUID(), std::move(rec));
    _byOwner.emplace(ObjectGuid::Empty, bot->GetGUID());

    TC_LOG_INFO("scripts.bots", "PbotMgr: reloaded WORLD bot {} (class {} level {}) on map {} — "
        "resuming where it left off.", bot->GetName(), uint32(unitClassId),
        uint32(bot->GetLevel()), bot->GetMapId());
    return true;
}

bool PbotMgr::FinalizeReloadedBot(ObjectGuid ownerGuid, Player* bot, WorldSession* session, uint8 unitClassId)
{
    if (IsBotLive(bot->GetGUID()))
        return false; // double-spawn guard (A.3): another reload already placed this bot

    // A world bot has no owner to stand next to, so it comes back exactly where it was saved. That
    // IS the persistence: the population picks up where it left off instead of being scattered anew
    // over the map every restart.
    if (ownerGuid.IsEmpty())
    {
        session->SetPlayer(bot);
        if (!PlaceWorldBotAtSavedPosition(bot))
            return false;

        return FinalizeReloadedWorldBot(bot, session, unitClassId);
    }

    Player* owner = ObjectAccessor::FindPlayer(ownerGuid);
    if (!owner || !owner->IsInWorld() || !owner->GetMap())
        return false; // owner left during the async window; caller tears down, roster row stays

    session->SetPlayer(bot);
    if (!PlaceInWorldNextToOwner(owner, bot))
        return false;

    // Reconstruct the PbotClass metadata from the engine class id. Since Phase 4B, PbotClass values
    // ARE the engine CLASS_* ids (pbot_mgr.h), so this is a range-checked cast rather than a switch
    // that had to be extended for every new class — the old switch silently reloaded a rogue or a
    // druid as a Warrior in the registry. Registry bookkeeping only (the AI resolves everything off
    // the live Player), so an out-of-range id still defaults harmlessly instead of failing the load.
    PbotClass cls = PbotClass::Warrior;
    if (unitClassId >= uint8(PbotClass::Warrior) && unitClassId <= uint8(PbotClass::Evoker))
        cls = PbotClass(unitClassId);
    else
        TC_LOG_WARN("scripts.bots", "PbotMgr: reloaded bot {} has unexpected class id {}; registry defaults to Warrior.",
            bot->GetGUID().ToString(), uint32(unitClassId));

    // Phase 5: rejoin the owner's party. The bot's own saved group was loaded by the login query
    // holder, but that only restores membership if the group still exists — after a dismiss cycle
    // or a server restart it usually does not, so we re-form it here. JoinOwnerGroup is a no-op
    // when the bot is already in the owner's group, so the common case costs one pointer compare.
    PbotParty::JoinOwnerGroup(owner, bot);

    RegisterBotRecord(owner, bot, session, cls);
    TC_LOG_INFO("scripts.bots", "PbotMgr: reloaded pbot {} (class {} level {}) for owner {}.",
        bot->GetGUID().ToString(), uint32(unitClassId), uint32(bot->GetLevel()), ownerGuid.ToString());
    return true;
}

std::vector<PbotRosterEntry> PbotMgr::QueryRosterForOwner(ObjectGuid ownerGuid)
{
    std::vector<PbotRosterEntry> rows;
    QueryResult result = WorldDatabase.Query(Trinity::StringFormat(
        "SELECT bot_guid, account_id, class FROM pbot_roster WHERE owner_guid = {}",
        ownerGuid.GetCounter()).c_str());
    if (!result)
        return rows;

    do
    {
        Field* fields = result->Fetch();
        PbotRosterEntry e;
        e.BotGuid   = ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt64());
        e.AccountId = fields[1].GetUInt32();
        e.ClassId   = fields[2].GetUInt8();
        rows.push_back(e);
    } while (result->NextRow());

    return rows;
}

std::vector<ObjectGuid> PbotMgr::QueryAllRosterOwners()
{
    std::vector<ObjectGuid> owners;
    QueryResult result = WorldDatabase.Query("SELECT DISTINCT owner_guid FROM pbot_roster");
    if (!result)
        return owners;

    do
    {
        owners.push_back(ObjectGuid::Create<HighGuid::Player>(result->Fetch()[0].GetUInt64()));
    } while (result->NextRow());

    return owners;
}

void PbotMgr::DeleteRosterRow(ObjectGuid botGuid)
{
    RosterDelete(botGuid);
}

void PbotMgr::DismissAll(Player* owner)
{
    if (!owner)
        return;

    // Snapshot the owner's bot guids: DismissBot mutates _byOwner, so we must not iterate it live.
    std::vector<ObjectGuid> guids;
    for (auto range = _byOwner.equal_range(owner->GetGUID()); range.first != range.second; ++range.first)
        guids.push_back(range.first->second);

    for (ObjectGuid g : guids)
        DismissBot(g);
}

void PbotMgr::DismissAllOnShutdown()
{
    // Snapshot every bot guid, then dismiss each — regardless of owner (SS8.5 step 4). This is the
    // only thing that saves/removes bot sessions at shutdown, since they were deliberately kept out
    // of World::m_sessions and nothing in the engine's shutdown path touches them.
    std::vector<ObjectGuid> guids;
    guids.reserve(_bots.size());
    for (auto const& kv : _bots)
        guids.push_back(kv.first);

    // Every bot is DISMISSED, which saves the character and leaves the roster row alone. Nothing is
    // deleted at shutdown any more.
    //
    // World bots used to be retired here — permanently deleted — on the grounds that they had no
    // roster row and would otherwise strand a character and an account on every restart. They have
    // a roster row now (see SpawnWorldBot), so the leak that justified deleting them is closed and
    // the deletion itself was the bug: it threw away every level, coin and quest the population had
    // accumulated, every single restart.
    for (ObjectGuid g : guids)
        DismissBot(g);
}

std::vector<ObjectGuid> PbotMgr::GetBotsOf(Player const* owner)
{
    std::vector<ObjectGuid> result;
    if (!owner)
        return result;

    for (auto range = _byOwner.equal_range(owner->GetGUID()); range.first != range.second; ++range.first)
        result.push_back(range.first->second);
    return result;
}

Player* PbotMgr::FindBot(ObjectGuid botGuid)
{
    if (Player* player = ObjectAccessor::FindPlayer(botGuid))
        return player;

    // ObjectAccessor drops a player for the duration of a far teleport. Falling back to the
    // session keeps a bot visible to commands (and to the AI) while it is in transit, instead of
    // it appearing to vanish — which is how a stuck teleport masqueraded as "0 bots queued".
    auto it = _bots.find(botGuid);
    if (it != _bots.end() && it->second.Session)
        return it->second.Session->GetPlayer();

    return nullptr;
}

// ---- Phase 4A chat command layer support -----------------------------------------------------

bool PbotMgr::OwnerHasBots(ObjectGuid ownerGuid)
{
    // One multimap probe, no allocation — this is on the hot path of every chat line on the realm.
    return _byOwner.find(ownerGuid) != _byOwner.end();
}

PbotAI* PbotMgr::GetBotAI(ObjectGuid botGuid)
{
    auto it = _bots.find(botGuid);
    return it != _bots.end() ? it->second.Ai.get() : nullptr;
}

ObjectGuid PbotMgr::GetOwnerOf(ObjectGuid botGuid)
{
    auto it = _bots.find(botGuid);
    return it != _bots.end() ? it->second.OwnerGuid : ObjectGuid::Empty;
}
