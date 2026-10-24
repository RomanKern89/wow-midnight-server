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

#include <unordered_set>

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

    // Remember a destination the bot could not walk to, so it stops being offered.
    //
    // The walk budget ends in a teleport ("carried it to the objective") when the bot makes no
    // headway - usually a capital's mesas and lifts, or a spawn point inside geometry. Carrying it
    // once is the escape; choosing the same place again next tick is the defect. Measured 2026-10-22:
    // 26 carries in one window, and the objective was re-chosen afterwards as if nothing happened.
    void MarkUnreachable(uint32 mapId, Position const& where);

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
    // Sized to what a bot ACTUALLY covers, not to what the walk budget allows on paper.
    //
    // 1200 was the arithmetic answer: three minutes of goal budget at roughly seven yards a second.
    // It ignored that the walk is interrupted — a bot fights, loots, is knocked off its mount — so
    // the trip times out short of the taker, the goal is dropped, the cooldown expires, and the
    // same doomed walk starts again. Measured over an eight-hour soak: 46 turn-ins sat inside the
    // old radius and produced FIVE hand-ins, while the delivery lift converted 41 attempts into 40.
    //
    // Anything beyond this is not "far", it is out of reach on foot, and out-of-reach work belongs
    // to the lift. Which is also what a player does: you walk across the field and you fly across
    // the zone.
    constexpr float MAX_TURN_IN_DISTANCE = 600.0f;

    // A turn-in spot the bot is already standing on is not a destination. Slightly wider than the
    // travel arrival radius, so a bot that stopped a few yards short still counts as "here".
    constexpr float ALREADY_HERE_RANGE = 40.0f;

    bool FindTurnIn(Player* bot, Position& out);

    // Is there an unphased place on this map to hand this quest in? Named for what it reads — the
    // ENDER tables — because the previous name said "Taker" and I believed it, which is how the two
    // halves of questing ended up judging by different things for a whole soak.
    bool HasEnderOnMap(uint32 questId, uint32 mapId);

    // Does anyone anywhere take this quest? Not "is there a reachable spot" — literally whether the
    // quest has an ender relation at all.
    //
    // The distinction matters because a quest with no ender is not broken: the engine completes it
    // by itself. Treating "no reachable spot" and "nobody to hand it to" as the same answer would
    // make the bot refuse legitimate work.
    bool HasAnyEnder(uint32 questId);

    // THE shared verdict: can a bot standing on this map ever finish this quest?
    //
    // Both halves of questing must ask this one question. They did not, and the cost was exact:
    // acquisition looked at the quest GIVER and took the quest; housekeeping looked at the ENDER
    // and threw it straight back out; the giver was still standing there, so the bot took it again.
    // Three bots logged 3,803 accept-and-drop pairs of three quests. Quest 14037 is the clean
    // example — Miles Standish hands it out unphased, and every spawn of William Mullins, the only
    // man who will take it back, sits in phase 43472 where no bot can reach him.
    //
    // Neither half was wrong on its own. They simply were not asking the same question.
    bool CanFinishOnMap(uint32 questId, uint32 mapId);

    // Is there ANY reachable place in the world to hand this quest in?
    //
    // False means the quest has an ender that exists only inside a phase, on every single spawn —
    // work the bot can never finish anywhere, by walking, flying or being teleported. Measured on
    // the live realm: 44 of 277 finished quests were in exactly that state, sitting in quest logs
    // forever because the tidy-up only runs when the log is FULL and bots hover below the cap.
    bool HasReachableEnderAnywhere(uint32 questId);

    // Does this bot hold finished work whose taker stands on the map it is on — at ANY distance?
    //
    // Deliberately unbounded, unlike FindTurnIn. That one answers "where should I walk next", so it
    // is capped at MAX_TURN_IN_DISTANCE to fit the walk budget. This one answers "would leaving
    // strand something I have already earned", and for that the distance is irrelevant: a taker
    // 3,000 yards away is still reachable next time the bot's wandering passes it, and becomes
    // unreachable forever the moment the bot changes continent.
    bool HasDeliverableOnMap(Player* bot);

    // Where is the finished work this bot can NOT deliver from the map it is on?
    //
    // Returns the map holding the most such quests, and a taker's position on it. False when every
    // completed quest is either deliverable here or has no recorded taker anywhere.
    //
    // A player flies back for a reward they earned. A bot with no way to change continent simply
    // carries it forever — measured on the live realm, 182 of 289 completed quests had their taker
    // on a map the bot had left, and nothing in the loop could ever bring them together again.
    bool FindRemoteTurnIn(Player* bot, uint32& outMapId, Position& outPos);

    // How many completed quests this bot could hand in on that map — the size of the prize, used
    // to decide whether a trip across a continent is worth making at all.
    uint32 CountRemoteTurnInsOnMap(Player* bot, uint32 mapId);

    // Which creatures does this bot still need to kill, and which items does it still need to
    // collect, for the quests it is carrying?
    //
    // This is the difference between a bot that fights and a bot that PROGRESSES. Measured on the
    // live realm: 45% of every tick went to combat, and of 362 quests in progress exactly TEN had
    // any objective advanced at all — the target search took the nearest level-appropriate creature
    // and quest credit happened only by coincidence. A bot standing in the middle of the wolves its
    // own quest names, killing the boars beside them, is not questing.
    //
    // Returns creature entries; empty when the bot has no kill/loot objective outstanding, in which
    // case the caller should fight whatever it would have fought anyway.
    std::unordered_set<uint32> WantedCreatures(Player* bot);

    // Reads the quest -> taker -> spawn position mapping once. Safe to call repeatedly; the first
    // call does the work. Called at world start so the cost is not paid inside a bot's tick.
    void PreloadTurnInSpots();
}

#endif // TRINITYCORE_PBOT_QUESTGOAL_H
