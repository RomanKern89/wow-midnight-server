/*
 * Companion Bots — moving house when a bot outgrows its zone (TrinityCore master, retail 12.0.7).
 *
 * A world bot quests, fights and levels up, and then stays in its starting zone forever. That is
 * the last obviously non-player thing about it: a real character clears a zone and moves on. This
 * module is the "moves on" half — once the bot's level has run past the level band of the zone it
 * is standing in, it picks a zone that fits and travels there.
 *
 * Two constraints shape the whole design:
 *   - the destination must be on an ALREADY LOADED map. Pulling a fresh continent in for one bot
 *     costs gigabytes, and loading maps is what has taken this server down more than once.
 *   - the check is cheap and rare. It is a zone lookup and a level comparison, once a minute.
 */

#ifndef TRINITYCORE_PBOT_MIGRATE_H
#define TRINITYCORE_PBOT_MIGRATE_H

#include "Define.h"
#include "Position.h"

class Player;

namespace PbotMigrate
{
    // How far past a zone's band a bot must be before it moves on. Small enough that a bot does not
    // linger for ten levels in a starter zone, large enough that it is not re-homed every level.
    constexpr uint8 OUTGROWN_BY = 6;

    // Drives one bot's "should I move house" check. Returns true when the bot was actually sent
    // somewhere, in which case the caller must reset its per-behaviour state — the movement order,
    // gather node, questgiver and corpse it was walking to all belong to the zone it just left.
    //
    // home/homeMapId are the bot's wander anchor, updated in place on a successful move.
    bool Tick(Player* bot, uint32& cooldownMs, uint32 diff, Position& home, uint32& homeMapId);
}

#endif // TRINITYCORE_PBOT_MIGRATE_H
