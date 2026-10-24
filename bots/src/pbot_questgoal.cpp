/*
 * Companion Bots — quest objective location. See pbot_questgoal.h.
 */

#include "pbot_questgoal.h"

#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "DB2Structure.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "pbot_flight.h"   // a turn-in past walking range is still a destination if it can be flown to

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Where the creatures named by kill objectives actually stand.
    //
    // Find() used to take its destination from QuestPOI data only. Many quests have none, and for
    // those the bot had no goal at all: it fell through to "fight anything fair" and ground random
    // wildlife. Measured 2026-10-22 on the live realm: 973 unfinished kill objectives, 29 with any
    // progress, and one bot with 279 kills of which ZERO were its quest targets.
    struct KillSpot
    {
        uint32 MapId = 0;
        Position Pos;
    };

    std::unordered_map<uint32, std::vector<KillSpot>> _killSpots;   // creature entry -> spawns

    // Quest ITEM objectives: which creatures drop the item. The objective names the item, not the
    // creature, so a bot could never resolve one to a hunt - measured 2026-10-23: 313 item-only
    // quests, 2353 units outstanding, 102 of 224 distinct items dropped by creatures, and the bots
    // walked past every one of them.
    std::unordered_map<uint32, std::vector<uint32>> _itemDroppers;   // item id -> creature entries
    bool _itemDroppersLoaded = false;

    void PreloadItemDroppers()
    {
        if (_itemDroppersLoaded)
            return;

        _itemDroppersLoaded = true;   // set first: a failed query must not retry forever

        QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT o.ObjectID, clt.Entry FROM quest_objectives o "
            "JOIN creature_loot_template clt ON clt.Item = o.ObjectID "
            "WHERE o.Type = 1 AND o.ObjectID > 0");

        if (!result)
        {
            TC_LOG_INFO("scripts.bots", "pbot: no item objective resolves to a creature drop");
            return;
        }

        uint32 rows = 0;
        do
        {
            Field* f = result->Fetch();
            uint32 const item = f[0].GetUInt32();
            uint32 const entry = f[1].GetUInt32();

            std::vector<uint32>& droppers = _itemDroppers[item];
            if (std::find(droppers.begin(), droppers.end(), entry) == droppers.end())
                droppers.push_back(entry);
            ++rows;
        }
        while (result->NextRow());

        TC_LOG_INFO("scripts.bots", "pbot: item droppers loaded - {} items, {} entry links",
            uint32(_itemDroppers.size()), rows);
    }

    // Destinations a bot could not walk to, per map. Bounded: an unbounded list on a long soak would
    // grow with every stuck moment and turn the lookup into a scan of thousands of points.
    struct UnreachableSpot
    {
        uint32 MapId = 0;
        Position Pos;
    };
    std::vector<UnreachableSpot> _unreachableObjectives;
    constexpr size_t MAX_UNREACHABLE_SPOTS = 4000;
    constexpr float UNREACHABLE_RADIUS = 80.0f;
    bool _killSpotsLoaded = false;

    void PreloadKillSpots()
    {
        if (_killSpotsLoaded)
            return;

        _killSpotsLoaded = true;   // set first: a failed query must not retry forever

        // Spawns of kill targets AND of creatures that drop quest items: both are places where a
        // quest can actually be advanced, and both need coordinates to walk to.
        QueryResult result = WorldDatabase.Query(
            "SELECT c.id, c.map, c.position_x, c.position_y, c.position_z FROM creature c "
            "JOIN ("
            "  SELECT DISTINCT ObjectID AS entry FROM quest_objectives WHERE Type = 0 AND ObjectID > 0 "
            "  UNION "
            "  SELECT DISTINCT clt.Entry AS entry FROM creature_loot_template clt "
            "    JOIN quest_objectives o ON o.Type = 1 AND o.ObjectID = clt.Item"
            ") q ON c.id = q.entry "
            "WHERE c.PhaseId = 0 AND c.PhaseGroup = 0");

        if (!result)
        {
            TC_LOG_ERROR("scripts.bots", "pbot: kill-spot query returned nothing - quests without POI "
                "data will have no destination");
            return;
        }

        uint32 rows = 0;
        do
        {
            Field* f = result->Fetch();
            uint32 const entry = f[0].GetUInt32();
            uint32 const mapId = f[1].GetUInt32();

            MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
            if (!mapEntry || !mapEntry->IsContinent())
                continue;

            KillSpot spot;
            spot.MapId = mapId;
            spot.Pos.Relocate(f[2].GetFloat(), f[3].GetFloat(), f[4].GetFloat());
            _killSpots[entry].push_back(spot);
            ++rows;
        }
        while (result->NextRow());

        TC_LOG_INFO("scripts.bots", "pbot: kill spots loaded - {} creature entries, {} spawns",
            uint32(_killSpots.size()), rows);
    }
}

void PbotQuestGoal::MarkUnreachable(uint32 mapId, Position const& where)
{
    if (_unreachableObjectives.size() >= MAX_UNREACHABLE_SPOTS)
        _unreachableObjectives.erase(_unreachableObjectives.begin());

    UnreachableSpot spot;
    spot.MapId = mapId;
    spot.Pos = where;
    _unreachableObjectives.push_back(spot);
}

namespace
{
    // An objective marker can be a whole area (a blob of points outlining a valley). Any point in
    // it is a fine place to stand, so one is chosen at random rather than always the first — which
    // also stops a whole population funnelling onto the same rock.
    bool PointFromBlob(QuestPOIBlobData const& blob, Position& out)
    {
        if (blob.Points.empty())
            return false;

        QuestPOIBlobPoint const& point = blob.Points[urand(0, uint32(blob.Points.size()) - 1)];

        // Stored as world coordinates (the client converts them to map space itself), so they can
        // be walked to directly. Z is frequently 0 in this data, and is corrected by the ground
        // snap in the travel step rather than trusted here.
        out.Relocate(float(point.X), float(point.Y), float(point.Z), 0.0f);
        return true;
    }
}

namespace
{
    // Quest id -> where its taker stands. Several spawns per quest are kept, because the nearest
    // one is what a player walks to and one recorded position may be on the wrong continent.
    struct TurnInSpot
    {
        uint32 MapId = 0;
        Position Pos;
    };

    std::unordered_map<uint32, std::vector<TurnInSpot>> _turnInSpots;
    bool _turnInSpotsLoaded = false;

    // Every quest that has an ender relation at all, phasing and spawn state ignored. _turnInSpots
    // cannot answer this: a quest whose only ender stands in a phase is absent from it, and so is a
    // quest nobody takes back. Those two need opposite treatment, so they need separate evidence.
    std::unordered_set<uint32> _questsWithEnder;

    // At most this many recorded places per quest ON EACH MAP. A handful of the most common enders
    // are spawned in dozens of copies, and keeping every one would store a great deal to answer
    // "the nearest" — but the trimming must never cost a map its only entry. See PreloadTurnInSpots.
    constexpr size_t MAX_SPOTS_PER_MAP = 8;
}

void PbotQuestGoal::PreloadTurnInSpots()
{
    if (_turnInSpotsLoaded)
        return;
    _turnInSpotsLoaded = true;   // set first: a failed query must not retry forever

    // creature_questender IS the mapping — which NPC takes which quest — and joining it to the
    // spawn table turns it into coordinates. Reading the world DB directly rather than walking the
    // engine's relation containers keeps this the same shape as the vendor and workbench tables,
    // which are already proven.
    //
    // Objects count. 1563 quests are handed in to a chest, an altar or a notice board rather than
    // to a person, and reading only creature_questender loses every one of them.
    //
    // Unphased spawns only — and this one I got wrong first and had to be told by the numbers. I
    // included phased rows on the reasoning that a quest often phases its own taker, so excluding
    // them hides deliverable work. The measurement disagreed flatly: turn-ins went from five in
    // forty minutes to ZERO, because a phased taker is frequently NEARER than the reachable one,
    // wins the nearest-first choice, and the bot walks to an empty patch of ground it can never
    // interact with. A bot cannot step into another phase. Plausible reasoning, refuted by a soak.
    // Event-gated and pooled spawns are excluded for the same reason phased ones are: they are not
    // in the world most of the time. Measured 2026-10-20 on Wystan, who held ten finished quests
    // and handed in none of them in an hour: the nearest taker on his continent was a Spring
    // Collector (Noblegarden, spawntimesecs 120) bound to game_event_creature, so he walked 905
    // yards to bare ground, the probe reported "givers in range 0", the cooldown blocked the next
    // turn-in, and he repeated it. Holiday NPCs stand in capitals, which makes them the NEAREST
    // taker far more often than their 314-of-26963 share of the spawn table would suggest.
    QueryResult result = WorldDatabase.Query(
        "SELECT qe.quest, c.map, c.position_x, c.position_y, c.position_z "
        "FROM creature_questender qe JOIN creature c ON c.id = qe.id "
        "WHERE c.PhaseId = 0 AND c.PhaseGroup = 0 "
        "AND NOT EXISTS (SELECT 1 FROM game_event_creature ge WHERE ge.guid = c.guid) "
        "AND NOT EXISTS (SELECT 1 FROM pool_members pm WHERE pm.spawnId = c.guid AND pm.type = 0) "
        "UNION ALL "
        "SELECT ge.quest, g.map, g.position_x, g.position_y, g.position_z "
        "FROM gameobject_questender ge JOIN gameobject g ON g.id = ge.id "
        "WHERE g.PhaseId = 0 AND g.PhaseGroup = 0 "
        "AND NOT EXISTS (SELECT 1 FROM game_event_gameobject geg WHERE geg.guid = g.guid) "
        "AND NOT EXISTS (SELECT 1 FROM pool_members pm WHERE pm.spawnId = g.guid AND pm.type = 1)");

    if (!result)
    {
        TC_LOG_ERROR("scripts.bots", "pbot: quest-ender query returned nothing — bots will finish "
            "quests and never hand any of them in");
        return;
    }

    uint32 rows = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 const questId = f[0].GetUInt32();

        uint32 const mapId = f[1].GetUInt32();

        // Continents only. A bot cannot walk into a dungeon, so a taker that exists only in one is
        // not a destination - and keeping it turned delivery into an endless retry: measured over
        // half an hour, two bots produced 116 "could not travel to map 568" lines, every one of
        // them the same Zul'Aman quest, and neither ever handed anything in. The town-spot tables
        // already draw the line here for the same reason.
        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry || !mapEntry->IsContinent())
            continue;

        // The cap is per MAP, not per quest, and that distinction is load-bearing now. It used to be
        // a flat eight-per-quest, which was harmless while this table only answered "walk where" —
        // any eight spots give a usable destination. It stopped being harmless the moment the accept
        // side started refusing work this table cannot place: eight spawns of a common ender on one
        // continent would evict the single spawn on another, and the bot would decline a quest it
        // was standing right next to the taker of.
        std::vector<TurnInSpot>& spots = _turnInSpots[questId];
        size_t onThisMap = 0;
        for (TurnInSpot const& existing : spots)
            if (existing.MapId == mapId)
                ++onThisMap;

        if (onThisMap >= MAX_SPOTS_PER_MAP)
            continue;

        TurnInSpot spot;
        spot.MapId = mapId;
        spot.Pos.Relocate(f[2].GetFloat(), f[3].GetFloat(), f[4].GetFloat());
        spots.push_back(spot);
        ++rows;
    }
    while (result->NextRow());

    // Deliberately unfiltered — no phase clause, no join to the spawn table. This answers only
    // "does anybody take this quest back", which is a property of the quest, not of the world.
    if (QueryResult enders = WorldDatabase.Query(
        "SELECT quest FROM creature_questender UNION SELECT quest FROM gameobject_questender"))
    {
        do
            _questsWithEnder.insert(enders->Fetch()[0].GetUInt32());
        while (enders->NextRow());
    }

    TC_LOG_INFO("scripts.bots", "pbot: loaded turn-in positions for {} quests ({} spawn rows); "
        "{} quests have an ender at all", uint32(_turnInSpots.size()), rows,
        uint32(_questsWithEnder.size()));
}

bool PbotQuestGoal::HasEnderOnMap(uint32 questId, uint32 mapId)
{
    PreloadTurnInSpots();

    auto const itr = _turnInSpots.find(questId);
    if (itr == _turnInSpots.end())
        return false;

    for (TurnInSpot const& spot : itr->second)
        if (spot.MapId == mapId)
            return true;

    return false;
}

bool PbotQuestGoal::HasAnyEnder(uint32 questId)
{
    PreloadTurnInSpots();
    return _questsWithEnder.count(questId) != 0;
}

std::unordered_set<uint32> PbotQuestGoal::WantedCreatures(Player* bot)
{
    std::unordered_set<uint32> wanted;
    if (!bot || !bot->IsInWorld())
        return wanted;

    PreloadItemDroppers();

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (QuestObjective const& obj : quest->GetObjectives())
        {
            // Item objectives name the ITEM, not the creature. Resolve it through the loot tables:
            // a bot that ignores these quests walks past the very mobs that would finish them.
            if (obj.Type == QUEST_OBJECTIVE_ITEM && obj.ObjectID > 0)
            {
                if (bot->GetItemCount(uint32(obj.ObjectID), true) >= uint32(std::max(1, obj.Amount)))
                    continue;   // enough already

                auto const droppers = _itemDroppers.find(uint32(obj.ObjectID));
                if (droppers == _itemDroppers.end())
                    continue;   // the item is not dropped by any creature - not a hunt

                for (uint32 entry : droppers->second)
                    wanted.insert(entry);

                continue;
            }

            if (obj.Type != QUEST_OBJECTIVE_MONSTER || obj.ObjectID <= 0)
                continue;

            // Untracked objectives have no progress to read, and asking logs an error per call —
            // one such objective once produced 633,073 log lines in a single soak.
            if (obj.StorageIndex < 0)
                continue;

            if (bot->GetQuestObjectiveData(obj) >= obj.Amount)
                continue;   // this one is already done; killing more of them earns nothing

            wanted.insert(uint32(obj.ObjectID));
        }
    }

    return wanted;
}

bool PbotQuestGoal::HasReachableEnderAnywhere(uint32 questId)
{
    PreloadTurnInSpots();

    // No ender relation at all means the engine closes it out itself — reachable by definition.
    if (!HasAnyEnder(questId))
        return true;

    // _turnInSpots is built from UNPHASED spawns only, so an ender present in the relation tables
    // but absent here exists solely inside a phase. A bot cannot step into a phase.
    return _turnInSpots.count(questId) != 0;
}

bool PbotQuestGoal::CanFinishOnMap(uint32 questId, uint32 mapId)
{
    // Nobody takes it back: the engine finishes it by itself, so anywhere will do. Rare — six of the
    // 562 quests the bots were holding when this was written — but refusing them would be wrong.
    if (!HasAnyEnder(questId))
        return true;

    if (!HasEnderOnMap(questId, mapId))
        return false;

    // And the WORK has to be here too, not just the taker.
    //
    // Measured 2026-10-22: 769 of 953 unfinished kill objectives named creatures with no spawn on the
    // bot's continent at all - quests picked up in another expansion, carried around, never worked
    // once. They fill the log, block new accepts, and hand the goal picker destinations it cannot
    // reach. Item objectives are deliberately not judged: the item is not the creature, and refusing
    // on a guess would throw away real work.
    PreloadKillSpots();

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return true;

    bool hasKillObjective = false;

    for (QuestObjective const& obj : quest->GetObjectives())
    {
        if (obj.Type != QUEST_OBJECTIVE_MONSTER || obj.ObjectID <= 0)
            continue;

        hasKillObjective = true;

        auto const itr = _killSpots.find(uint32(obj.ObjectID));
        if (itr == _killSpots.end())
            continue;

        for (KillSpot const& spot : itr->second)
            if (spot.MapId == mapId)
                return true;   // at least one target stands on this continent
    }

    return !hasKillObjective;   // nothing to judge by: leave the old answer
}

bool PbotQuestGoal::FindTurnIn(Player* bot, Position& out)
{
    if (!bot || !bot->IsInWorld())
        return false;

    PreloadTurnInSpots();

    // Nearest, not random, and only if it is actually within reach. A player carrying finished work
    // walks to the closest person who will take it — and unlike an objective blob, where any point
    // in the area will do, these are specific people standing in specific places.
    //
    // Everything beyond MAX_TURN_IN_DISTANCE is left in the log rather than chased. That is not the
    // bot giving up: it is the bot declining to spend its afternoon on a march it cannot finish,
    // and going to do something it can. The far ones become deliverable again the moment its
    // wandering takes it back to that part of the world.
    // ...unless there is a flight. The cap is a statement about walking, and it stayed a statement
    // about walking even after the bots learned to fly, which is why nothing changed: a taker 5,000
    // yards away was never offered as a destination, so the bot never set out, so no flight was ever
    // requested for it. Measured, that was 95 of the 285 rewards waiting on this realm.
    //
    // Simply raising the number would put bots back on marches they cannot finish. Asking whether
    // this particular spot is flyable admits exactly the far ones that a flight can serve, and no
    // others. Nearest still wins among what is admitted.
    float const walkCapSq = MAX_TURN_IN_DISTANCE * MAX_TURN_IN_DISTANCE;
    float best = std::numeric_limits<float>::max();
    bool found = false;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        auto const itr = _turnInSpots.find(questId);
        if (itr == _turnInSpots.end())
            continue;

        for (TurnInSpot const& spot : itr->second)
        {
            if (spot.MapId != bot->GetMapId())
                continue;

            float const distance = bot->GetExactDistSq(spot.Pos.GetPositionX(),
                spot.Pos.GetPositionY(), spot.Pos.GetPositionZ());

            // Already standing here and still holding the quest? Then it cannot be handed in at
            // this spot — the taker is phased, pooled, dead or simply absent — and offering the
            // place as a destination again parks the bot on it forever.
            //
            // That is exactly what happened: with a turn-in goal chosen ahead of everything else,
            // a bot that arrived and could not deliver re-chose the ground under its own feet,
            // stayed "busy travelling" every tick, and the quest module below never ran at all.
            // Measured, brutally: quest lines fell from 46 in forty minutes to TWO, while the same
            // bots kept killing, gathering, chatting and trading perfectly normally.
            if (distance <= ALREADY_HERE_RANGE * ALREADY_HERE_RANGE)
                continue;

            // Beyond walking range this is only a destination if the bot can fly most of the way.
            // Checked second, after the cheap tests, because it is the expensive one.
            if (distance > walkCapSq && !PbotFlight::CanReach(bot, spot.Pos))
                continue;

            if (distance < best)
            {
                best = distance;
                out = spot.Pos;
                found = true;
            }
        }
    }

    return found;
}

bool PbotQuestGoal::HasDeliverableOnMap(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return false;

    PreloadTurnInSpots();

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        auto const itr = _turnInSpots.find(questId);
        if (itr == _turnInSpots.end())
            continue;

        for (TurnInSpot const& spot : itr->second)
            if (spot.MapId == bot->GetMapId())
                return true;
    }

    return false;
}

bool PbotQuestGoal::FindRemoteTurnIn(Player* bot, uint32& outMapId, Position& outPos)
{
    if (!bot || !bot->IsInWorld())
        return false;

    PreloadTurnInSpots();

    // "Out of reach" is not the same as "on another map". Measured on the live realm: of 133
    // completed quests whose taker stood on the bot's OWN map, 83 were further away than
    // MAX_TURN_IN_DISTANCE — so the bot would never walk to them, yet they counted as local work
    // and blocked the trip that could have collected them. A destination is worth teleporting to
    // when walking there was never going to happen, whichever map it is on.
    std::unordered_map<uint32, uint32> countByMap;
    std::unordered_map<uint32, Position> spotByMap;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        auto const itr = _turnInSpots.find(questId);
        if (itr == _turnInSpots.end())
            continue;

        // Within walking range on this map — the local module will handle it, and hauling the bot
        // across the world for something it could reach on foot would be absurd.
        bool walkable = false;
        for (TurnInSpot const& spot : itr->second)
            if (spot.MapId == bot->GetMapId()
                && bot->GetExactDistSq(spot.Pos.GetPositionX(), spot.Pos.GetPositionY(),
                                       spot.Pos.GetPositionZ())
                   <= MAX_TURN_IN_DISTANCE * MAX_TURN_IN_DISTANCE)
            {
                walkable = true;
                break;
            }

        if (walkable)
            continue;

        // Count the QUEST once per map, not once per recorded spawn.
        //
        // This counted every spot, so a quest whose taker is spawned eight times on one continent
        // scored eight and a quest with a single taker elsewhere scored one — and the "best map"
        // was decided by how often an NPC happens to be duplicated rather than by how much work
        // waits there. Worse, CountRemoteTurnInsOnMap (which decides whether the trip is worth
        // making) counts quests, so the two halves measured different things: this one would pick
        // a map claiming eight, that one would answer "only 1 waiting", and the trip never
        // happened. 313 of 477 refusals in one hour read exactly like that.
        std::unordered_set<uint32> mapsForThisQuest;
        for (TurnInSpot const& spot : itr->second)
        {
            if (mapsForThisQuest.insert(spot.MapId).second)
                ++countByMap[spot.MapId];

            spotByMap.emplace(spot.MapId, spot.Pos);
        }
    }

    if (countByMap.empty())
        return false;

    // The map that clears the most of the backlog in one trip.
    auto best = countByMap.begin();
    for (auto itr = countByMap.begin(); itr != countByMap.end(); ++itr)
        if (itr->second > best->second)
            best = itr;

    outMapId = best->first;
    outPos = spotByMap[best->first];
    return true;
}

uint32 PbotQuestGoal::CountRemoteTurnInsOnMap(Player* bot, uint32 mapId)
{
    if (!bot || !bot->IsInWorld())
        return 0;

    PreloadTurnInSpots();

    uint32 count = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        auto const itr = _turnInSpots.find(questId);
        if (itr == _turnInSpots.end())
            continue;

        for (TurnInSpot const& spot : itr->second)
            if (spot.MapId == mapId
                && (mapId != bot->GetMapId()
                    || bot->GetExactDistSq(spot.Pos.GetPositionX(), spot.Pos.GetPositionY(),
                                           spot.Pos.GetPositionZ())
                       > MAX_TURN_IN_DISTANCE * MAX_TURN_IN_DISTANCE))
            {
                ++count;
                break;
            }
    }

    return count;
}

bool PbotQuestGoal::Find(Player* bot, Position& out)
{
    if (!bot || !bot->IsInWorld())
        return false;

    // Collect every unfinished quest that has objective data HERE, then go where the MOST of them
    // can be worked on at once.
    //
    // Walking the log in slot order would make every bot pursue its oldest quest forever, and a
    // quest whose objective sits on another continent would block the ones it could actually do.
    // Picking at random fixed that but threw away the thing a player does instinctively: when three
    // quests all point at the same hillside, you do all three in one trip. A bot choosing at random
    // walks to the hillside, then to a swamp, then back to the hillside — the same work, three times
    // the walking, and it looks exactly as aimless as it is.
    PreloadKillSpots();

    std::vector<Position> candidates;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;   // complete quests want the questgiver, which is the quest module's job

        if (QuestPOIData const* poi = sObjectMgr->GetQuestPOIData(int32(questId)))
        {
            for (QuestPOIBlobData const& blob : poi->Blobs)
            {
                if (blob.MapID != int32(bot->GetMapId()))
                    continue;

                Position point;
                if (PointFromBlob(blob, point))
                    candidates.push_back(point);
            }
        }

        // Plus the ground the quest's own targets stand on. Many quests carry no POI data, and a
        // destination derived from the creatures is better than no destination at all.
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (QuestObjective const& obj : quest->GetObjectives())
        {
            if (obj.Type != QUEST_OBJECTIVE_MONSTER || obj.ObjectID <= 0)
                continue;

            if (obj.StorageIndex < 0)
                continue;   // untracked: no progress to read, and asking logs an error per call

            if (bot->GetQuestObjectiveData(obj) >= obj.Amount)
                continue;

            auto const itr = _killSpots.find(uint32(obj.ObjectID));
            if (itr == _killSpots.end())
                continue;

            // Nearest handful only: the scoring below is quadratic, and four spawn points of the same
            // entry in the same valley all describe the same destination.
            std::vector<KillSpot const*> nearest;
            for (KillSpot const& spot : itr->second)
            {
                if (spot.MapId != bot->GetMapId())
                    continue;

                nearest.push_back(&spot);
            }

            std::sort(nearest.begin(), nearest.end(), [bot](KillSpot const* a, KillSpot const* b)
            {
                return bot->GetExactDistSq(a->Pos.GetPositionX(), a->Pos.GetPositionY(),
                    a->Pos.GetPositionZ())
                    < bot->GetExactDistSq(b->Pos.GetPositionX(), b->Pos.GetPositionY(),
                    b->Pos.GetPositionZ());
            });

            for (size_t i = 0; i < nearest.size() && i < 4; ++i)
                candidates.push_back(nearest[i]->Pos);
        }
    }

    // Drop destinations already known to be unreachable: offering them again is what produced one
    // carry after another at the same place.
    if (!_unreachableObjectives.empty())
    {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [bot](Position const& candidate)
            {
                for (UnreachableSpot const& bad : _unreachableObjectives)
                {
                    if (bad.MapId != bot->GetMapId())
                        continue;

                    if (bad.Pos.GetExactDist2d(candidate.GetPositionX(), candidate.GetPositionY())
                        < UNREACHABLE_RADIUS)
                        return true;
                }
                return false;
            }), candidates.end());
    }

    if (candidates.empty())
        return false;

    // Score each candidate by how much OTHER work sits around it, and take the busiest corner of
    // the map. Distance breaks ties: among equally productive places, a player goes to the near one.
    //
    // The radius is what "the same area" means to someone standing there — far enough that several
    // objectives in one valley count together, tight enough that two ends of a zone do not.
    size_t bestIndex = 0;
    uint32 bestCompany = 0;
    float bestDistance = std::numeric_limits<float>::max();

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        uint32 company = 0;
        for (size_t j = 0; j < candidates.size(); ++j)
            if (i != j && candidates[i].GetExactDist2d(candidates[j].GetPositionX(),
                candidates[j].GetPositionY()) <= CLUSTER_RADIUS)
                ++company;

        float const distance = bot->GetExactDist2d(candidates[i].GetPositionX(),
            candidates[i].GetPositionY());

        if (company > bestCompany || (company == bestCompany && distance < bestDistance))
        {
            bestIndex = i;
            bestCompany = company;
            bestDistance = distance;
        }
    }

    out = candidates[bestIndex];
    return true;
}
