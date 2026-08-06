/*
 * Companion Bots — partying up (TrinityCore master, retail 12.0.7).
 *
 * Two bots can stand three yards apart beating on the same boar and never acknowledge each other.
 * Players do not do that; they group, they heal each other, they pull together. Grouping is also
 * the single change that makes the combat code they already have look intelligent — the healer
 * rotation heals the lowest-health ALLY, and without a group every bot's only ally is itself.
 *
 * Only other world bots are grouped with, never a real player: a party invite nobody asked for is
 * an imposition, and a human joining a bot party should be their decision, not ours.
 */

#ifndef TRINITYCORE_PBOT_GROUP_H
#define TRINITYCORE_PBOT_GROUP_H

#include "Define.h"

class Player;

namespace PbotGroup
{
    // How close two bots must be to notice each other. Roughly "we can see each other working".
    // Raised from 30 after a 25-minute run produced not a single party: the population is spread
    // over forty-odd zones, so two bots within thirty yards of each other essentially never happens.
    // A hundred yards is still "we can see each other working" and it is the range at which two
    // people questing the same hub would actually notice one another.
    constexpr float PARTY_RANGE = 100.0f;

    // Forms or joins a party with a nearby world bot of the same faction. Returns true if anything
    // happened, so the caller can spend the tick on it.
    bool Tick(Player* bot, uint32& cooldownMs, uint32 diff);
}

#endif // TRINITYCORE_PBOT_GROUP_H
