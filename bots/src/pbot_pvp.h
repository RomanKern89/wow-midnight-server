/*
 * Companion Bots — Phase 7 PvP (TrinityCore master, retail 12.0.7).
 *
 * Two capabilities, both of which turn on the same fact: a bot IS a Player, so every PvP system in
 * the engine already applies to it. Nothing here reimplements PvP — it only supplies the decisions
 * a human would otherwise make with a mouse.
 *
 *   Duels    — a challenged bot accepts. The accept path in the engine lives in a packet handler
 *              (WorldSession::HandleDuelAccepted) and validates an arbiter GUID that only the
 *              client would have echoed back; a bot has no client and no way to learn that GUID
 *              through a public API. So we perform the same state transition the handler performs,
 *              minus the anti-spoof check that exists to guard against a lying client. See
 *              AcceptPendingDuel.
 *
 *   World PvP — a hostile player is a valid thing to attack, and the engine's IsValidAttackTarget
 *              already encodes every rule about when that is true (faction, PvP flags, sanctuaries,
 *              GM state, duels in progress). We ask it rather than re-deriving any of it.
 *
 * Battlegrounds are deliberately NOT here. Queueing bots into a BG needs queue membership, an
 * instance map with a Battleground object behind it, and per-map objective AI — that is its own
 * body of work, not a function in this file.
 */

#ifndef TRINITYCORE_PBOT_PVP_H
#define TRINITYCORE_PBOT_PVP_H

#include "Define.h"

class Player;
class Unit;

namespace PbotPvP
{
    // How far a world bot will look for an enemy player. Shorter than the creature hunt range: a
    // bot that sprints across a zone at the first sight of an enemy reads as an aimbot, not a
    // player.
    constexpr float PVP_SEARCH_RANGE = 25.0f;

    // Completes a duel the bot was challenged to, transitioning both sides into the 3-second
    // countdown exactly as accepting through the client would. Returns false when there is no
    // pending challenge (or the bot is the one who issued it).
    //
    // Called from the AI tick rather than straight from the OnDuelRequest hook: that hook fires
    // from inside the duel spell's effect handler, and mutating both duellists' state while the
    // spell is still executing is a re-entrancy risk for no benefit — one tick later is
    // indistinguishable to a human.
    bool AcceptPendingDuel(Player* bot);

    // Nearest hostile PLAYER the bot is allowed to attack, or nullptr. Used by world bots only;
    // companion bots follow their owner's lead and never open PvP on their own.
    Unit* FindHostilePlayer(Player* bot);
}

#endif // TRINITYCORE_PBOT_PVP_H
