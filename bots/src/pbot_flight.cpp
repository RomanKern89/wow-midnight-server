/*
 * Companion Bots — flight paths. See pbot_flight.h for why bots need them.
 */

#include "pbot_flight.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "TaxiPathGraph.h"
#include "pbot_travel.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // How often a bot looks around for a flight point it has not seen before. Discovery scans every
    // node in the game, so it must not run on every tick of every bot: sixty bots against a few
    // thousand nodes ten times a second is real work to answer "no" with.
    constexpr uint32 DISCOVER_INTERVAL_MS = 10000;

    // The capital and hub nodes the engine grants a new character, lifted from
    // PlayerTaxi::InitTaxiNodesForLevel. Repeated here rather than called because that function
    // reaches them through a switch on race that names the classic races and stops — see
    // PbotFlight::SeedKnownNodes.
    constexpr uint32 ALLIANCE_START_NODES[] = { 2, 6, 26, 27, 49, 94, 100, 456, 457, 582, 589, 619, 620, 624 };
    constexpr uint32 HORDE_START_NODES[]    = { 11, 22, 23, 69, 82, 99, 384, 402, 460, 536, 537, 625, 631 };

    struct FlightState
    {
        uint32 SourceNode = 0;      // the platform being walked to; 0 when not trying to board
        uint32 BoardingSince = 0;   // world clock when that walk began, against BOARDING_BUDGET_MS
        uint32 NextDiscover = 0;    // world clock of the next look around
        uint32 NextBoard = 0;       // world clock before which no boarding is attempted again

        // Platforms this bot could not walk to. Not a cooldown — a verdict.
        //
        // Measured over an eight-hour soak: three bots produced 503 of the 535 abandoned walks
        // between them, each trying the SAME flight point about 170 times. Wrennaa walked at
        // Undercity's flight master, which is underground; Ulric at Thunder Bluff's, which is on
        // a mesa reached by elevators a bot cannot ride; Pellina at Razor Hill. Each spent the
        // entire soak on it, because giving up cleared the attempt and the next tick started an
        // identical one.
        //
        // The shape is exactly the accept-and-drop loop this session began with: two states that
        // each undo the other, with nothing remembering that the last attempt already failed.
        std::unordered_set<uint32> Unreachable;
    };

    std::unordered_map<ObjectGuid, FlightState> _state;

    FlightState& StateOf(ObjectGuid guid)
    {
        return _state[guid];
    }

    // Flight points grouped by the map they stand on, built once.
    //
    // sObjectMgr->GetNearestTaxiNode is the engine's answer to "which node is nearest", and it is
    // the wrong question here twice over. It walks all several thousand nodes, which is too much to
    // do per candidate destination per bot per tick. And it answers about the NEAREST node, when
    // what a bot needs is the nearest node IT KNOWS — the game lets a player fly to any discovered
    // point, and a player standing in Elwynn with business in Redridge flies to the nearest thing
    // on their own map and rides the rest. Restricting bots to the single nearest node would ground
    // them almost always, since the nearest node to an out-of-the-way quest-taker is exactly the
    // sort of small outpost nobody has discovered yet.
    //
    // The membership test (faction, in the taxi network at all) is copied from GetNearestTaxiNode
    // so the two agree about which nodes exist; what differs is only the choice made among them.
    std::unordered_map<uint32, std::vector<TaxiNodesEntry const*>> _nodesByMap;
    bool _nodeIndexBuilt = false;

    void BuildNodeIndex()
    {
        if (_nodeIndexBuilt)
            return;
        _nodeIndexBuilt = true;

        uint32 kept = 0;
        for (TaxiNodesEntry const* node : sTaxiNodesStore)
        {
            if (!node || node->GetFlags().HasFlag(TaxiNodeFlags::IgnoreForFindNearest))
                continue;

            // Not part of the taxi network at all — the same bitmask GetNearestTaxiNode consults.
            uint32 const field = uint32((node->ID - 1) / (sizeof(TaxiMask::value_type) * 8));
            TaxiMask::value_type const submask =
                TaxiMask::value_type(1 << ((node->ID - 1) % (sizeof(TaxiMask::value_type) * 8)));
            if ((sTaxiNodesMask[field] & submask) == 0)
                continue;

            _nodesByMap[node->ContinentID].push_back(node);
            ++kept;
        }

        TC_LOG_INFO("scripts.bots", "pbot flight: indexed {} flight points across {} maps",
            kept, uint32(_nodesByMap.size()));
    }

    bool VisibleTo(TaxiNodesEntry const* node, uint32 team)
    {
        if (team == HORDE)
            return node->GetFlags().HasFlag(TaxiNodeFlags::ShowOnHordeMap);
        if (team == ALLIANCE)
            return node->GetFlags().HasFlag(TaxiNodeFlags::ShowOnAllianceMap);
        return false;
    }

    // The nearest flight point on this map that the bot has actually discovered — and, for the
    // departure end, has not already proved it cannot walk to.
    TaxiNodesEntry const* NearestKnownNode(Player* bot, uint32 mapId, float x, float y,
                                           std::unordered_set<uint32> const* skip = nullptr)
    {
        BuildNodeIndex();

        auto const itr = _nodesByMap.find(mapId);
        if (itr == _nodesByMap.end())
            return nullptr;

        TaxiNodesEntry const* best = nullptr;
        float bestDist = 0.0f;

        for (TaxiNodesEntry const* node : itr->second)
        {
            if (!VisibleTo(node, uint32(bot->GetTeam())) || !bot->m_taxi.IsTaximaskNodeKnown(node->ID))
                continue;

            if (skip && skip->count(node->ID))
                continue;

            float const dx = node->Pos.X - x;
            float const dy = node->Pos.Y - y;
            float const d = dx * dx + dy * dy;

            if (!best || d < bestDist)
            {
                best = node;
                bestDist = d;
            }
        }

        return best;
    }

    Creature* FindFlightMaster(Player* bot)
    {
        Creature* found = nullptr;
        auto check = [bot](Creature* c)
        {
            return c && c->IsAlive() && c->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER)
                && !c->IsHostileTo(bot) && bot->IsWithinDist(c, PbotFlight::BOARDING_RANGE);
        };

        Trinity::CreatureLastSearcher<decltype(check)> searcher(bot, found, check);
        Cell::VisitAllObjects(bot, searcher, PbotFlight::BOARDING_RANGE);
        return found;
    }

    // Flat distance only. Node heights and quest-marker heights are not measured the same way —
    // quest_poi records Z as zero for a great many points — and a 3D comparison against a zero picks
    // a node by how low it sits rather than by how near it is.
    float Flat(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Everything the two callers need to agree on. They must ask the same question in the same way:
    // "can I fly there" deciding to accept a destination, and "should I fly there" on arrival at
    // the platform, are the same question, and the whole reason this session started was two halves
    // of quest handling answering theirs differently.
    struct Leg
    {
        uint32 SrcNode = 0;
        uint32 DstNode = 0;
        TaxiNodesEntry const* SrcEntry = nullptr;
        TaxiNodesEntry const* DstEntry = nullptr;
        float WalkNow = 0.0f;
        float ToPlatform = 0.0f;
        float FromLanding = 0.0f;
    };

    bool PlanFlight(Player* bot, Position const& dest, Leg& leg,
                    std::unordered_set<uint32> const& unreachable)
    {
        uint32 const mapId = bot->GetMapId();

        // Both ends are the nearest point the bot KNOWS, not the nearest point that exists. A bot
        // that has never been to the little outpost beside the quest-taker flies to the town it has
        // been to and rides in from there, which is what the saving test below is for.
        //
        // The skip list applies only to the departure end. It records platforms this bot could not
        // WALK to, and it lands at the far one rather than walking to it.
        leg.DstEntry = NearestKnownNode(bot, mapId, dest.GetPositionX(), dest.GetPositionY());
        leg.SrcEntry = NearestKnownNode(bot, mapId, bot->GetPositionX(), bot->GetPositionY(),
            &unreachable);

        if (!leg.DstEntry || !leg.SrcEntry || leg.DstEntry == leg.SrcEntry)
            return false;

        leg.DstNode = leg.DstEntry->ID;
        leg.SrcNode = leg.SrcEntry->ID;

        // Is the whole exercise worth it? Not "is the destination far" — the honest comparison is
        // the walk as it stands against the walk to the platform plus the walk from wherever the
        // flight lands. A flight that drops the bot on the far side of the zone it started in has
        // saved nothing and cost it a trip to the flight master.
        leg.WalkNow     = Flat(bot->GetPositionX(), bot->GetPositionY(), dest.GetPositionX(), dest.GetPositionY());
        leg.ToPlatform  = Flat(bot->GetPositionX(), bot->GetPositionY(), leg.SrcEntry->Pos.X, leg.SrcEntry->Pos.Y);
        leg.FromLanding = Flat(dest.GetPositionX(), dest.GetPositionY(), leg.DstEntry->Pos.X, leg.DstEntry->Pos.Y);

        if (leg.ToPlatform > PbotFlight::MAX_PLATFORM_WALK)
            return false;

        return leg.WalkNow - (leg.ToPlatform + leg.FromLanding) >= PbotFlight::MIN_SAVING;
    }
}

void PbotFlight::Forget(ObjectGuid guid)
{
    _state.erase(guid);
}

void PbotFlight::Discover(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->IsInFlight())
        return;

    FlightState& state = StateOf(bot->GetGUID());
    uint32 const now = GameTime::GetGameTimeMS();
    if (state.NextDiscover && now < state.NextDiscover)
        return;

    state.NextDiscover = now + DISCOVER_INTERVAL_MS;

    // The engine's own nearest-node search: it already applies the faction filter and skips nodes
    // outside the taxi network, which is exactly the set a player is able to discover.
    uint32 const nearest = sObjectMgr->GetNearestTaxiNode(bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), bot->GetMapId(), bot->GetTeam());
    if (!nearest)
        return;

    TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(nearest);
    if (!node)
        return;

    if (Flat(bot->GetPositionX(), bot->GetPositionY(), node->Pos.X, node->Pos.Y) > DISCOVER_RANGE)
        return;

    if (bot->m_taxi.SetTaximaskNode(nearest))
        TC_LOG_INFO("scripts.bots", "pbot flight: {} discovered flight point {} on map {}",
            bot->GetName(), nearest, bot->GetMapId());
}

void PbotFlight::SeedKnownNodes(Player* bot)
{
    if (!bot)
        return;

    // The engine's baseline first — it also handles the level-gated hub and a Death Knight's whole
    // old-world mask, neither of which is worth restating here.
    bot->InitTaxiNodesForLevel();

    // Then the capitals, unconditionally by team.
    //
    // InitTaxiNodesForLevel reaches these through `switch (race)` with the classic races written out
    // by name. Every race added since — the allied races, Dracthyr, Earthen — falls through it and
    // is granted nothing. Measured on the live realm before this: of sixty bots, fifty-eight knew
    // exactly one flight point. A Void Elf is not meant to be more stranded than a Night Elf; the
    // list simply stopped being maintained.
    //
    // For a classic race every one of these is already set and this loop does nothing at all.
    uint32 granted = 0;
    if (bot->GetTeam() == ALLIANCE)
    {
        for (uint32 node : ALLIANCE_START_NODES)
            if (bot->m_taxi.SetTaximaskNode(node))
                ++granted;
    }
    else
    {
        for (uint32 node : HORDE_START_NODES)
            if (bot->m_taxi.SetTaximaskNode(node))
                ++granted;
    }

    if (granted)
        TC_LOG_INFO("scripts.bots", "pbot flight: {} starts with {} capital flight points its race "
            "was never granted", bot->GetName(), granted);
}

bool PbotFlight::TryTravel(Player* bot, Position const& dest)
{
    if (!bot || !bot->IsInWorld())
        return false;

    // Airborne. The engine owns this bot until it lands, and anything the decision ladder does to it
    // meanwhile — a MovePoint, a MotionMaster::Clear — would tear the flight down in mid-air.
    if (bot->IsInFlight())
        return true;

    FlightState& state = StateOf(bot->GetGUID());
    auto abandon = [&state]() -> bool
    {
        state.SourceNode = 0;
        state.BoardingSince = 0;
        return false;
    };

    // ActivateTaxiPathTo refuses in combat anyway; bailing here keeps the walk to the platform from
    // being charged for time the bot spent fighting.
    if (bot->IsInCombat())
        return abandon();

    Leg leg;
    if (!PlanFlight(bot, dest, leg, state.Unreachable))
        return abandon();

    uint32 const now = GameTime::GetGameTimeMS();

    // A refusal a moment ago is still a refusal now — an empty purse, a shapeshift, a faction that
    // will not serve this bot do not change between ticks. Checked BEFORE the walk below, or a bot
    // that cannot board keeps hiking to the platform to be told so again.
    if (state.NextBoard && now < state.NextBoard)
        return abandon();

    // Walk to the flight master. A bot cannot board from across the zone any more than a player can.
    if (leg.ToPlatform > BOARDING_RANGE)
    {
        if (state.SourceNode != leg.SrcNode)
        {
            state.SourceNode = leg.SrcNode;
            state.BoardingSince = now;
        }

        // Platforms can be unreachable — on a roof the navmesh does not cover, across water, inside
        // a phase. Without a budget a bot commits its whole day to a set of stairs it cannot climb.
        // This is the failure that had bots standing beside anvils for ten minutes at a time, and it
        // is why that budget exists.
        if (now - state.BoardingSince > BOARDING_BUDGET_MS)
        {
            // Written down, not merely abandoned. Abandoning alone is what let three bots spend an
            // entire eight-hour soak walking at the same three flight masters, ~170 attempts each.
            state.Unreachable.insert(leg.SrcNode);

            TC_LOG_INFO("scripts.bots", "pbot flight: {} cannot walk to flight point {} — writing it "
                "off and using another", bot->GetName(), leg.SrcNode);
            return abandon();
        }

        Position platform(leg.SrcEntry->Pos.X, leg.SrcEntry->Pos.Y, leg.SrcEntry->Pos.Z, 0.0f);
        if (PbotTravel::StepGround(bot, platform))
            return true;

        // Ground travel reports done while the bot is still not at the platform: there is no path
        // to it. Walking to the real destination is the better use of the next three minutes.
        return abandon();
    }

    std::vector<uint32> route;
    if (TaxiPathGraph::GetCompleteNodeRoute(leg.SrcEntry, leg.DstEntry, bot, route) < 2)
        return abandon();

    // Can it afford the ticket? ActivateTaxiPathTo checks this itself and refuses, but finding out
    // by being refused is how one broke bot wrote seven thousand log lines in twenty minutes.
    //
    // Deliberately conservative: the engine applies a reputation discount when a flight master is
    // handed over, and this sum does not. The worst that costs is a bot walking somewhere it could
    // just barely have afforded to fly.
    uint64 fare = 0;
    for (size_t i = 1; i < route.size(); ++i)
    {
        uint32 path = 0, cost = 0;
        sObjectMgr->GetTaxiPath(route[i - 1], route[i], path, cost);
        fare += cost;
    }

    if (bot->GetMoney() < fare)
    {
        state.NextBoard = now + BOARD_RETRY_MS;
        return abandon();
    }

    // Hand over the flight master when one is standing here, the way the client-driven path does:
    // that is what applies the reputation discount and records who the bot flew with.
    Creature* master = FindFlightMaster(bot);

    // Do not read the return value as success. Under InstantFlightPaths the engine teleports the
    // player to the last node and returns FALSE — the flight happened and the answer says it did
    // not. That setting is off on this realm, so the branch is not live here, but a return value
    // that means the opposite of itself under a config nobody remembers setting is exactly what
    // gets misread a year later. Ask the bot where it is instead of asking the function how it did.
    bool const claimed = bot->ActivateTaxiPathTo(route, master);
    bool const airborne = bot->IsInFlight();
    bool const teleported = !airborne
        && Flat(bot->GetPositionX(), bot->GetPositionY(), leg.DstEntry->Pos.X, leg.DstEntry->Pos.Y) <= BOARDING_RANGE;

    state.SourceNode = 0;
    state.BoardingSince = 0;

    if (!airborne && !teleported)
    {
        state.NextBoard = now + BOARD_RETRY_MS;
        TC_LOG_INFO("scripts.bots", "pbot flight: {} could not board at {} for {} (engine said {}) "
            "— not asking again for {} minutes", bot->GetName(), leg.SrcNode, leg.DstNode, claimed,
            BOARD_RETRY_MS / 60000);
        return false;
    }

    TC_LOG_INFO("scripts.bots", "pbot flight: {} flew from {} to {} over {} hops, saving {:.0f} "
        "yards of walking", bot->GetName(), leg.SrcNode, leg.DstNode, uint32(route.size() - 1),
        leg.WalkNow - leg.FromLanding);
    return true;
}

bool PbotFlight::CanReach(Player* bot, Position const& dest)
{
    if (!bot || !bot->IsInWorld())
        return false;

    Leg leg;
    return PlanFlight(bot, dest, leg, StateOf(bot->GetGUID()).Unreachable);
}
