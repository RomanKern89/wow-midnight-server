/*
 * Companion Bots — shared constants & enums (TrinityCore master, retail 12.0.7).
 *
 * This header is the single source of truth for the bot class/role enums, the
 * creature-template entry ids, and every tunable used across the core and the
 * per-class kits. See DESIGN.md SS0/SS1/SS2/SS7.
 */

#ifndef TRINITYCORE_BOT_COMMON_H
#define TRINITYCORE_BOT_COMMON_H

#include "Define.h"

// Player-facing bot classes. The order here matches the creature entries below and the
// `.bot add <class>` token parsing in cs_bot.cpp.
enum BotClass : uint8
{
    BOT_CLASS_WARRIOR = 0,
    BOT_CLASS_PALADIN = 1,
    BOT_CLASS_PRIEST  = 2,
    BOT_CLASS_MAGE    = 3,
    BOT_CLASS_HUNTER  = 4,
    BOT_CLASS_MAX
};

// Combat role, chosen by each kit's constructor (drives follow distance + engage movement).
enum BotRole : uint8
{
    BOT_ROLE_TANK   = 0,
    BOT_ROLE_MELEE  = 1,
    BOT_ROLE_RANGED = 2,
    BOT_ROLE_HEALER = 3
};

// creature_template entries (DESIGN SS7). Reserved custom range 9000001-9000005.
constexpr uint32 BOT_ENTRY_WARRIOR = 9000001;
constexpr uint32 BOT_ENTRY_PALADIN = 9000002;
constexpr uint32 BOT_ENTRY_PRIEST  = 9000003;
constexpr uint32 BOT_ENTRY_MAGE    = 9000004;
constexpr uint32 BOT_ENTRY_HUNTER  = 9000005;

// Registry limit (requirement #10 / DESIGN SS0).
constexpr uint32 MAX_BOTS_PER_PLAYER = 4;

// Follow / leash tunables (DESIGN SS2).
constexpr float  BOT_LEASH_DISTANCE = 60.0f;  // >this from owner -> near-teleport back
constexpr uint32 BOT_LEASH_CHECK_MS = 1000;   // leash/owner-validity poll cadence
constexpr float  BOT_SUMMON_SPREAD  = 3.0f;   // radial summon offset so bots don't stack
constexpr uint32 BOT_GCD_MS         = 1500;   // emulated global cooldown (DESIGN SS4)

// Per-role follow distance (DESIGN SS2).
constexpr float  BOT_FOLLOW_DIST_TANK   = 2.5f;
constexpr float  BOT_FOLLOW_DIST_MELEE  = 3.0f;
constexpr float  BOT_FOLLOW_DIST_RANGED = 6.0f;  // ranged + healer stay out of melee

// Ranged/healer chase band while engaged (DESIGN SS3).
constexpr float  BOT_RANGED_CHASE_MIN = 20.0f;
constexpr float  BOT_RANGED_CHASE_MAX = 30.0f;

// Fixed per-slot follow angles (radians) so up-to-4 bots fan out around the owner instead of
// stacking. Avoid 0 / PI which line up directly behind/in front (DESIGN SS2).
constexpr float  BOT_FOLLOW_ANGLES[MAX_BOTS_PER_PLAYER] = { 2.0f, 4.0f, 1.0f, 5.0f };

#endif // TRINITYCORE_BOT_COMMON_H
