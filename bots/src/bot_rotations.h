/*
 * Companion Bots — shared combat rotations (DESIGN_PHASE3 SS9 PART B).
 *
 * The per-class priority ladders live here as caster-agnostic free functions so BOTH the
 * Phase 1/2 creature kits (boss_bot_warrior, ...) and the Phase 3.2 fake-player bots (PbotAI)
 * drive combat from ONE source of truth. A caller builds a BotCombatContext, binds the cast /
 * threat / positioning callbacks to whatever primitives it already owns, and calls the matching
 * Run*Rotation. Behavior (spell ids, thresholds, order, AoE branch) is identical to the ladders
 * previously inlined in each bot_<class>.cpp UpdateBotCombatAI.
 *
 * NOTE on the context shape vs the B.2 sketch: B.2 drew TryCast as bool(spellId, cooldownMs) and
 * listed Self/Victim/IsAoEMode/TryCast/SustainThreat + an optional HealTarget (B.3). The real
 * ladders cast on more than one target within a single tick — the priest heals an ally AND Smites
 * the victim, the paladin heals AND meleees the victim, the hunter Feign-Deaths itself AND shoots
 * the victim — so TryCast takes an explicit target (dropping it would change who gets hit, i.e. a
 * behavior change). For the same reason the priest's Power Word: Shield step needs the owner, and
 * positioning is caller-specific (creature kits reposition via KeepRanged/RearPosition, PbotAI via
 * its own B.4 MoveChase); both are exposed as context fields so the full ladder still lives in one
 * place. These are the minimal additions required to keep the extraction behavior-preserving.
 */

#pragma once

#include <cstdint>
#include <functional>

class Unit;

struct BotCombatContext
{
    Unit* Self   = nullptr;
    Unit* Victim = nullptr;
    bool  IsAoEMode = false;

    // Guarded cast, bound by the caller to whichever helper it already has: BotAI::DoCastChecked
    // for creature kits, or PbotAI's SpellHistory-backed one for pbots. Same contract either way:
    // returns true only if the cast actually started. Target is explicit so one rotation can cast
    // on the victim, a heal target, or self.
    std::function<bool(Unit* target, uint32 spellId, uint32 cooldownMs)> TryCast;

    // Tank-only raw-threat sustain; no-op default for non-tank callers, bound to AddThreat by the
    // warrior kit / a ThreatManager::AddThreat for pbots.
    std::function<void(float amount)> SustainThreat = [](float) {};

    // Per-caller positioning, invoked at the exact point the kit ladder repositioned. Creature kits
    // bind KeepRangedPosition / KeepRearPosition; pbots bind their B.4 ranged/melee chase. No-op for
    // callers that do not reposition (the warrior tank never leaves melee).
    std::function<void()> Reposition = [] {};

    // Healer heal target (priest / paladin), selected by the caller before invoking the rotation
    // (B.3), mirroring BotAI::HealLowestAlly / PbotAI's own equivalent.
    Unit* HealTarget = nullptr;

    // Owner reference for the priest's Power Word: Shield upkeep step.
    Unit* Owner = nullptr;
};

void RunWarriorRotation(BotCombatContext& ctx);
void RunPaladinRotation(BotCombatContext& ctx);
void RunPriestRotation(BotCombatContext& ctx);   // heals use ctx.HealTarget / owner shield uses ctx.Owner (B.3)
void RunMageRotation(BotCombatContext& ctx);
void RunHunterRotation(BotCombatContext& ctx, bool aoeCluster);

// --- Phase 4B: the remaining eight classes (bot_rotations_ext.cpp) ---------------------------
//
// Same contract as above: the caller guarantees a live ctx.Victim and binds TryCast/Reposition.
// These ladders are deliberately shallower than a player's real rotation — they cover the opener,
// a defensive, an interrupt where the class has one, an AoE branch and a filler. Depth belongs in
// a later pass once these are confirmed working in the world; a shallow ladder that always does
// something sensible beats a deep one built on unverified assumptions.
//
// Every spell id used by these is guarded by DoCastChecked's sSpellMgr lookup, so an id that does
// not exist on this build degrades to "skip this step", never to a crash.
void RunRogueRotation(BotCombatContext& ctx);
void RunWarlockRotation(BotCombatContext& ctx);
void RunDruidRotation(BotCombatContext& ctx);          // heals use ctx.HealTarget
void RunShamanRotation(BotCombatContext& ctx);         // heals use ctx.HealTarget
void RunMonkRotation(BotCombatContext& ctx);           // heals use ctx.HealTarget
void RunDeathKnightRotation(BotCombatContext& ctx);
void RunDemonHunterRotation(BotCombatContext& ctx);
void RunEvokerRotation(BotCombatContext& ctx);         // heals use ctx.HealTarget
