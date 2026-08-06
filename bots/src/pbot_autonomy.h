/*
 * Companion Bots — Phase 6 autonomous behaviour (TrinityCore master, retail 12.0.7).
 *
 * A companion bot is defined by its owner: it follows them, fights what they fight, and is torn
 * down when they log out. A WORLD bot has no owner at all. It is spawned into a place and left to
 * behave like a player who happens to be questing there — pick a fight it can win, fight it, sit
 * down and recover, wander, get up when it dies, repeat.
 *
 * That difference is exactly one thing structurally: an empty _ownerGuid. Everything the companion
 * path derives from the owner (where to stand, what to attack, when to disengage) needs a
 * replacement, and those replacements live here so PbotAI's companion logic stays untouched and
 * readable.
 *
 * Scope of this first pass, deliberately: fight, recover, wander, self-resurrect. No questing, no
 * vendors, no travel between zones. Those need systems that do not exist yet, and a bot that does
 * these four things convincingly is worth far more than one that half-does eight.
 */

#ifndef TRINITYCORE_PBOT_AUTONOMY_H
#define TRINITYCORE_PBOT_AUTONOMY_H

#include "Define.h"

class Player;
class Unit;
class Position;

namespace PbotAutonomy
{
    // Radius a world bot looks in for something to fight. Kept modest: the search visits grid
    // cells, and a wide radius on every idle tick for every world bot is the one thing in this
    // file that could actually cost server performance.
    // Raised from 40 after measuring the consequence: 58 of 60 bots had gained XP over 25 minutes
    // but not one had levelled. Nothing was broken — they simply were not finding enough to kill.
    // A player clearing a zone engages things further away than the length of a house.
    constexpr float SEARCH_RANGE = 60.0f;

    // How far a bot will drift from the spot it was spawned at. Bounded so a populated zone stays
    // populated instead of slowly draining into whatever direction the random walk favoured.
    constexpr float WANDER_RADIUS = 35.0f;

    // Below this health fraction a bot out of combat sits down and recovers instead of looking for
    // the next fight — the single behaviour that most makes a bot read as "a player", because it
    // is what a player does.
    //
    // Lowered from 55/90 for the same reason SEARCH_RANGE went up: sitting from half health until
    // nearly full is most of a bot's day, and a bot sitting down is a bot not earning experience.
    // A player takes the next pull at two thirds health; only a bad fight sends them to the ground.
    constexpr float REST_BELOW_HEALTH_PCT = 35.0f;

    // Recover up to this before considering itself ready again.
    constexpr float RESUME_ABOVE_HEALTH_PCT = 70.0f;

    // Picks something worth attacking within SEARCH_RANGE, or nullptr.
    //
    // Filters hard on purpose: only creatures (players are Phase 7 territory), only ones the bot is
    // actually allowed to attack, never critters, never elites or bosses, and never anything more
    // than a few levels above the bot. A bot that walks into a world boss because it was the
    // nearest hostile does not look alive, it looks broken.
    // range defaults to SEARCH_RANGE; callers pass a personality-scaled value so that how far a bot
    // will go looking for a fight is part of who it is rather than a global constant.
    Unit* FindTarget(Player* bot, float range = SEARCH_RANGE);

    // How many creatures within range pass the same filter FindTarget applies. Purely diagnostic,
    // and the one number that separates "there is nothing here to kill" from "it kills slowly" —
    // two causes of a low levelling rate that look identical from the outside.
    uint32 CountTargets(Player* bot, float range);

    // Why FindTarget rejected everything nearby, as counts per reason. Without this a zero is
    // unattributable: too high level, elite, critter, already fighting and friendly all produce
    // exactly the same silence.
    struct RejectionTally
    {
        uint32 Considered = 0;
        uint32 TooHighLevel = 0;
        uint32 Elite = 0;
        uint32 Critter = 0;
        uint32 AlreadyInCombat = 0;
        uint32 NotAttackable = 0;
        uint32 Accepted = 0;
    };
    RejectionTally ExplainTargets(Player* bot, float range);

    // Sends the bot on a random short walk around its home position. No-op when it is already
    // moving somewhere.
    void Wander(Player* bot, Position const& home);

    // Sits down to regenerate. Idempotent — safe to call every tick while resting.
    void Rest(Player* bot);

    // Stands back up, if sitting. Called before anything that needs the bot on its feet.
    void StopResting(Player* bot);

    // Brings a dead world bot back after its death timer, since there is no client to release and
    // run a corpse back. Returns true while the bot is still dead (caller should do nothing else
    // this tick).
    //
    // Revives it AT ITS HOME ANCHOR, not where it fell. Reviving on the spot looks reasonable and
    // is badly wrong in practice: the bot comes back next to whatever just killed it, on half
    // health, and dies again — observed live as bots sitting at 0% health for minutes on end,
    // stuck in a death loop. Home is by definition somewhere it survived long enough to be spawned.
    // How many deaths in a row at the same place before the bot is moved somewhere it can survive.
    //
    // Measured: 619 deaths against 36 kills in half an hour, and they were not spread — one bot died
    // 104 times, another 71, all in a handful of high-level zones. Reviving at the home anchor was
    // supposed to break a death loop, but when the anchor itself sits in lethal country it CLOSES
    // the loop instead: die, revive, die again, every seventeen seconds.
    constexpr uint8 DEATHS_BEFORE_RELOCATING = 3;

    // Deaths further apart than this are unrelated bad luck, not a loop.
    constexpr uint32 LOOP_WINDOW_MS = 120000;

    // Takes the bot's home anchor by reference: escaping a death loop means moving house, and the
    // anchor has to move with the bot or it will simply walk back to where it kept dying.
    bool HandleDeath(Player* bot, uint32 diff, uint32& deathTimerMs, Position& home, uint32& homeMapId,
        uint8& consecutiveDeaths, uint32& sinceLastDeathMs);

    // --- Breaking off a fight that is already lost -------------------------------------------

    // Health below which a bot starts thinking about leaving. Scaled per bot by Caution, so the
    // timid break off early and the reckless push on — the same fight is not the same decision for
    // everyone.
    constexpr float RETREAT_BELOW_HEALTH_PCT = 30.0f;

    // Never run from something this close to dead. Fleeing a mob on its last legs throws away the
    // kill, the loot and the experience, and is the single most obviously wrong thing a fleeing bot
    // can do.
    constexpr float FINISH_TARGET_BELOW_PCT = 25.0f;

    // How far to get away before considering itself out of it.
    constexpr float RETREAT_DISTANCE = 70.0f;

    // A retreat that has not worked by now is not going to; stop running and let the bot fight or
    // die rather than sprint across the zone forever.
    constexpr uint32 RETREAT_BUDGET_MS = 15000;

    // Is this fight lost? Deliberately narrow — three conditions must hold at once, because a bot
    // that runs from fights it would have won levels slower and looks cowardly rather than clever.
    bool ShouldRetreat(Player* bot, Unit* victim);

    // Disengage and run. Returns true while still running away.
    bool Retreat(Player* bot, uint32& retreatMs, uint32 diff);
}

#endif // TRINITYCORE_PBOT_AUTONOMY_H
