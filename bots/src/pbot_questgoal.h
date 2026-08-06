/*
 * Companion Bots — knowing where a quest is done (TrinityCore master, retail 12.0.7).
 *
 * A bot could already walk up to a questgiver and accept "kill eight wolves", and then it stood
 * there. Kill credit is granted by the engine, so the quest could only ever complete by accident,
 * if a wolf happened to wander into its 40-yard search. That is the single biggest reason the bots
 * did not read as players: they took work and never went to do it.
 *
 * The world DB already knows where every objective is — quest_poi_points, the same data that draws
 * the marker on a player's map, 313k points of it. So the bot does not need to be told anything:
 * it asks where its own quest is and walks there.
 */

#ifndef TRINITYCORE_PBOT_QUESTGOAL_H
#define TRINITYCORE_PBOT_QUESTGOAL_H

#include "Define.h"
#include "Position.h"

class Player;

namespace PbotQuestGoal
{
    // How close two objective markers have to be to count as "the same area", i.e. as work that
    // one trip can advance together. Roughly a corner of a zone rather than the whole of it.
    constexpr float CLUSTER_RADIUS = 200.0f;

    // Where should this bot go to progress its unfinished quests? Picks the place where the MOST of
    // them can be worked on at once, nearest first among equals. False when it holds no unfinished
    // quest with objective data on the map it is standing on.
    bool Find(Player* bot, Position& out);

    // Where should it go to HAND ONE IN? False when it holds nothing finished, or nothing finished
    // whose taker stands on this map.
    //
    // This turned out to be the larger half of questing. Measured: bots were carrying 255 completed
    // quests between them and handing in about one per half hour — they did the work and never went
    // back for the reward, which is the least player-like thing a character can do. Finishing has to
    // outrank starting, or a bot accumulates a log full of done work and no longer has room to take
    // anything new.
    // How far a bot will walk to deliver finished work.
    //
    // Bounded because most of what a bot has finished cannot be delivered from where it stands:
    // measured over 259 completed quests, 136 had their taker on a different continent, 47 had no
    // recorded taker at all, and among the rest the distances ran to 4221, 9619 and 11603 yards.
    // A player crosses that with a flight path; a bot walks it at seven yards a second and is still
    // walking when the goal budget expires, having done nothing else meanwhile.
    //
    // Sized against PBOT_GOAL_WALK_BUDGET_MS (three minutes) rather than chosen for roundness: this
    // is roughly what a bot can actually cover before the journey is abandoned.
    constexpr float MAX_TURN_IN_DISTANCE = 1200.0f;

    // A turn-in spot the bot is already standing on is not a destination. Slightly wider than the
    // travel arrival radius, so a bot that stopped a few yards short still counts as "here".
    constexpr float ALREADY_HERE_RANGE = 40.0f;

    bool FindTurnIn(Player* bot, Position& out);

    // Is there anywhere on this map to hand this quest in? Asked by the quest log when it is full
    // and has to decide what is dead weight.
    bool HasTakerOnMap(uint32 questId, uint32 mapId);

    // Reads the quest -> taker -> spawn position mapping once. Safe to call repeatedly; the first
    // call does the work. Called at world start so the cost is not paid inside a bot's tick.
    void PreloadTurnInSpots();
}

#endif // TRINITYCORE_PBOT_QUESTGOAL_H
