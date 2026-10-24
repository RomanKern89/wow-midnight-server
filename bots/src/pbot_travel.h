/*
 * Companion Bots — going somewhere (TrinityCore master, retail 12.0.7).
 *
 * Everything a world bot did until now happened inside a 35-yard bubble: it wandered around its
 * spawn anchor and fought whatever walked into a 40-yard search. That is why bots looked idle, why
 * they barely levelled (nothing to kill unless it came to them), and why a quest they had taken was
 * never actually done — they had no idea where its objective was and no way to walk there.
 *
 * This module is the missing half: a destination, and the ability to reach it. Long walks have to
 * be stepped — asking the pathfinder for a point hundreds of yards away returns no path at all
 * (the same `_checkPathLengths` failure that once left bots standing next to gathering nodes and
 * frozen in battlegrounds), so travel is issued one bounded hop at a time.
 */

#ifndef TRINITYCORE_PBOT_TRAVEL_H
#define TRINITYCORE_PBOT_TRAVEL_H

#include "Define.h"
#include "Position.h"

class Player;

namespace PbotTravel
{
    // Longest single MovePoint we will ask for. Beyond roughly this the navmesh stops answering.
    constexpr float MAX_HOP = 50.0f;

    // Close enough to call it arrived. Generous on purpose: the point of travelling is to be in the
    // right AREA, where the local hunt/gather/quest logic takes over.
    constexpr float ARRIVED_RANGE = 25.0f;

    // How far below the ground a position may sit before it is treated as invalid.
    //
    // Generous, because legitimate places ARE below the surface — caves, cellars, the inside of any
    // building with a floor of its own. What this catches is the thing that has no floor above it
    // at all: a character that fell through the world.
    constexpr float UNDERGROUND_BY = 12.0f;

    // Is this somewhere a bot can actually stand and be moved from?
    //
    // The failure this exists for: two bots spent an entire four-hour soak motionless, one at 21 to
    // 43 yards below the terrain and the other at 6, because the navmesh runs ABOVE them and no path
    // can be built from under the floor. Nothing anywhere validated a position before storing it as
    // the bot's home, so every rescue teleported them back underground and five minutes later they
    // were stuck again. 98 of the 149 rescues in that soak were those two bots.
    bool IsStandable(Player* bot, Position const& pos);

    // The same position, lifted onto the ground beneath it. Returns false when there is no ground
    // there at all — off the edge of the map, or a coordinate that means nothing.
    bool SnapToGround(Player* bot, Position& pos);

    // Issues the next hop toward dest. Returns true while still travelling, false once the bot has
    // arrived or the destination is unreachable — the caller then does something else.
    //
    // On foot only. Use this when the destination is deliberately local, or from inside the flight
    // code itself, where asking about flying again would be circular.
    bool StepGround(Player* bot, Position const& dest);

    // The same, but a long journey goes by air when the bot can arrange it. This is what callers
    // should use: every distant errand in the codebase — the quest goal, the turn-in, the repair
    // trip, moving house — becomes flyable by going through here, without any of them knowing that
    // flight paths exist.
    bool StepToward(Player* bot, Position const& dest);

    // A point far enough away to be a real journey, ground-snapped. This is what replaces jittering
    // around a spawn point: a bot with nothing to do walks somewhere, the way a player does.
    //
    // How far is a matter of character — a homebody circles its zone, a wanderer crosses it — so the
    // distance is scaled by the bot's own wanderlust rather than being the same for everyone.
    bool PickRoamPoint(Player* bot, Position& out);

    // How many movement orders this bot has had to write off because they never became a live
    // spline — the signature of a place the navmesh cannot leave. Reported wherever a failed walk is
    // explained, so "the ground refuses to be crossed" can be told apart from "the bot was busy
    // fighting" instead of guessed at.
    uint32 FailedHops(Player const* bot);

    // How many hops this bot has actually issued and had ACCEPTED (a spline really launched) since
    // the current destination was chosen.
    //
    // The companion of FailedHops, and the one number that separates two failures that look
    // identical from outside. A destination never reached after dozens of accepted hops is a
    // destination the GROUND does not lead to — a cliff, a wall, a navmesh gap the pathfinder routes
    // around and comes back from — and no change to which tick wins will help it. A destination
    // never reached after a handful of hops is one the bot was not really walking to, which is a
    // different repair entirely.
    //
    // Without this the two read the same: a bot that never issued the movement and a bot whose
    // movement leads nowhere both end an 180-second budget standing where they started, with zero
    // failed orders and no combat — which is exactly the state the last diagnosis had to guess at.
    uint32 HopsIssued(Player const* bot);
    void ResetHops(Player const* bot);
}

#endif // TRINITYCORE_PBOT_TRAVEL_H
