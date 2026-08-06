/*
 * Companion Bots — quest objective location. See pbot_questgoal.h.
 */

#include "pbot_questgoal.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"

#include <limits>
#include <unordered_map>
#include <vector>

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

    // At most this many recorded places per quest. A handful of the most common enders are spawned
    // in dozens of copies, and keeping every one would store a great deal to answer "the nearest".
    constexpr size_t MAX_SPOTS_PER_QUEST = 8;
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
    QueryResult result = WorldDatabase.Query(
        "SELECT qe.quest, c.map, c.position_x, c.position_y, c.position_z "
        "FROM creature_questender qe JOIN creature c ON c.id = qe.id "
        "WHERE c.PhaseId = 0 AND c.PhaseGroup = 0 "
        "UNION ALL "
        "SELECT ge.quest, g.map, g.position_x, g.position_y, g.position_z "
        "FROM gameobject_questender ge JOIN gameobject g ON g.id = ge.id "
        "WHERE g.PhaseId = 0 AND g.PhaseGroup = 0");

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

        std::vector<TurnInSpot>& spots = _turnInSpots[questId];
        if (spots.size() >= MAX_SPOTS_PER_QUEST)
            continue;

        TurnInSpot spot;
        spot.MapId = f[1].GetUInt32();
        spot.Pos.Relocate(f[2].GetFloat(), f[3].GetFloat(), f[4].GetFloat());
        spots.push_back(spot);
        ++rows;
    }
    while (result->NextRow());

    TC_LOG_INFO("scripts.bots", "pbot: loaded turn-in positions for {} quests ({} spawn rows)",
        uint32(_turnInSpots.size()), rows);
}

bool PbotQuestGoal::HasTakerOnMap(uint32 questId, uint32 mapId)
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
    float best = MAX_TURN_IN_DISTANCE * MAX_TURN_IN_DISTANCE;   // compared squared, as below
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
    std::vector<Position> candidates;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;   // complete quests want the questgiver, which is the quest module's job

        QuestPOIData const* poi = sObjectMgr->GetQuestPOIData(int32(questId));
        if (!poi)
            continue;

        for (QuestPOIBlobData const& blob : poi->Blobs)
        {
            if (blob.MapID != int32(bot->GetMapId()))
                continue;

            Position point;
            if (PointFromBlob(blob, point))
                candidates.push_back(point);
        }
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
