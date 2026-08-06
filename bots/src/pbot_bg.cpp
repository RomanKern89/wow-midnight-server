/*
 * Companion Bots — Phase 7 battleground participation.
 * See pbot_bg.h for which client-side decisions this replaces and which it deliberately does not.
 */

#include "pbot_bg.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Cell.h"
#include "CellImpl.h"
#include "DB2Stores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "ObjectMgr.h"      // WorldSafeLocsEntry (team start positions)
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
    // A queue slot that was never filled has a zero battlemaster list id.
    bool SlotInUse(BattlegroundQueueTypeId const& queueId)
    {
        return queueId.BattlemasterListId != 0;
    }

    // --- Warsong Gulch objectives -------------------------------------------------------------

    // Flags are found by TYPE AND FACTION, never by entry or map id.
    //
    // The entries this used to look for (179830/179831, type 24 FLAGSTAND) are the classic ones and
    // are not what this build spawns — which is why a bot standing in its own base reported
    // "ownFlag NOT FOUND". The modern Warsong Gulch script (battleground_warsong_gulch.cpp,
    // registered for map 2106) spawns 227741 "Alliance flag in base" and 227740 "Horde flag in
    // base", both GAMEOBJECT_TYPE_NEW_FLAG (36).
    //
    // Hardcoding those two would still be wrong, because there is NO battlemaster list id that
    // leads to map 2106 alone — the only queue that reaches the scripted Warsong Gulch is the
    // random-battleground pool (bm 32), which also serves Twin Peaks, Deephaul Ravine and seven
    // others. A bot therefore cannot know in advance which battleground it will land in. Asking
    // "which nearby object is a flag, and is it mine or theirs" answers that for all of them.
    constexpr uint8 GO_TYPE_NEW_FLAG = 36;   // GAMEOBJECT_TYPE_NEW_FLAG

    enum class FlagSide : uint8 { Friendly, Hostile, Unknown };

    // A flag carries the faction of the team that owns it, so the one to CAPTURE is the hostile
    // one and the one to run home to is the friendly one.
    FlagSide SideOfFlag(Player const* bot, GameObject const* go)
    {
        FactionTemplateEntry const* goFaction  = sFactionTemplateStore.LookupEntry(go->GetFaction());
        FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
        if (!goFaction || !botFaction)
            return FlagSide::Unknown;

        if (botFaction->IsHostileTo(goFaction))
            return FlagSide::Hostile;
        if (botFaction->IsFriendlyTo(goFaction))
            return FlagSide::Friendly;
        return FlagSide::Unknown;
    }

    // Auras a player has while carrying the opposing team's flag.
    constexpr uint32 AURA_SILVERWING_FLAG = 23333;   // carried by Horde (took the Alliance flag)
    constexpr uint32 AURA_WARSONG_FLAG    = 23335;   // carried by Alliance (took the Horde flag)

    // Map 489 is the LEGACY Warsong Gulch and has no script bound to it (battleground_scripts has
    // no row for it), so nothing spawns there — no flags, no gates, no objects at all. Kept here
    // only as a warning: `bm 2` queues for exactly that empty map, which is where bots used to
    // arrive and find nothing to do.
    constexpr uint32 WSG_LEGACY_UNSCRIPTED_MAP_ID = 489;

    // Warsong Gulch is roughly 350 yards end to end and a bot starts at its OWN base, so a 200y
    // sweep never reached the enemy flag — measured: 20 bots, 4 minutes, zero flag grabs. The
    // radius now covers the map, and the cost is paid back by searching rarely and caching the
    // result (see PbotBgState).
    constexpr float  FLAG_SEARCH_RANGE   = 400.0f;
    constexpr uint32 FLAG_SEARCH_CD_MS   = 5000;
    constexpr float  FLAG_INTERACT_RANGE = 4.0f;

    // "Close enough to the enemy base that if there were a flag here we would have seen it."
    // Reaching this without finding one means this battleground has no flags to take.
    constexpr float  ARRIVED_AT_BASE_RANGE = 60.0f;
    constexpr uint32 BG_POINT_ID         = 0xB61;

    class FlagBySideCheck
    {
    public:
        FlagBySideCheck(Player const* bot, FlagSide side, float range) : _bot(bot), _side(side), _range(range) { }

        bool operator()(GameObject* go) const
        {
            if (!go || !go->isSpawned() || go->GetGoType() != GameobjectTypes(GO_TYPE_NEW_FLAG))
                return false;

            if (!_bot->IsWithinDist(go, _range))
                return false;

            return SideOfFlag(_bot, go) == _side;
        }

    private:
        Player const* _bot;
        FlagSide _side;
        float _range;
    };

    GameObject* FindFlag(Player* bot, FlagSide side)
    {
        GameObject* found = nullptr;
        FlagBySideCheck check(bot, side, FLAG_SEARCH_RANGE);
        Trinity::GameObjectLastSearcher<FlagBySideCheck> searcher(bot, found, check);
        Cell::VisitAllObjects(bot, searcher, FLAG_SEARCH_RANGE);
        return found;
    }

    bool IsCarryingFlag(Player const* bot)
    {
        return bot->HasAura(AURA_SILVERWING_FLAG) || bot->HasAura(AURA_WARSONG_FLAG);
    }

    // Longest single hop we will ask the pathfinder for. Asking for the enemy base in one go — some
    // 590 yards away — produced no usable path at all: bots advanced once, then stood on the same
    // coordinates for three minutes. Travelling in bounded steps keeps every request inside what
    // the navmesh will actually answer, and the next step is issued as soon as the bot idles.
    constexpr float MAX_HOP = 50.0f;

    // Walks toward a point, re-issuing only when the motion generator has gone idle so the path is
    // not restarted every tick. Far destinations are approached one hop at a time.
    void MoveToward(Player* bot, Position const& dest)
    {
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
            return;

        float x = dest.GetPositionX();
        float y = dest.GetPositionY();
        float z = dest.GetPositionZ();

        float const distance = bot->GetExactDist2d(x, y);
        if (distance > MAX_HOP)
        {
            // Step MAX_HOP yards along the straight line toward the destination.
            float const ratio = MAX_HOP / distance;
            x = bot->GetPositionX() + (x - bot->GetPositionX()) * ratio;
            y = bot->GetPositionY() + (y - bot->GetPositionY()) * ratio;
            z = bot->GetPositionZ();
        }

        bot->UpdateAllowedPositionZ(x, y, z);
        bot->GetMotionMaster()->MovePoint(BG_POINT_ID, x, y, z);
    }

    // Collects every GameObject entry near the bot. Used only by the diagnostic: the flag ids taken
    // from gameobject_template turned out not to be what this build spawns in a live match, and
    // guessing further ids would repeat the mistake.
    class NearbyGameObjectCollector
    {
    public:
        NearbyGameObjectCollector(Player const* bot, float range, std::vector<std::pair<uint32, float>>& out)
            : _bot(bot), _range(range), _out(out) { }

        // Always returns false: the searcher stores nothing, this check does the collecting.
        bool operator()(GameObject* go)
        {
            if (go && go->isSpawned() && _bot->IsWithinDist(go, _range))
                _out.emplace_back(go->GetEntry(), _bot->GetExactDist(go));
            return false;
        }

    private:
        Player const* _bot;
        float _range;
        std::vector<std::pair<uint32, float>>& _out;
    };
}

bool PbotBG::Queue(Player* bot, uint16 battlemasterListId, std::string& err)
{
    if (!bot || !bot->IsInWorld())
    {
        err = "bot is not in the world";
        return false;
    }

    if (bot->InBattleground())
    {
        err = "already in a battleground";
        return false;
    }

    if (!bot->HasFreeBattlegroundQueueId())
    {
        err = "no free battleground queue slot";
        return false;
    }

    BattlegroundTypeId const bgTypeId = BattlegroundTypeId(battlemasterListId);
    BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(bgTypeId);
    if (!bgTemplate || bgTemplate->MapIDs.empty())
    {
        err = "unknown battleground";
        return false;
    }

    uint32 const mapId = uint32(bgTemplate->MapIDs.front());

    // Brackets are per level range; a bot outside every bracket for this map cannot queue at all,
    // which is worth reporting rather than silently doing nothing.
    PVPDifficultyEntry const* bracketEntry = DB2Manager::GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
    if (!bracketEntry)
    {
        err = "no bracket for this level";
        return false;
    }

    BattlegroundQueueTypeId const queueId =
        BattlegroundMgr::BGQueueTypeId(battlemasterListId, BattlegroundQueueIdType::Battleground,
                                       /*rated*/ false, /*teamSize*/ 0);

    BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueId);

    // nullptr group = queued solo, exactly as a player clicking "join as individual" does.
    GroupQueueInfo* ginfo = queue.AddGroup(bot, nullptr, bot->GetTeam(), bracketEntry,
                                           /*isPremade*/ false, /*ArenaRating*/ 0, /*MatchmakerRating*/ 0);
    if (!ginfo)
    {
        err = "queue refused the bot";
        return false;
    }

    bot->AddBattlegroundQueueId(queueId);

    // Without this the bot sits in the queue forever. BattlegroundQueue does not poll itself —
    // it is only processed when someone schedules an update for that bracket, which the player
    // join handler does immediately after AddGroup (BattleGroundHandler.cpp:251). Omitting it was
    // why 20 correctly-queued bots produced no match at all.
    sBattlegroundMgr->ScheduleQueueUpdate(0, queueId, bracketEntry->GetBracketId());

    TC_LOG_INFO("scripts.bots", "PbotBG: bot {} queued for battleground {} (map {}, bracket {}).",
        bot->GetName(), uint32(battlemasterListId), mapId, uint32(bracketEntry->GetBracketId()));
    return true;
}

bool PbotBG::IsQueued(Player const* bot)
{
    if (!bot)
        return false;

    for (uint32 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (SlotInUse(bot->GetBattlegroundQueueTypeId(i)))
            return true;

    return false;
}

bool PbotBG::TickInBattleground(Player* bot, PbotBgState& state, uint32 diff)
{
    // Any battleground, not just one map id: the queue that reaches the scripted flag battlegrounds
    // is a random pool, so the bot finds out where it is by looking, not by being told.
    if (!bot || !bot->IsAlive() || !bot->InBattleground())
    {
        state.HasTarget = false;
        return false;
    }

    // ★ Being "in a battleground" is not the same as STANDING in one. A bot that was invited but
    // whose port never completed still answers InBattleground(), and every objective position this
    // function produces belongs to the battleground's map. Measured live: ten such bots walked east
    // across Elwynn Forest for fourteen minutes — nearly six thousand yards — heading for a base
    // that exists on another map entirely. Same family of bug as the per-map home anchor.
    Battleground* const bg = bot->GetBattleground();
    if (!bg || bot->GetMapId() != bg->GetMapId())
    {
        state.HasTarget = false;
        return false;   // let the ordinary combat/wander path deal with wherever it actually is
    }

    // Capture the ENEMY's flag; your own flag marks the base you run it back to.
    bool const carrying = IsCarryingFlag(bot);
    FlagSide const wantedSide = carrying ? FlagSide::Friendly : FlagSide::Hostile;

    if (state.SearchCooldownMs > diff)
        state.SearchCooldownMs -= diff;
    else
        state.SearchCooldownMs = 0;

    // Refresh the target occasionally. Between refreshes the bot just walks to the remembered
    // position, which is what keeps a map-wide sweep affordable across a whole match of bots.
    if (!state.SearchCooldownMs)
    {
        state.SearchCooldownMs = FLAG_SEARCH_CD_MS;

        if (GameObject* flag = FindFlag(bot, wantedSide))
        {
            state.Target = flag->GetPosition();
            state.HasTarget = true;

            if (!carrying && bot->IsWithinDist(flag, FLAG_INTERACT_RANGE))
            {
                // Same entry point a right-click reaches; the battleground script owns the pickup.
                flag->Use(bot);
                TC_LOG_INFO("scripts.bots", "PbotBG: bot {} grabbed the enemy flag.", bot->GetName());
                state.HasTarget = false;
                return true;
            }
        }
        else
        {
            // Flag not standing anywhere in range — either an enemy is carrying it, or (far more
            // often at the start of a match) it is simply too far away to see: the two bases sit
            // about 590 yards apart, well beyond any usable search radius.
            state.HasTarget = false;

            // So walk toward the enemy base and search again from there. The battleground knows
            // where that is, which beats hardcoding coordinates per map.
            // ...but only while we are still far from it. Once the bot has walked to the enemy base
            // and STILL sees no flag, this is simply not a capture-the-flag battleground — the
            // random pool also serves cart and resource maps (Deephaul Ravine, where two bots stood
            // motionless for twelve minutes waiting for a flag that does not exist there). Giving up
            // hands the tick back to the combat path, which is the right thing to do in a fight.
            if (!carrying)
            {
                TeamId const enemyTeamId = bot->GetBGTeam() == ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
                if (WorldSafeLocsEntry const* enemyBase = bg->GetTeamStartPosition(enemyTeamId))
                {
                    if (bot->GetExactDist(&enemyBase->Loc) > ARRIVED_AT_BASE_RANGE)
                    {
                        state.Target = enemyBase->Loc;
                        state.HasTarget = true;
                    }
                }
            }
        }
    }

    if (!state.HasTarget)
        return false;   // nothing objective to do; the combat path takes over

    // Close enough to interact, but the search is still on cooldown — wait here rather than
    // wandering off; the next refresh will pick the flag up.
    if (!carrying && bot->GetExactDist(&state.Target) <= FLAG_INTERACT_RANGE)
        return true;

    MoveToward(bot, state.Target);
    return true;
}

std::string PbotBG::DescribeBattlegroundState(Player* bot)
{
    if (!bot)
        return "no bot";

    if (!bot->InBattleground())
        return Trinity::StringFormat("map {} — not in a battleground", bot->GetMapId());

    Battleground* bg = bot->GetBattleground();
    std::string const status = bg
        ? Trinity::StringFormat("bgStatus {}", uint32(bg->GetStatus()))
        : std::string("bg NULL");

    GameObject* enemyFlag = FindFlag(bot, FlagSide::Hostile);
    GameObject* ownFlag   = FindFlag(bot, FlagSide::Friendly);

    // What GameObjects are actually here? The configured flag entries were not found even at a
    // bot's own base, so list what the battleground really spawned nearby.
    std::vector<std::pair<uint32, float>> nearby;
    std::string nearbyText;
    {
        GameObject* ignored = nullptr;   // the collector gathers; the searcher's own result is unused
        NearbyGameObjectCollector collector(bot, 60.0f, nearby);
        Trinity::GameObjectLastSearcher<NearbyGameObjectCollector> searcher(bot, ignored, collector);
        Cell::VisitAllObjects(bot, searcher, 60.0f);

        std::sort(nearby.begin(), nearby.end(),
            [](auto const& a, auto const& b) { return a.second < b.second; });

        uint32 shown = 0;
        for (auto const& [entry, dist] : nearby)
        {
            if (shown++ >= 6)
                break;
            nearbyText += Trinity::StringFormat("{}@{:.0f}y ", entry, dist);
        }
        if (nearbyText.empty())
            nearbyText = "none";
    }

    return Trinity::StringFormat("map {} pos({:.0f} {:.0f} {:.0f}) team {} {} carrying {} | enemyFlag {} | ownFlag {} | nearbyGO: {}",
        bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
        bot->GetBGTeam() == ALLIANCE ? "A" : "H", status, IsCarryingFlag(bot) ? "YES" : "no",
        enemyFlag ? Trinity::StringFormat("{:.0f}y", bot->GetExactDist(enemyFlag)) : std::string("NOT FOUND"),
        ownFlag   ? Trinity::StringFormat("{:.0f}y", bot->GetExactDist(ownFlag))   : std::string("NOT FOUND"),
        nearbyText);
}

bool PbotBG::TickQueue(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->InBattleground())
        return false;

    for (uint32 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const queueId = bot->GetBattlegroundQueueTypeId(i);
        if (!SlotInUse(queueId))
            continue;

        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueId);

        // Copied by value on purpose: RemovePlayer below can destroy the GroupQueueInfo, and the
        // engine's own handler takes the same precaution for the same reason.
        GroupQueueInfo ginfo;
        if (!queue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo))
            continue;

        if (!ginfo.IsInvitedToBGInstanceGUID)
            continue;   // still waiting for a match

        BattlegroundTypeId const bgTypeId = BattlegroundTypeId(queueId.BattlemasterListId);
        Battleground* bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
        if (!bg)
            continue;

        if (!bot->IsInvitedForBattlegroundQueueType(queueId))
            continue;

        // From here down this mirrors WorldSession::HandleBattleFieldPortOpcode's accept path.
        if (!bot->InBattleground())
            bot->SetBattlegroundEntryPoint();

        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }

        queue.RemovePlayer(bot->GetGUID(), false);

        bot->SetBattlegroundId(bg->GetInstanceID(), bg->GetTypeID(), queueId);
        bot->SetBGTeam(ginfo.Team);

        // Starts the teleport into the instance. The bot is added to the battleground when the
        // worldport is acknowledged — which PbotWorldScript does on the next tick.
        BattlegroundMgr::SendToBattleground(bot, bg);

        TC_LOG_INFO("scripts.bots", "PbotBG: bot {} entering battleground instance {} (type {}).",
            bot->GetName(), bg->GetInstanceID(), uint32(bg->GetTypeID()));
        return true;
    }

    return false;
}
