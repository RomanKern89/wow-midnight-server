/*
 * Companion Bots — riding (TrinityCore master, retail 12.0.7).
 *
 * Bots now walk hundreds of yards to reach a quest objective, and they do it on foot at running
 * pace, which no player above level 20 has done since 2004. A mounted traveller is one of the most
 * recognisable silhouettes in the game; a party of characters jogging cross-country is not.
 *
 * The mount is chosen from the client's own Mount table rather than a list of ours, so it stays
 * correct as expansions add mounts, and the speed bonus is applied alongside it — a mounted model
 * moving at walking pace would look worse than no mount at all.
 */

#ifndef TRINITYCORE_PBOT_MOUNT_H
#define TRINITYCORE_PBOT_MOUNT_H

#include "Define.h"

class Player;

namespace PbotMount
{
    // Distance beyond which a journey is worth mounting for. Below it, mounting and dismounting
    // again is more conspicuous than just walking.
    constexpr float WORTH_MOUNTING = 80.0f;

    // Puts the bot on a mount if it is not already on one. Silently does nothing in combat.
    void MountUp(Player* bot);

    // Takes the bot off its mount and restores its speed. Safe to call when not mounted.
    void Dismount(Player* bot);
}

#endif // TRINITYCORE_PBOT_MOUNT_H
