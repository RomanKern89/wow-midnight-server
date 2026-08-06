/*
 * Companion Bots — BotAI base class (TrinityCore master, retail 12.0.7).
 *
 * BotAI : public ScriptedAI is the shared base for every companion-bot kit. Per-class kits
 * (boss_bot_warrior, boss_bot_paladin, ...) derive from it and implement UpdateBotCombatAI;
 * they never touch the engine cast/movement APIs directly — everything funnels through the
 * protected helpers here (DoCastChecked, HealLowestAlly, SelectAttackTarget). See DESIGN.md
 * SS1/SS3/SS4/SS10. This header is the frozen interface the kit coders build against.
 */

#ifndef TRINITYCORE_BOT_AI_H
#define TRINITYCORE_BOT_AI_H

#include "ScriptedCreature.h"   // ScriptedAI, CreatureAI hooks, EvadeReason
#include "ObjectGuid.h"
#include "bot_common.h"
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Player;
class Unit;
class Creature;

class BotAI : public ScriptedAI
{
public:
    explicit BotAI(Creature* creature, BotRole role, bool isRanged) noexcept;

    // Called once by BotMgr right after summon + AddToMap. Stores owner/slot and kicks off
    // the explicit follow (DESIGN SS2) — this, not the base JustAppeared, is authoritative.
    void InitializeBot(ObjectGuid ownerGuid, uint8 slot);

    // CreatureAI / ScriptedAI hook overrides (signatures verified against CreatureAI.h).
    void JustAppeared() override;
    void EnterEvadeMode(EvadeReason why = EvadeReason::Other) override;
    void JustEngagedWith(Unit* who) override;
    void JustDied(Unit* killer) override;
    void OnHealthDepleted(Unit* attacker, bool isKill) override;
    void UpdateAI(uint32 diff) override;

    // Phase 2: gold share. Fires when THIS bot lands the killing blow (CreatureAI.h:110).
    void KilledUnit(Unit* victim) override;

    // Phase 2: (re)apply cosmetic weapons + item-level power scaling. Called from InitializeBot
    // and from BotMgr's owner-level-changed handler, so it is public (DESIGN_PHASE2 SS4c).
    void ApplyGearIllusion();

protected:
    // Per-class kit hooks.
    virtual void UpdateBotCombatAI(uint32 diff) = 0;      // only while engaged with a live target
    virtual void UpdateBotOutOfCombat(uint32 /*diff*/) {} // optional idle/buff/heal-upkeep logic

    // --- Phase 2: combat depth (per-kit overrides + shared helpers, DESIGN_PHASE2 SS2/SS5) ---
    // Interrupt spell for this kit, 0 = no interrupt (Priest/Hunter). Kits override.
    virtual uint32 GetInterruptSpellId() const { return 0; }
    // (hpPctThreshold, spellId) pairs, checked top-to-bottom, first match wins. Kits override
    // with a function-local static table — returned by const& so the hot combat tick never
    // allocates (review finding #2).
    virtual std::vector<std::pair<uint8, uint32>> const& GetDefensiveCooldowns() const;
    // Class flavor one-liners (Russian, game chat). Base returns a shared default set so bots
    // have flavor even before a kit overrides; kits may override with class-specific lines.
    virtual std::vector<std::string> const& GetFlavorLines() const;

    uint32 CountNearbyEnemies(float range) const;
    bool TryInterruptTarget(Unit* target);
    void TryDefensiveCooldowns();
    bool IsAoEMode() const { return _isAoEMode; }

    // --- Phase 2: human behaviors (DESIGN_PHASE2 SS3) ---
    // Returns true if the bot disengaged this tick (caller should skip the combat rotation).
    bool TryFleeIfCritical();
    void TickIdleFlavor(uint32 diff);
    void TryPostCombatSitDrink();

    // --- Phase 2: mounts (DESIGN_PHASE2 SS1) ---
    void SyncMount();

    // Guarded cast: GCD + per-spell cooldown + spell-exists check. Never call DoCast* raw.
    // Returns true only if the cast actually started (DESIGN SS4).
    bool DoCastChecked(Unit* target, uint32 spellId, uint32 cooldownMs, bool triggered = false);
    bool IsSpellReady(uint32 spellId) const;

    Player* GetOwnerPlayer() const;                       // map-scoped fresh resolve, never cached
    Unit* SelectAttackTarget() const;                     // owner victim / owner attacker / self attacker
    Unit* HealLowestAlly(float range, bool includeSelf) const; // owner + group + owner's bots

    BotRole    _role;
    bool       _isRanged;
    ObjectGuid _ownerGuid;
    uint8      _slot;

private:
    void FollowOwner();
    void EngageTarget(Unit* target);
    void TickLeash(uint32 diff);
    void TickCooldowns(uint32 diff);
    float GetFollowDistance() const;

    // Phase 2: cosmetic weapon visuals for the owner's level bracket (DESIGN_PHASE2 SS4b).
    void SetVirtualItemsForBracket(uint8 ownerLevel);

    std::unordered_map<uint32, uint32> _cooldownsMs;  // spellId -> ms remaining
    uint32 _gcdMs;
    uint32 _leashCheckMs;

    // Phase 2 transient state (DESIGN_PHASE2 SS5).
    bool   _wasOwnerMounted = false;
    bool   _isAoEMode       = false;
    uint32 _aoeCheckMs      = 0;   // throttle CountNearbyEnemies to ~2Hz
    bool   _isSeated        = false;
    uint32 _seatedMs        = 0;
    uint32 _idleFlavorMs    = 0;
};

#endif // TRINITYCORE_BOT_AI_H
