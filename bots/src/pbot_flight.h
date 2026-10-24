/*
 * Companion Bots — flight paths (TrinityCore master, retail 12.0.7).
 *
 * Everything a bot went to, it walked to, in fifty-yard hops at seven yards a second. That is why
 * every long errand in this codebase carries a distance cap: turn-ins stop at 1200 yards, the goal
 * walk gets three minutes, and beyond that the bot simply gives up and does something nearer. The
 * caps are not wrong — they are what keeps a bot from spending its whole day in transit — but they
 * describe a character who cannot use the transport the world is built around.
 *
 * Measured at the time of writing: of 285 finished quests waiting to be handed in, 95 had their
 * taker on the same map but further than the cap, and another 131 on a map the bot had left. Two
 * thirds of the earned rewards were sitting behind a journey a real player makes without thinking.
 *
 * So: flight masters. Two halves, both of them the same ones a player has:
 *
 *   Discovery — a flight point becomes available by going near it, which is exactly how retail has
 *   worked since the toast message replaced clicking. Bots already migrate between zones, so their
 *   map of the world grows the same way a player's does, by having been places.
 *
 *   Boarding — a bot with a long way to go walks to the flight master (it has to; you cannot board
 *   from across the zone), and flies. Source and destination both have to be known, which is the
 *   engine's own rule for a player who is not a taxi cheater, and this code does not exempt itself
 *   from it.
 *
 * Nothing here reimplements flying. Player::ActivateTaxiPathTo does the whole job — cost, mount,
 * faction discount, the spline itself. This module only decides whether to call it.
 */

#ifndef TRINITYCORE_PBOT_FLIGHT_H
#define TRINITYCORE_PBOT_FLIGHT_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

class Player;

namespace PbotFlight
{
    // How near a flight point has to be before the bot knows about it. Retail discovers on
    // approach, not on conversation, and a flight master is visible from about this far.
    constexpr float DISCOVER_RANGE = 100.0f;

    // A journey worth flying. Below this, walking is faster than crossing the zone to the flight
    // master, waiting for the mount and landing somewhere that is still not the destination.
    constexpr float WORTH_FLYING = 700.0f;

    // Close enough to the flight master to board.
    constexpr float BOARDING_RANGE = 12.0f;

    // The flight has to leave the bot meaningfully nearer than it started, or it is a scenic tour.
    // A landing that saves less than this is not worth the fare or the walk to the platform.
    constexpr float MIN_SAVING = 500.0f;

    // Give up walking to the flight master after this long and just walk to the destination. Bots
    // strand on unreachable platforms otherwise — the same thing that used to happen with anvils.
    constexpr uint32 BOARDING_BUDGET_MS = 180000;

    // And do not set out for a platform further than this in the first place. The budget above
    // catches a platform that cannot be reached; this catches one that simply cannot be reached IN
    // TIME, which the budget would otherwise rediscover every three minutes for the rest of the
    // bot's life. Sized to the same three minutes at a bot's ordinary pace.
    constexpr float MAX_PLATFORM_WALK = 1200.0f;

    // After a refused boarding, leave it alone for this long.
    //
    // Measured the hard way: a level-20 bot with a purse of exactly zero stood at the Ironforge
    // flight master and asked to fly to Goldshire on every single tick. The engine said no every
    // time, correctly, and the log took SEVEN THOUSAND lines of it in twenty minutes. The fare check
    // below stops that particular bot; this stops the next cause of refusal from doing the same,
    // whatever it turns out to be.
    constexpr uint32 BOARD_RETRY_MS = 300000;

    // Learn any flight point the bot is standing near. Idempotent, and throttled internally — the
    // scan behind it walks every node in the game, which is not a thing to do sixty times a tick.
    void Discover(Player* bot);

    // Grant the flight points a character of this race and level is supposed to start with.
    //
    // Needed because the engine's own InitTaxiNodesForLevel lists the classic races by name and
    // stops. Every allied race, Dracthyr and Earthen falls through it and starts life knowing a
    // single node — measured on the live realm, where 58 of 60 bots knew exactly one. They are not
    // meant to be more isolated than a Human; the list simply was never extended.
    void SeedKnownNodes(Player* bot);

    // Try to make progress toward dest by air. Returns true if the bot is now busy with the attempt
    // — in the air, or walking to the platform — and the caller should not move it this tick.
    // False means flight is not on the cards for this journey and the caller should walk.
    //
    // Takes no state and no tick delta on purpose. The per-bot progress lives behind the GUID, the
    // way personality already does, so that PbotTravel::StepToward can stay a two-argument call and
    // every existing errand gains flight without being edited. Elapsed time comes from the world
    // clock rather than accumulated deltas, which also means a bot that stops asking for a while
    // does not keep an ageing boarding attempt alive.
    bool TryTravel(Player* bot, Position const& dest);

    // Could this bot get to dest by air, if it decided to? Asked BEFORE a destination is chosen,
    // by the code that used to refuse anything past a walking cap.
    //
    // That cap is the reason flight changes anything at all. A turn-in more than 1200 yards away
    // was never offered to the bot as a destination, so it never travelled there, so no flight was
    // ever requested for it — 95 of the 285 rewards waiting on this realm sat behind exactly that.
    // Raising the cap outright would put bots back on nine-thousand-yard marches they cannot
    // finish; asking this instead admits the far ones only when there is a flight to serve them.
    bool CanReach(Player* bot, Position const& dest);

    // Drop a bot's flight state. Called when a bot leaves the world so the table does not grow.
    void Forget(ObjectGuid guid);
}

#endif // TRINITYCORE_PBOT_FLIGHT_H
