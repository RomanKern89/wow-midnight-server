/*
 * Companion Bots — shared combat rotations (DESIGN_PHASE3 SS9 PART B).
 *
 * Each Run*Rotation body is a mechanical lift of the corresponding bot_<class>.cpp
 * UpdateBotCombatAI priority ladder as it shipped in Phase 1/2 — same spell ids, same
 * thresholds, same cooldown values, same order, same AoE branch. Only the plumbing changed:
 *   me                -> ctx.Self
 *   DoCastChecked(t,) -> ctx.TryCast(t, spellId, cooldownMs)
 *   IsAoEMode()       -> ctx.IsAoEMode
 *   AddThreat(,)      -> ctx.SustainThreat(amount)
 *   HealLowestAlly()  -> ctx.HealTarget   (selected by the caller, B.3)
 *   GetOwnerPlayer()  -> ctx.Owner
 *   KeepRanged/Rear   -> ctx.Reposition() (bound by the caller)
 */

#include "bot_rotations.h"
#include "Unit.h"
#include "ThreatManager.h"
#include <cstddef>

namespace
{
    // ---- Warrior (TANK) — from bot_warrior.cpp ----
    constexpr uint32 SPELL_CHARGE       = 100;    // opener / gap-closer
    constexpr uint32 SPELL_THUNDER_CLAP = 6343;   // AoE threat
    constexpr uint32 SPELL_TAUNT        = 355;    // regain aggro on current target
    constexpr uint32 SPELL_SHIELD_SLAM  = 23922;  // single-target threat filler

    constexpr uint32 CD_CHARGE_MS       = 20000;
    constexpr uint32 CD_THUNDER_CLAP_MS = 6000;
    constexpr uint32 CD_TAUNT_MS        = 8000;
    constexpr uint32 CD_SHIELD_SLAM_MS  = 9000;

    constexpr float  TANK_THREAT_SUSTAIN = 10.0f;
    constexpr std::size_t AOE_THREAT_MIN_TARGETS = 2;

    // ---- Paladin (MELEE HYBRID) — from bot_paladin.cpp ----
    constexpr uint32 SPELL_JUDGMENT        = 20271;  // dps / debuff, on cooldown
    constexpr uint32 SPELL_CRUSADER_STRIKE = 35395;  // melee dps filler
    // SPELL_HOLY_LIGHT / CD_HOLY_LIGHT_MS are duplicated here and in bot_paladin.cpp: the emergency
    // combat heal lives in this rotation, the between-pull top-off lives in the kit's untouched
    // UpdateBotOutOfCombat. Keep both copies in sync (82326 / 0ms).
    constexpr uint32 SPELL_HOLY_LIGHT      = 82326;  // emergency direct heal
    constexpr uint32 CD_JUDGMENT_MS        = 12000;
    constexpr uint32 CD_CRUSADER_STRIKE_MS = 4500;
    constexpr uint32 CD_HOLY_LIGHT_MS      = 0;      // GCD-gated only, spammable in an emergency
    constexpr float  HEAL_THRESHOLD_COMBAT = 50.0f;  // emergency heal that pre-empts dps

    // ---- Priest (HEALER) — from bot_priest.cpp ----
    constexpr uint32 SPELL_FLASH_HEAL = 2061;   // fast direct heal
    constexpr uint32 SPELL_RENEW      = 139;    // HoT upkeep
    constexpr uint32 SPELL_PW_SHIELD  = 17;     // pre-shield / absorb
    constexpr uint32 SPELL_SMITE      = 585;    // dps filler

    constexpr uint32 CD_FLASH_HEAL = 0;
    constexpr uint32 CD_RENEW      = 12000;     // approximates HoT duration
    constexpr uint32 CD_PW_SHIELD  = 15000;     // approximates Weakened Soul lockout
    constexpr uint32 CD_SMITE      = 0;

    constexpr float FLASH_HEAL_PCT   = 50.0f;
    constexpr float RENEW_PCT        = 90.0f;
    constexpr float SHIELD_OWNER_PCT = 90.0f;

    // ---- Mage (RANGED DPS) — from bot_mage.cpp ----
    constexpr uint32 SPELL_FIREBALL  = 133;   // primary filler
    constexpr uint32 SPELL_FROSTBOLT = 116;   // alt filler / slow

    constexpr uint32 CD_FIREBALL  = 0;
    constexpr uint32 CD_FROSTBOLT = 0;

    // ---- Hunter (RANGED DPS) — from bot_hunter.cpp ----
    constexpr uint32 SPELL_ARCANE_SHOT = 185358;  // burst
    constexpr uint32 SPELL_STEADY_SHOT = 56641;   // filler
    constexpr uint32 SPELL_MULTI_SHOT  = 2643;    // AoE (3+ targets)
    constexpr uint32 SPELL_FEIGN_DEATH = 5384;    // defensive: drop threat @ 20% HP

    constexpr uint32 CD_ARCANE_SHOT = 3000;
    constexpr uint32 CD_STEADY_SHOT = 0;
    constexpr uint32 CD_MULTI_SHOT  = 4000;
    constexpr uint32 CD_FEIGN_DEATH = 60000;

    constexpr uint8  FEIGN_DEATH_HP_PCT = 20;     // play dead below this HP%
}

// Tank ladder: sustain threat, close with Charge, AoE/taunt/threat-pump. Caller guarantees a live
// victim (the warrior kit guards !victim || !victim->IsAlive() before building the context).
void RunWarriorRotation(BotCombatContext& ctx)
{
    // Sustain threat every tick while tanking.
    ctx.SustainThreat(TANK_THREAT_SUSTAIN);

    // Out of melee range -> close the gap with Charge (also the opener).
    if (!ctx.Self->IsWithinMeleeRange(ctx.Victim))
    {
        ctx.TryCast(ctx.Victim, SPELL_CHARGE, CD_CHARGE_MS);
        return;
    }

    // Holding multiple attackers -> AoE threat.
    if (ctx.Self->GetThreatManager().GetThreatListSize() >= AOE_THREAT_MIN_TARGETS)
        if (ctx.TryCast(ctx.Victim, SPELL_THUNDER_CLAP, CD_THUNDER_CLAP_MS))
            return;

    // Lost top threat on the current target -> taunt it back.
    if (ctx.Victim->GetThreatManager().GetCurrentVictim() != ctx.Self)
        if (ctx.TryCast(ctx.Victim, SPELL_TAUNT, CD_TAUNT_MS))
            return;

    // Otherwise pump threat: in AoE mode prefer Thunder Clap, else single-target Shield Slam.
    if (ctx.IsAoEMode)
        if (ctx.TryCast(ctx.Victim, SPELL_THUNDER_CLAP, CD_THUNDER_CLAP_MS))
            return;

    ctx.TryCast(ctx.Victim, SPELL_SHIELD_SLAM, CD_SHIELD_SLAM_MS);
}

// Melee hybrid: emergency heal pre-empts dps, then rear-position and Judgment/Crusader Strike.
void RunPaladinRotation(BotCombatContext& ctx)
{
    // Emergency heal takes priority over dps. ctx.HealTarget is the lowest-HP living ally in range
    // (caller's HealLowestAlly); the threshold guard decides whether it is worth a cast.
    if (Unit* wounded = ctx.HealTarget)
        if (wounded->GetHealthPct() < HEAL_THRESHOLD_COMBAT)
            if (ctx.TryCast(wounded, SPELL_HOLY_LIGHT, CD_HOLY_LIGHT_MS))
                return;

    if (!ctx.Victim || !ctx.Victim->IsAlive())
        return;

    // Stand behind the target while meleeing (Phase 2 SS3).
    ctx.Reposition();

    // Judgment on cooldown, then Crusader Strike as the filler.
    if (ctx.TryCast(ctx.Victim, SPELL_JUDGMENT, CD_JUDGMENT_MS))
        return;

    ctx.TryCast(ctx.Victim, SPELL_CRUSADER_STRIKE, CD_CRUSADER_STRIKE_MS);
}

// Healer: hold ranged distance, heal the ally in the most trouble, keep the owner shielded, else
// chip at the enemy. Caller guarantees a non-null victim.
void RunPriestRotation(BotCombatContext& ctx)
{
    ctx.Reposition();

    // 1-2. Heal the ally in the most trouble.
    if (Unit* ally = ctx.HealTarget)
    {
        float const hp = ally->GetHealthPct();
        if (hp < FLASH_HEAL_PCT)
        {
            if (ctx.TryCast(ally, SPELL_FLASH_HEAL, CD_FLASH_HEAL))
                return;
        }
        else if (hp < RENEW_PCT)
        {
            if (ctx.TryCast(ally, SPELL_RENEW, CD_RENEW))
                return;
        }
    }

    // 3. Keep the owner shielded.
    if (ctx.Owner && ctx.Owner->IsAlive() && ctx.Owner->GetHealthPct() < SHIELD_OWNER_PCT)
        if (ctx.TryCast(ctx.Owner, SPELL_PW_SHIELD, CD_PW_SHIELD))
            return;

    // 4. Nobody needs healing — chip at the enemy.
    ctx.TryCast(ctx.Victim, SPELL_SMITE, CD_SMITE);
}

// Ranged nuker: hold distance, Fireball, fall back to Frostbolt. Caller guarantees a non-null victim.
void RunMageRotation(BotCombatContext& ctx)
{
    ctx.Reposition();

    // Fireball first; fall back to Frostbolt if Fireball could not fire.
    if (ctx.TryCast(ctx.Victim, SPELL_FIREBALL, CD_FIREBALL))
        return;

    ctx.TryCast(ctx.Victim, SPELL_FROSTBOLT, CD_FROSTBOLT);
}

// Ranged dps: Feign Death emergency, then Multi-Shot on a cluster, Arcane Shot on cooldown, Steady
// Shot filler. aoeCluster is the caller's "threat list already holds MULTI_SHOT_ENEMY_THRESHOLD"
// signal (kept in the kit adapter). Caller guarantees a non-null victim.
void RunHunterRotation(BotCombatContext& ctx, bool aoeCluster)
{
    // Emergency: below 20% HP, Feign Death to drop threat, then stop swinging so the bot actually
    // plays dead (Phase 2 SS2/SS6).
    if (ctx.Self->GetHealthPct() <= FEIGN_DEATH_HP_PCT)
        if (ctx.TryCast(ctx.Self, SPELL_FEIGN_DEATH, CD_FEIGN_DEATH))
        {
            ctx.Self->AttackStop();
            return;
        }

    ctx.Reposition();

    // AoE when enough enemies are on the bot (base IsAoEMode = 3+ nearby, or the bot's own threat
    // list already holds that many — Phase 2 SS6 Multi-Shot swap).
    if (ctx.IsAoEMode || aoeCluster)
    {
        if (ctx.TryCast(ctx.Victim, SPELL_MULTI_SHOT, CD_MULTI_SHOT))
            return;
    }

    // Burst on cooldown, otherwise fall back to the filler.
    if (ctx.TryCast(ctx.Victim, SPELL_ARCANE_SHOT, CD_ARCANE_SHOT))
        return;

    ctx.TryCast(ctx.Victim, SPELL_STEADY_SHOT, CD_STEADY_SHOT);
}
