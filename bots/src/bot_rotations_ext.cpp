/*
 * Companion Bots — Phase 4B rotations for the remaining eight classes.
 *
 * Companion piece to bot_rotations.cpp, which holds the five original Phase 1/2 ladders. Split by
 * file rather than appended so neither file grows past the size where a class ladder is easy to
 * find and reason about.
 *
 * Shape of every ladder here, in priority order:
 *   1. self-preservation (defensive cooldown / self-heal at a HP threshold)
 *   2. party support     (heal the wounded ally the caller selected, for the hybrid classes)
 *   3. interrupt         (only for classes that have one, and only while the target is casting)
 *   4. positioning       (ctx.Reposition — ranged hold their band, melee close)
 *   5. AoE branch        (when ctx.IsAoEMode)
 *   6. cooldown ability, then filler
 *
 * SPELL IDS: these are baseline class abilities, but this realm runs retail build 12.0.7.68275 and
 * ids do get reshuffled between expansions. Each one is therefore verified against the running
 * server's spell store (".lookup spell <name>" on the console) rather than trusted from memory —
 * and until an id is verified, DoCastChecked's sSpellMgr guard turns a wrong id into a silently
 * skipped ladder step, so the bot degrades to auto-attack instead of misbehaving.
 */

#include "bot_rotations.h"
#include "Unit.h"

namespace
{
    // ---- Rogue (MELEE DPS) ----
    constexpr uint32 SPELL_SINISTER_STRIKE = 1752;
    constexpr uint32 SPELL_EVISCERATE      = 196819;
    constexpr uint32 SPELL_FAN_OF_KNIVES   = 51723;
    constexpr uint32 SPELL_KICK            = 1766;
    constexpr uint32 SPELL_EVASION         = 5277;
    constexpr uint32 SPELL_CRIMSON_VIAL    = 185311;

    constexpr uint32 CD_EVISCERATE    = 6000;
    constexpr uint32 CD_FAN_OF_KNIVES = 4000;
    constexpr uint32 CD_KICK          = 15000;
    constexpr uint32 CD_EVASION       = 120000;
    constexpr uint32 CD_CRIMSON_VIAL  = 30000;

    // ---- Warlock (RANGED DPS) ----
    constexpr uint32 SPELL_SHADOW_BOLT      = 686;
    constexpr uint32 SPELL_CORRUPTION       = 172;
    constexpr uint32 SPELL_IMMOLATE         = 348;
    constexpr uint32 SPELL_DRAIN_LIFE       = 234153;
    constexpr uint32 SPELL_RAIN_OF_FIRE     = 5740;
    constexpr uint32 SPELL_UNENDING_RESOLVE = 104773;

    constexpr uint32 CD_CORRUPTION       = 14000;   // approximates DoT duration
    constexpr uint32 CD_IMMOLATE         = 15000;   // approximates DoT duration
    constexpr uint32 CD_DRAIN_LIFE       = 8000;
    constexpr uint32 CD_RAIN_OF_FIRE     = 8000;
    constexpr uint32 CD_UNENDING_RESOLVE = 180000;

    // ---- Druid (RANGED CASTER + HEALS) ----
    constexpr uint32 SPELL_WRATH        = 5176;
    constexpr uint32 SPELL_MOONFIRE     = 8921;
    constexpr uint32 SPELL_SUNFIRE      = 93402;
    constexpr uint32 SPELL_REJUVENATION = 774;
    constexpr uint32 SPELL_REGROWTH     = 8936;
    constexpr uint32 SPELL_BARKSKIN     = 22812;

    constexpr uint32 CD_MOONFIRE     = 12000;   // approximates DoT duration
    constexpr uint32 CD_SUNFIRE      = 10000;
    constexpr uint32 CD_REJUVENATION = 12000;
    constexpr uint32 CD_REGROWTH     = 0;
    constexpr uint32 CD_BARKSKIN     = 60000;

    // ---- Shaman (RANGED CASTER + HEALS) ----
    constexpr uint32 SPELL_LIGHTNING_BOLT  = 188196;
    constexpr uint32 SPELL_CHAIN_LIGHTNING = 188443;
    constexpr uint32 SPELL_FLAME_SHOCK     = 188389;
    constexpr uint32 SPELL_HEALING_SURGE   = 8004;
    constexpr uint32 SPELL_WIND_SHEAR      = 57994;
    constexpr uint32 SPELL_ASTRAL_SHIFT    = 108271;

    constexpr uint32 CD_CHAIN_LIGHTNING = 3000;
    constexpr uint32 CD_FLAME_SHOCK     = 15000;   // approximates DoT duration
    constexpr uint32 CD_HEALING_SURGE   = 0;
    constexpr uint32 CD_WIND_SHEAR      = 12000;
    constexpr uint32 CD_ASTRAL_SHIFT    = 90000;

    // ---- Monk (MELEE + HEALS) ----
    constexpr uint32 SPELL_TIGER_PALM         = 100780;
    constexpr uint32 SPELL_BLACKOUT_KICK      = 100784;
    constexpr uint32 SPELL_RISING_SUN_KICK    = 107428;
    constexpr uint32 SPELL_SPINNING_CRANE     = 101546;
    constexpr uint32 SPELL_VIVIFY             = 116670;
    constexpr uint32 SPELL_FORTIFYING_BREW    = 115203;

    constexpr uint32 CD_BLACKOUT_KICK   = 3000;
    constexpr uint32 CD_RISING_SUN_KICK = 10000;
    constexpr uint32 CD_SPINNING_CRANE  = 4000;
    constexpr uint32 CD_VIVIFY          = 0;
    constexpr uint32 CD_FORTIFYING_BREW = 180000;

    // ---- Death Knight (MELEE) ----
    constexpr uint32 SPELL_DEATH_STRIKE        = 49998;
    // Death Coil doubles as the ranged opener. Icy Touch (45477) was the natural pull here, but the
    // selftest reported it MISSING on this build — it is not a baseline ability in modern retail —
    // so the ladder uses the verified 40y Death Coil for both the pull and the resource dump rather
    // than a step that would silently never fire.
    constexpr uint32 SPELL_DEATH_COIL          = 47541;
    constexpr uint32 SPELL_BLOOD_BOIL          = 50842;
    constexpr uint32 SPELL_DEATH_AND_DECAY     = 43265;
    constexpr uint32 SPELL_MIND_FREEZE         = 47528;
    constexpr uint32 SPELL_ICEBOUND_FORTITUDE  = 48792;

    constexpr uint32 CD_DEATH_STRIKE       = 3000;
    constexpr uint32 CD_DEATH_COIL         = 3000;
    constexpr uint32 CD_BLOOD_BOIL         = 7500;
    constexpr uint32 CD_DEATH_AND_DECAY    = 20000;
    constexpr uint32 CD_MIND_FREEZE        = 15000;
    constexpr uint32 CD_ICEBOUND_FORTITUDE = 180000;

    // ---- Demon Hunter (MELEE) ----
    constexpr uint32 SPELL_DEMONS_BITE     = 162243;
    constexpr uint32 SPELL_CHAOS_STRIKE    = 162794;
    constexpr uint32 SPELL_BLADE_DANCE     = 188499;
    constexpr uint32 SPELL_IMMOLATION_AURA = 258920;
    constexpr uint32 SPELL_THROW_GLAIVE    = 185123;
    constexpr uint32 SPELL_DISRUPT         = 183752;
    constexpr uint32 SPELL_BLUR            = 198589;

    constexpr uint32 CD_CHAOS_STRIKE    = 3000;
    constexpr uint32 CD_BLADE_DANCE     = 9000;
    constexpr uint32 CD_IMMOLATION_AURA = 15000;
    constexpr uint32 CD_THROW_GLAIVE    = 9000;
    constexpr uint32 CD_DISRUPT         = 15000;
    constexpr uint32 CD_BLUR            = 60000;

    // ---- Evoker (RANGED + HEALS) ----
    constexpr uint32 SPELL_LIVING_FLAME    = 361469;
    constexpr uint32 SPELL_AZURE_STRIKE    = 362969;
    constexpr uint32 SPELL_DISINTEGRATE    = 356995;
    constexpr uint32 SPELL_FIRE_BREATH     = 357208;
    constexpr uint32 SPELL_EMERALD_BLOSSOM = 355913;
    constexpr uint32 SPELL_OBSIDIAN_SCALES = 363916;
    constexpr uint32 SPELL_QUELL           = 351338;

    constexpr uint32 CD_DISINTEGRATE    = 3000;
    constexpr uint32 CD_FIRE_BREATH     = 28000;
    constexpr uint32 CD_AZURE_STRIKE    = 0;
    constexpr uint32 CD_EMERALD_BLOSSOM = 6000;
    constexpr uint32 CD_OBSIDIAN_SCALES = 90000;
    constexpr uint32 CD_QUELL           = 20000;

    // Shared thresholds. Kept local to this file so tuning the new classes cannot perturb the
    // five shipped ladders in bot_rotations.cpp.
    constexpr float HP_DEFENSIVE_PCT = 35.0f;   // pop a defensive cooldown below this
    constexpr float HP_SELF_HEAL_PCT = 55.0f;   // self-heal below this
    constexpr float HP_ALLY_HEAL_PCT = 65.0f;   // heal a wounded ally below this

    // True when the target is casting something worth cutting short. skipInstant defaults to true
    // inside the engine helper, so instant casts are not treated as interruptible.
    bool TargetIsCasting(Unit* target)
    {
        return target && target->IsNonMeleeSpellCast(false);
    }
}

// Melee dps: bail out with Evasion/Crimson Vial when hurt, interrupt, then AoE or the strike ladder.
void RunRogueRotation(BotCombatContext& ctx)
{
    float const selfHp = ctx.Self->GetHealthPct();
    if (selfHp <= HP_DEFENSIVE_PCT && ctx.TryCast(ctx.Self, SPELL_EVASION, CD_EVASION))
        return;
    if (selfHp <= HP_SELF_HEAL_PCT && ctx.TryCast(ctx.Self, SPELL_CRIMSON_VIAL, CD_CRIMSON_VIAL))
        return;

    if (TargetIsCasting(ctx.Victim) && ctx.TryCast(ctx.Victim, SPELL_KICK, CD_KICK))
        return;

    ctx.Reposition();

    if (ctx.IsAoEMode && ctx.TryCast(ctx.Victim, SPELL_FAN_OF_KNIVES, CD_FAN_OF_KNIVES))
        return;

    // Eviscerate is the finisher; without combo-point visibility we simply pace it on a cooldown
    // and let Sinister Strike fill the gaps. A failed cast (not enough points) costs nothing —
    // DoCastChecked returns false and we fall through to the filler on the same tick.
    if (ctx.TryCast(ctx.Victim, SPELL_EVISCERATE, CD_EVISCERATE))
        return;

    ctx.TryCast(ctx.Victim, SPELL_SINISTER_STRIKE, 0);
}

// Ranged dps: defensive, drain when hurt, keep both DoTs rolling, AoE, then Shadow Bolt filler.
void RunWarlockRotation(BotCombatContext& ctx)
{
    if (ctx.Self->GetHealthPct() <= HP_DEFENSIVE_PCT &&
        ctx.TryCast(ctx.Self, SPELL_UNENDING_RESOLVE, CD_UNENDING_RESOLVE))
        return;

    ctx.Reposition();

    // Drain Life doubles as the warlock's self-heal, so it outranks the DoT upkeep when hurt.
    if (ctx.Self->GetHealthPct() <= HP_SELF_HEAL_PCT &&
        ctx.TryCast(ctx.Victim, SPELL_DRAIN_LIFE, CD_DRAIN_LIFE))
        return;

    if (ctx.IsAoEMode && ctx.TryCast(ctx.Victim, SPELL_RAIN_OF_FIRE, CD_RAIN_OF_FIRE))
        return;

    if (ctx.TryCast(ctx.Victim, SPELL_CORRUPTION, CD_CORRUPTION))
        return;
    if (ctx.TryCast(ctx.Victim, SPELL_IMMOLATE, CD_IMMOLATE))
        return;

    ctx.TryCast(ctx.Victim, SPELL_SHADOW_BOLT, 0);
}

// Ranged caster that also heals: ally heals outrank damage, then DoT upkeep, then Wrath filler.
void RunDruidRotation(BotCombatContext& ctx)
{
    if (ctx.Self->GetHealthPct() <= HP_DEFENSIVE_PCT &&
        ctx.TryCast(ctx.Self, SPELL_BARKSKIN, CD_BARKSKIN))
        return;

    if (Unit* ally = ctx.HealTarget)
    {
        float const hp = ally->GetHealthPct();
        if (hp < HP_SELF_HEAL_PCT && ctx.TryCast(ally, SPELL_REGROWTH, CD_REGROWTH))
            return;
        if (hp < HP_ALLY_HEAL_PCT && ctx.TryCast(ally, SPELL_REJUVENATION, CD_REJUVENATION))
            return;
    }

    ctx.Reposition();

    if (ctx.TryCast(ctx.Victim, SPELL_MOONFIRE, CD_MOONFIRE))
        return;
    if (ctx.IsAoEMode && ctx.TryCast(ctx.Victim, SPELL_SUNFIRE, CD_SUNFIRE))
        return;

    ctx.TryCast(ctx.Victim, SPELL_WRATH, 0);
}

// Ranged caster that also heals: defensive, ally heal, interrupt, AoE chain, then Lightning Bolt.
void RunShamanRotation(BotCombatContext& ctx)
{
    if (ctx.Self->GetHealthPct() <= HP_DEFENSIVE_PCT &&
        ctx.TryCast(ctx.Self, SPELL_ASTRAL_SHIFT, CD_ASTRAL_SHIFT))
        return;

    if (Unit* ally = ctx.HealTarget)
        if (ally->GetHealthPct() < HP_ALLY_HEAL_PCT &&
            ctx.TryCast(ally, SPELL_HEALING_SURGE, CD_HEALING_SURGE))
            return;

    if (TargetIsCasting(ctx.Victim) && ctx.TryCast(ctx.Victim, SPELL_WIND_SHEAR, CD_WIND_SHEAR))
        return;

    ctx.Reposition();

    if (ctx.IsAoEMode && ctx.TryCast(ctx.Victim, SPELL_CHAIN_LIGHTNING, CD_CHAIN_LIGHTNING))
        return;

    if (ctx.TryCast(ctx.Victim, SPELL_FLAME_SHOCK, CD_FLAME_SHOCK))
        return;

    ctx.TryCast(ctx.Victim, SPELL_LIGHTNING_BOLT, 0);
}

// Melee that also heals: brew when hurt, Vivify a wounded ally, AoE spin, then the kick ladder.
void RunMonkRotation(BotCombatContext& ctx)
{
    if (ctx.Self->GetHealthPct() <= HP_DEFENSIVE_PCT &&
        ctx.TryCast(ctx.Self, SPELL_FORTIFYING_BREW, CD_FORTIFYING_BREW))
        return;

    if (Unit* ally = ctx.HealTarget)
        if (ally->GetHealthPct() < HP_ALLY_HEAL_PCT && ctx.TryCast(ally, SPELL_VIVIFY, CD_VIVIFY))
            return;

    ctx.Reposition();

    if (ctx.IsAoEMode && ctx.TryCast(ctx.Victim, SPELL_SPINNING_CRANE, CD_SPINNING_CRANE))
        return;

    if (ctx.TryCast(ctx.Victim, SPELL_RISING_SUN_KICK, CD_RISING_SUN_KICK))
        return;
    if (ctx.TryCast(ctx.Victim, SPELL_BLACKOUT_KICK, CD_BLACKOUT_KICK))
        return;

    ctx.TryCast(ctx.Victim, SPELL_TIGER_PALM, 0);
}

// Melee with a self-heal built into its rotation: Death Strike is both the heal and the filler.
void RunDeathKnightRotation(BotCombatContext& ctx)
{
    if (ctx.Self->GetHealthPct() <= HP_DEFENSIVE_PCT &&
        ctx.TryCast(ctx.Self, SPELL_ICEBOUND_FORTITUDE, CD_ICEBOUND_FORTITUDE))
        return;

    if (TargetIsCasting(ctx.Victim) && ctx.TryCast(ctx.Victim, SPELL_MIND_FREEZE, CD_MIND_FREEZE))
        return;

    // Out of melee reach: pull at range instead of running in silently.
    if (!ctx.Self->IsWithinMeleeRange(ctx.Victim))
    {
        ctx.Reposition();
        ctx.TryCast(ctx.Victim, SPELL_DEATH_COIL, CD_DEATH_COIL);
        return;
    }

    if (ctx.IsAoEMode)
    {
        if (ctx.TryCast(ctx.Victim, SPELL_DEATH_AND_DECAY, CD_DEATH_AND_DECAY))
            return;
        if (ctx.TryCast(ctx.Victim, SPELL_BLOOD_BOIL, CD_BLOOD_BOIL))
            return;
    }

    if (ctx.TryCast(ctx.Victim, SPELL_DEATH_STRIKE, CD_DEATH_STRIKE))
        return;

    ctx.TryCast(ctx.Victim, SPELL_DEATH_COIL, CD_DEATH_COIL);
}

// Melee: Blur when hurt, interrupt, ranged opener, AoE aura/dance, then the strike ladder.
void RunDemonHunterRotation(BotCombatContext& ctx)
{
    if (ctx.Self->GetHealthPct() <= HP_DEFENSIVE_PCT && ctx.TryCast(ctx.Self, SPELL_BLUR, CD_BLUR))
        return;

    if (TargetIsCasting(ctx.Victim) && ctx.TryCast(ctx.Victim, SPELL_DISRUPT, CD_DISRUPT))
        return;

    if (!ctx.Self->IsWithinMeleeRange(ctx.Victim))
    {
        ctx.Reposition();
        ctx.TryCast(ctx.Victim, SPELL_THROW_GLAIVE, CD_THROW_GLAIVE);
        return;
    }

    if (ctx.IsAoEMode)
    {
        if (ctx.TryCast(ctx.Self, SPELL_IMMOLATION_AURA, CD_IMMOLATION_AURA))
            return;
        if (ctx.TryCast(ctx.Victim, SPELL_BLADE_DANCE, CD_BLADE_DANCE))
            return;
    }

    if (ctx.TryCast(ctx.Victim, SPELL_CHAOS_STRIKE, CD_CHAOS_STRIKE))
        return;

    ctx.TryCast(ctx.Victim, SPELL_DEMONS_BITE, 0);
}

// Ranged hybrid: scales when hurt, Emerald Blossom for the party, Quell interrupt, breath/beam,
// Living Flame filler.
void RunEvokerRotation(BotCombatContext& ctx)
{
    if (ctx.Self->GetHealthPct() <= HP_DEFENSIVE_PCT &&
        ctx.TryCast(ctx.Self, SPELL_OBSIDIAN_SCALES, CD_OBSIDIAN_SCALES))
        return;

    if (Unit* ally = ctx.HealTarget)
        if (ally->GetHealthPct() < HP_ALLY_HEAL_PCT &&
            ctx.TryCast(ally, SPELL_EMERALD_BLOSSOM, CD_EMERALD_BLOSSOM))
            return;

    if (TargetIsCasting(ctx.Victim) && ctx.TryCast(ctx.Victim, SPELL_QUELL, CD_QUELL))
        return;

    ctx.Reposition();

    // Fire Breath is the cone/AoE cooldown; Azure Strike also cleaves, so it covers the AoE gap
    // while Fire Breath is down.
    if (ctx.IsAoEMode)
    {
        if (ctx.TryCast(ctx.Victim, SPELL_FIRE_BREATH, CD_FIRE_BREATH))
            return;
        if (ctx.TryCast(ctx.Victim, SPELL_AZURE_STRIKE, CD_AZURE_STRIKE))
            return;
    }

    if (ctx.TryCast(ctx.Victim, SPELL_DISINTEGRATE, CD_DISINTEGRATE))
        return;

    ctx.TryCast(ctx.Victim, SPELL_LIVING_FLAME, 0);
}
