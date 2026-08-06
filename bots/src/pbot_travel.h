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

    // Issues the next hop toward dest. Returns true while still travelling, false once the bot has
    // arrived or the destination is unreachable — the caller then does something else.
    bool StepToward(Player* bot, Position const& dest);

    // A point far enough away to be a real journey, ground-snapped. This is what replaces jittering
    // around a spawn point: a bot with nothing to do walks somewhere, the way a player does.
    //
    // How far is a matter of character — a homebody circles its zone, a wanderer crosses it — so the
    // distance is scaled by the bot's own wanderlust rather than being the same for everyone.
    bool PickRoamPoint(Player* bot, Position& out);
}

#endif // TRINITYCORE_PBOT_TRAVEL_H
