/*
 * Companion Bots — Phase 3.1 fake-player (pbot) shared constants (TrinityCore master, retail 12.0.7).
 *
 * Constants only. The pbot classes fabricate REAL Player characters driven server-side with no
 * client attached (DESIGN_PHASE3 SS8). Everything tunable that PbotMgr / PbotAI share lives here so
 * there is a single source of truth. The pbot machinery is deliberately parallel to (not shared
 * with) the Phase 1/2 creature-bot code in bot_common.h: creature bots are Creature/CreatureAI,
 * pbots are Player/(our own)PbotAI. See DESIGN_PHASE3 SS8.1/SS8.6/SS8.7.
 */

#ifndef TRINITYCORE_PBOT_COMMON_H
#define TRINITYCORE_PBOT_COMMON_H

#include "Define.h"

#include <ctime>
#include <limits>

// ---- THE IDLE-KICK SENTINEL (fixes a reproducible server SIGSEGV) ---------------------------
//
// Map::Update calls WorldSession::Update on EVERY player's session; there is no opt-out, so a bot
// session is updated by the engine whether we like it or not. The first statement of that function
// is an idle kick that dereferences m_Socket[CONNECTION_TYPE_REALM] WITHOUT a null check — and a
// bot has no socket. So a bot session must never satisfy IsConnectionIdle(), which is simply
// `m_timeOutTime < now`.
//
// The obvious approach — call ResetTimeOutTime() every tick — was tried and still crashed: it sets
// the timeout to only now+60s (SocketTimeOutTimeActive, which World.cpp divides by 1000), so the
// protection lasts exactly one minute past the last refresh and any tick the refresh does not run
// starts a 60-second fuse. A sentinel far in the future removes the timing dependency entirely:
// one write makes the session permanently non-idle, and re-writing it each tick merely re-asserts
// the same value rather than restarting a countdown.
//
// Halved to leave headroom for any `m_timeOutTime + x` arithmetic elsewhere in the engine.
constexpr time_t PBOT_SESSION_NEVER_IDLE = std::numeric_limits<time_t>::max() / 2;

// Max fake-player bots one owner may have live at once (DESIGN_PHASE3 SS6/SS8, mirrors the Phase 1
// creature-bot cap so the two systems feel consistent to the player).
constexpr uint32 PBOT_MAX_PER_OWNER = 4;

// Server-wide cap on concurrently live pbots (review finding #6): each live pbot is a full Player
// ticked every frame; without a global ceiling there is no operational kill-switch.
//
// Raised from 32 once world population became the point: 20 bots in a battleground left the server
// at "Update time diff: 1", i.e. no measurable tick cost, so 32 was far below what the machine can
// carry. The real cost driver is not the bots' own AI but the map GRIDS they keep active — bots
// scattered across many zones are more expensive than the same number standing together — so treat
// this as a ceiling to measure against, not a target to fill blindly.
constexpr uint32 PBOT_GLOBAL_MAX = 200;

// Delay between two bots of a queued population batch (DESIGN: see PbotMgr::TickPopulateQueue).
//
// Creating a world bot is not cheap work that happens to be slow — it can force the map, its grids,
// its vmaps and its mmap tiles in from disk, all synchronously on the world thread. Thirty of those
// inside a single command tick blocked the world thread for over a minute and TrinityCore's
// anti-freeze watchdog deliberately crashed the server ("World Thread hangs for 60004 ms"). One per
// second keeps the worst tick to a single map load, which the watchdog tolerates easily.
constexpr uint32 PBOT_POPULATE_INTERVAL_MS = 1000;

// ---- travel pacing (PbotAI::TickTravel) ------------------------------------------------------
//
// How long a bot stays put after reaching a destination before choosing the next one. Longer after
// a quest objective than after a plain roam: having walked to where the quest's creatures are, the
// bot needs time to actually kill them, whereas a roam is over the moment it arrives.
constexpr uint32 PBOT_GOAL_QUEST_LINGER_MS = 120000;   // 2 minutes among the objective's mobs
constexpr uint32 PBOT_GOAL_ROAM_LINGER_MS  = 20000;    // brief pause, then move on again

// Retry delay when no destination could be chosen at all (no quest here, no walkable roam point).
constexpr uint32 PBOT_GOAL_RETRY_MS = 15000;

// How long a bot may go with no quest to pursue before it hearths back to its race's home. Long
// enough that a bot between quest hubs does not bounce home the moment it finishes one, short
// enough that a bot in a zone with nothing left for it does not circle it all evening.
constexpr uint32 PBOT_HEARTH_AFTER_MS = 600000;   // 10 minutes

// Longest a bot may spend walking to a chosen destination before abandoning it.
//
// Every "walk to X" behaviour needs one of these. A destination the navmesh will not path to makes
// the behaviour report "busy" every tick while the bot stands perfectly still, which starves
// hunting, gathering and wandering — and that is not a corner case: measured over twenty minutes,
// 50 of 59 bots had moved a median of ZERO yards and 52 of 60 had earned no experience at all.
constexpr uint32 PBOT_GOAL_WALK_BUDGET_MS = 180000;   // 3 minutes for a long cross-zone journey

// Below this greed a bot walks past gathering nodes without stopping — some players never take a
// profession, and a world where every single character mines every vein is its own kind of tell.
constexpr uint8 MIN_GREED_TO_GATHER = 35;

// How often a bot bothers with the mail and the auction house. Slow on purpose: both walk grid
// cells, and neither errand is urgent — an unread letter and an unsold ore stack cost nothing for
// another minute, whereas running these searches every tick for sixty bots costs the world thread.
constexpr uint32 MARKET_INTERVAL_MS = 60000;

// Bot account naming scheme (DESIGN_PHASE3 SS8.4/SS8.5). Each LIVE pbot gets its own dedicated
// account so it maps 1:1 to a socket-less WorldSession exactly like a real player's one-account/
// one-session/one-online-character relationship — the conservative choice that avoids any hidden
// "one active session per account" engine assumption. Usernames are PBOT_ACCOUNT_PREFIX + a decimal
// index; AccountMgr normalises case internally. Must stay <= MAX_ACCOUNT_STR (16) chars including
// the digits, which "PBOT" + an 11-digit index comfortably satisfies.
constexpr char const* PBOT_ACCOUNT_PREFIX = "PBOT";

// Length of the random password minted per bot account. These accounts never authenticate through a
// real client (we construct the WorldSession object directly), so the password is purely to satisfy
// AccountMgr::CreateAccount; it is randomised anyway rather than hardcoded. Must be <= MAX_PASS_STR (16).
constexpr uint32 PBOT_ACCOUNT_PASSWORD_LEN = 12;

// Safety bound on how many username indices AcquireBotAccount() will probe before giving up, so a
// pathological "every candidate already exists / creation keeps failing" state fails loudly instead
// of spinning.
constexpr uint32 PBOT_ACCOUNT_MAX_PROBE = 100000;

// Faction-matched races for MVP (DESIGN_PHASE3 SS8.7): Human for Alliance owners, Orc for Horde.
// Both are original Vanilla races with complete playercreateinfo rows in any base world DB, so
// Player::Create() resolves full starter gear/spells/position for every class with no custom data.
constexpr uint8 PBOT_RACE_HUMAN = 1;
constexpr uint8 PBOT_RACE_ORC   = 2;

// Radial offset (yards) applied per slot so multiple pbots don't spawn stacked on the owner. Same
// idiom as Phase 1 bot summoning (bot_common.h BOT_SUMMON_SPREAD).
constexpr float PBOT_SPAWN_SPREAD = 3.0f;

// Fixed per-slot spawn/follow angles (radians) so up to PBOT_MAX_PER_OWNER bots fan out around the
// owner instead of lining up. Avoid 0 / PI which sit directly in front of / behind the owner.
constexpr float PBOT_SPAWN_ANGLES[PBOT_MAX_PER_OWNER] = { 2.0f, 4.0f, 1.0f, 5.0f };

#endif // TRINITYCORE_PBOT_COMMON_H
