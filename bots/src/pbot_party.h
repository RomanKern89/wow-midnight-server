/*
 * Companion Bots — Phase 5 party integration (TrinityCore master, retail 12.0.7).
 *
 * Two things have to be true before a bot behaves like a real group member rather than a follower
 * that happens to cast spells:
 *
 *   1. It must be IN the owner's group. Group membership is what makes the engine share quest
 *      credit, loot rolls and — the reason this exists — experience. Without it a bot can fight
 *      beside the owner for hours and never gain a level, which is the single most visible way the
 *      illusion breaks.
 *   2. It must start at the owner's level. A level-1 bot next to a level-70 owner is not a
 *      companion, it is a corpse: it cannot survive a single hit from anything the owner fights,
 *      and it can never earn its way up because it dies before the kill credit lands.
 *
 * Both are done through the engine's own APIs (Group::Create/AddMember, Player::GiveLevel) rather
 * than by writing fields directly, so the bot ends up in exactly the state a real character would.
 */

#ifndef TRINITYCORE_PBOT_PARTY_H
#define TRINITYCORE_PBOT_PARTY_H

#include "Define.h"

class Player;

namespace PbotParty
{
    // Puts the bot in the owner's group, creating the group (owner as leader) if there is none.
    // Returns false when the bot could not be added — most often because the owner's group is
    // already full of real players. That is not a spawn failure: the bot still works, it just
    // will not receive shared experience, so the caller reports it rather than aborting.
    bool JoinOwnerGroup(Player* owner, Player* bot);

    // Removes the bot from whatever group it is in. Safe to call when it is in none.
    // Called on both dismiss and retire so a squad's worth of offline bots can never squat the
    // owner's five party slots.
    void LeaveGroup(Player* bot);

    // Raises a freshly created bot to the owner's level and teaches it everything a character of
    // that level should know (default skills, specialization spells, talent tier state).
    //
    // Deliberately one-directional and only ever called at spawn: after that the bot earns its
    // levels from shared experience like a real member. Levelling it down is never correct — a
    // character's level is not a display property, and lowering it would strip spells the bot may
    // already be mid-cast with.
    void SyncLevelToOwner(Player* owner, Player* bot);
}

#endif // TRINITYCORE_PBOT_PARTY_H
