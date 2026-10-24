/*
 * Trips back for finished work left on another map.
 *
 * A bot that completes a quest and then moves on has earned a reward it can never collect: the
 * taker stands on a continent it is no longer on, and every other module reasons only about the
 * map the bot is standing on. Measured on the live realm — 182 of 289 completed quests were in
 * exactly that state, and no amount of local questing could ever reduce that number.
 *
 * This is the return leg. It uses the same teleport PbotMigrate uses, on the same principle: a
 * player crosses a continent by flight path, not on foot, and a bot that walked would spend its
 * entire life in transit.
 */

#ifndef TRINITYCORE_PBOT_DELIVERY_H
#define TRINITYCORE_PBOT_DELIVERY_H

#include "Define.h"
#include "Position.h"

class Player;

namespace PbotDelivery
{
    // How long after one trip before another may be considered. Long, because a bot that teleports
    // whenever it holds a stray completed quest is a bot that lives in loading screens.
    //
    // Lowered 10 -> 5 minutes on 2026-10-19 and reverted the same day: hand-ins did NOT rise (8 in 25
    // minutes against 11 in 30 before — noise), flights actually fell, and the queue moved at the same
    // slow pace (219 -> 217). So this cap is not what spaces the deliveries out either. Do not touch it
    // again without first measuring what PbotDelivery::Tick actually returns and how often.
    constexpr uint32 AFTER_TRIP_MS = 600000;   // 10 minutes

    // At least this much waiting on the far map before the trip is worth making.
    //
    // Was 3 (and "a handful is worth a journey"), lowered to 1 on 2026-10-20 by explicit operator
    // decision: the bot may travel for a SINGLE far-flung finished quest, and the operator also allows
    // teleports, flight masters and the bot's own mount once it is high enough level. That reverses the
    // old human-likeness rule ("criterion 1: never teleport"), so the undelivered queue is now meant to
    // drain fast rather than slowly. STUCK_QUEST_COUNT is kept for the message it prints; with a
    // threshold of 1 it is inert.
    constexpr uint32 WORTH_THE_TRIP = 1;

    // ...unless the bot is stuck, and then one is enough.
    //
    // A flat threshold of three assumed the undelivered work PILES UP on one map. Measured, it does
    // not: it scatters. Of 477 declined trips in an hour, 323 were declined for "only 1 waiting" or
    // "only 2 waiting" — a bot with fourteen finished quests had them spread across ten different
    // maps, no single one of them worth a journey, so it went nowhere and kept all fourteen.
    //
    // A player clears a full log even for one reward, because the log slot is worth more than the
    // trip costs. Below this many held quests the bot is not under pressure and the flat threshold
    // stands; at or above it, any single reward justifies the flight.
    constexpr uint32 STUCK_QUEST_COUNT = 10;

    // How long to leave it after deciding a trip is not worth making. The answer cannot change
    // until the bot finishes another quest, so asking every thirty seconds only fills the log.
    constexpr uint32 DECLINED_RETRY_MS = 300000;   // 5 minutes

    // Returns true if the bot was sent. `home`/`homeMapId` are moved to the destination, exactly as
    // PbotMigrate does, so the bot lives there rather than walking back toward the map it left.
    bool Tick(Player* bot, uint32& cooldownMs, uint32 diff, Position& home, uint32& homeMapId);
}

#endif
