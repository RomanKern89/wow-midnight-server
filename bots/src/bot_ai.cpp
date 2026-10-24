/*
 * Companion Bots — BotAI implementation (TrinityCore master, retail 12.0.7).
 * See DESIGN.md SS1/SS3/SS4 for the master tick loop, target acquisition, guarded casting,
 * heal-target selection, follow/leash behavior.
 */

#include "bot_ai.h"
#include "bot_mgr.h"
#include "Creature.h"
#include "Player.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "CombatManager.h"
#include "ThreatManager.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SpellDefines.h"
#include "DBCEnums.h"
#include "SharedDefines.h"   // Emote, Language, EMOTE_ONESHOT_*
#include "UnitDefines.h"     // UnitStandStateType, UnitMods, UNIT_STATE_CASTING
#include "Random.h"          // urand
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    // --- Phase 2 tunables (DESIGN_PHASE2 SS2/SS3/SS4c) ---
    constexpr float  BOT_AOE_RANGE            = 10.0f;   // "clustered" radius
    constexpr uint32 BOT_AOE_MIN_ENEMIES      = 3;       // >= this => AoE mode
    constexpr uint32 BOT_AOE_CHECK_MS         = 500;     // enemy-count recheck cadence
    constexpr uint32 BOT_INTERRUPT_CD_MS      = 15000;   // shared interrupt cooldown
    constexpr uint32 BOT_DEFENSIVE_CD_MS      = 60000;   // shared defensive-cooldown cooldown
    constexpr float  BOT_FLEE_HP_PCT          = 15.0f;   // non-tank flees below this
    constexpr uint32 BOT_IDLE_FLAVOR_MIN_MS   = 30000;   // idle emote/say/sit window low
    constexpr uint32 BOT_IDLE_FLAVOR_MAX_MS   = 90000;   // idle emote/say/sit window high
    constexpr uint32 BOT_SEATED_MIN_MS        = 4000;    // post-combat sit duration low
    constexpr uint32 BOT_SEATED_MAX_MS        = 8000;    // post-combat sit duration high
    constexpr float  BOT_ILVL_BASELINE        = 150.0f;  // "typical leveling ilvl" (tuning placeholder)
    constexpr float  BOT_POWER_FACTOR_MIN     = 0.5f;
    constexpr float  BOT_POWER_FACTOR_MAX     = 2.5f;

    // Roll thresholds for TickIdleFlavor (0..99): <40 emote, <70 say, else sit+eat.
    constexpr uint32 BOT_IDLE_ROLL_EMOTE      = 40;
    constexpr uint32 BOT_IDLE_ROLL_SAY        = 70;

    // Idle emote pool (SharedDefines.h enum Emote).
    constexpr std::array<Emote, 4> BOT_IDLE_EMOTES =
        { EMOTE_ONESHOT_BOW, EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_LAUGH };

    // Equipment-slot convention for SetVirtualItem (DESIGN_PHASE2 SS4b).
    constexpr uint32 BOT_SLOT_MAINHAND = 0;
    constexpr uint32 BOT_SLOT_OFFHAND  = 1;
    constexpr uint32 BOT_SLOT_RANGED   = 2;
}

BotAI::BotAI(Creature* creature, BotRole role, bool isRanged) noexcept
    : ScriptedAI(creature), _role(role), _isRanged(isRanged), _slot(0),
      _gcdMs(0), _leashCheckMs(BOT_LEASH_CHECK_MS)
{
}

void BotAI::InitializeBot(ObjectGuid ownerGuid, uint8 slot)
{
    _ownerGuid = ownerGuid;
    _slot = slot;
    _gcdMs = 0;
    _leashCheckMs = BOT_LEASH_CHECK_MS;
    _cooldownsMs.clear();
    _idleFlavorMs = urand(BOT_IDLE_FLAVOR_MIN_MS, BOT_IDLE_FLAVOR_MAX_MS);
    FollowOwner();
    ApplyGearIllusion();   // cosmetic weapons + item-level power scaling (DESIGN_PHASE2 SS4c)
}

// --- hooks -----------------------------------------------------------------------------------

void BotAI::JustAppeared()
{
    // Base may start a follow via GetCharmerOrOwner() if the owner is already set; harmless if
    // not (the summon-ordering window in DESIGN SS2). We re-assert our own slot-angle follow
    // once InitializeBot has run.
    ScriptedAI::JustAppeared();
    if (!_ownerGuid.IsEmpty())
        FollowOwner();
}

void BotAI::EnterEvadeMode(EvadeReason why)
{
    // CreatureAI::EnterEvadeMode stops combat and re-follows the owner via GetCharmerOrOwner()
    // (works here because SetOwnerGUID was set at summon time — DESIGN SS3). We then re-assert
    // our slot-angle follow on top.
    ScriptedAI::EnterEvadeMode(why);
    FollowOwner();
}

void BotAI::JustEngagedWith(Unit* who)
{
    // Record the engagement immediately: the victim's threat list is already cleared by the time
    // PlayerScript::OnCreatureKill fires (Unit::Kill → setDeathState → CombatStop →
    // ClearAllThreat runs first), so a live threat query there always sees an empty list.
    // This note (10s TTL, refreshed in UpdateAI) is what gold-share consults instead (finding #1).
    if (who)
        BotMgr::NoteBotEngagement(_ownerGuid, who->GetGUID());
}

void BotAI::JustDied(Unit* /*killer*/)
{
    // Clear transient cast state. The corpse auto-despawns (TempSummon); the registry slot is
    // reclaimed by the next owner event / RemoveBot.
    _cooldownsMs.clear();
    _gcdMs = 0;
}

void BotAI::OnHealthDepleted(Unit* /*attacker*/, bool /*isKill*/)
{
    // Optional log hook (DESIGN SS1) — intentionally inert for MVP.
}

void BotAI::UpdateAI(uint32 diff)
{
    // Cooldowns/GCD tick down every frame regardless of combat state, so out-of-combat casts
    // (heal upkeep, buffs) see fresh readiness too (DESIGN SS4).
    TickCooldowns(diff);
    TickLeash(diff);

    // UpdateVictim() returns false (and internally triggers evade) when there is no valid
    // combat target. In that case try to acquire one; otherwise run idle logic and follow.
    if (!UpdateVictim())
    {
        if (Unit* target = SelectAttackTarget())
        {
            EngageTarget(target);
            return;
        }
        UpdateBotOutOfCombat(diff);
        FollowOwner();
        SyncMount();           // copy owner's mount/speed while idle (DESIGN_PHASE2 SS1)
        TickIdleFlavor(diff);  // emotes / one-liners / post-combat sit+eat (DESIGN_PHASE2 SS3)
        return;
    }

    // In combat: a seated bot must stand before it can swing/cast (DESIGN_PHASE2 SS3).
    if (_isSeated)
    {
        me->SetStandState(UNIT_STAND_STATE_STAND);
        _isSeated = false;
    }

    // Critical-HP non-tank disengages before attempting anything else this tick.
    if (TryFleeIfCritical())
        return;

    // Refresh AoE-mode flag on a throttle so kits can branch single-target vs AoE.
    if (_aoeCheckMs > diff)
        _aoeCheckMs -= diff;
    else
    {
        _isAoEMode = CountNearbyEnemies(BOT_AOE_RANGE) >= BOT_AOE_MIN_ENEMIES;
        _aoeCheckMs = BOT_AOE_CHECK_MS;
    }

    TryDefensiveCooldowns();
    if (Unit* target = SelectAttackTarget())
    {
        TryInterruptTarget(target);
        // Refresh the engagement note (10s TTL) so the owner's killing blow can be gold-credited
        // even though the victim's threat list is wiped before OnCreatureKill fires (finding #1).
        // Piggybacks the AoE throttle window to avoid a map write every tick.
        if (_aoeCheckMs == BOT_AOE_CHECK_MS)
            BotMgr::NoteBotEngagement(_ownerGuid, target->GetGUID());
    }

    UpdateBotCombatAI(diff);
}

// --- casting ---------------------------------------------------------------------------------

bool BotAI::DoCastChecked(Unit* target, uint32 spellId, uint32 cooldownMs, bool triggered)
{
    if (_gcdMs > 0)
        return false;
    if (!IsSpellReady(spellId))
        return false;
    if (!sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE))
        return false;   // spell not present on this build — degrade silently (DESIGN SS4/SS9)
    if (!target)
        return false;

    if (DoCast(target, spellId, CastSpellExtraArgs(triggered)) != SPELL_CAST_OK)
        return false;

    _cooldownsMs[spellId] = cooldownMs;
    _gcdMs = BOT_GCD_MS;
    return true;
}

bool BotAI::IsSpellReady(uint32 spellId) const
{
    auto it = _cooldownsMs.find(spellId);
    return it == _cooldownsMs.end() || it->second == 0;
}

void BotAI::TickCooldowns(uint32 diff)
{
    _gcdMs = (_gcdMs > diff) ? _gcdMs - diff : 0;
    for (auto& [spellId, remaining] : _cooldownsMs)
        remaining = (remaining > diff) ? remaining - diff : 0;
}

// --- target selection ------------------------------------------------------------------------

Unit* BotAI::SelectAttackTarget() const
{
    if (Player* owner = GetOwnerPlayer())
    {
        // 1. Owner's current victim.
        if (owner->IsInCombat())
            if (Unit* victim = owner->GetVictim())
                if (victim->IsAlive())
                    return victim;

        // 2. Anything in PvE combat with the owner (owner attacked but not yet swinging back).
        for (auto const& [guid, ref] : owner->GetCombatManager().GetPvECombatRefs())
            if (Unit* foe = ref->GetOther(owner))
                if (foe->IsAlive())
                    return foe;
    }

    // 3. Anything already on the bot's own threat list.
    if (Unit* mine = me->GetThreatManager().GetCurrentVictim())
        return mine;

    return nullptr;
}

Unit* BotAI::HealLowestAlly(float range, bool includeSelf) const
{
    Unit* best = nullptr;
    float bestPct = 101.0f;

    auto consider = [&](Unit* u)
    {
        if (!u || !u->IsAlive())
            return;
        if (me->GetExactDist(u) > range)
            return;
        float pct = u->GetHealthPct();
        if (pct < bestPct)
        {
            bestPct = pct;
            best = u;
        }
    };

    if (Player* owner = GetOwnerPlayer())
    {
        consider(owner);

        if (Group* group = owner->GetGroup())
        {
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                if (slot.guid == owner->GetGUID())
                    continue;
                if (Player* member = ObjectAccessor::GetPlayer(*me, slot.guid))
                    consider(member);
            }
        }

        for (ObjectGuid const& botGuid : BotMgr::GetBots(owner))
        {
            if (botGuid == me->GetGUID())
                continue;
            if (Creature* bot = ObjectAccessor::GetCreature(*me, botGuid))
                consider(bot);
        }
    }

    if (includeSelf)
        consider(me);

    return best;
}

// --- owner / movement ------------------------------------------------------------------------

Player* BotAI::GetOwnerPlayer() const
{
    // Map-scoped resolve — the owner should share the bot's map during normal ticks.
    return ObjectAccessor::GetPlayer(*me, _ownerGuid);
}

float BotAI::GetFollowDistance() const
{
    switch (_role)
    {
        case BOT_ROLE_TANK:  return BOT_FOLLOW_DIST_TANK;
        case BOT_ROLE_MELEE: return BOT_FOLLOW_DIST_MELEE;
        default:             return BOT_FOLLOW_DIST_RANGED;  // ranged + healer
    }
}

void BotAI::FollowOwner()
{
    Player* owner = GetOwnerPlayer();
    if (!owner || !owner->IsInWorld())
        return;

    // Only (re)issue the follow if we are not already following — otherwise every idle tick
    // would thrash the motion master.
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        return;

    float angle = BOT_FOLLOW_ANGLES[_slot % MAX_BOTS_PER_PLAYER];
    me->GetMotionMaster()->MoveFollow(owner, GetFollowDistance(), ChaseAngle(angle));
}

void BotAI::EngageTarget(Unit* target)
{
    if (_isRanged)
    {
        // Set the victim (so UpdateVictim() keeps us in the combat branch) without triggering
        // melee chase; keep our distance in the ranged band and let the kit cast.
        if (me->GetVictim() != target)
        {
            me->Attack(target, false);
            me->GetMotionMaster()->MoveChase(target, ChaseRange(BOT_RANGED_CHASE_MIN, BOT_RANGED_CHASE_MAX));
        }
    }
    else
    {
        // Melee / tank: Attack + engine default combat movement chases to melee range.
        AttackStart(target);
    }
}

void BotAI::TickLeash(uint32 diff)
{
    if (_leashCheckMs > diff)
    {
        _leashCheckMs -= diff;
        return;
    }
    _leashCheckMs = BOT_LEASH_CHECK_MS;

    // Global resolve here (not map-scoped): the owner may already be on another map mid-teleport.
    Player* owner = ObjectAccessor::FindPlayer(_ownerGuid);
    if (!owner || !owner->IsInWorld())
    {
        me->DespawnOrUnsummon();   // owner logged out / gone — clean ourselves up
        return;
    }

    if (me->GetMap() != owner->GetMap())
    {
        // Owner changed maps; BotMgr::OnOwnerMapChanged resummons a fresh bot on the new map,
        // so this orphaned copy despawns itself.
        me->DespawnOrUnsummon();
        return;
    }

    if (me->GetExactDist2d(owner) > BOT_LEASH_DISTANCE)
        me->NearTeleportTo(owner->GetPosition());
}

// =============================================================================================
// Phase 2 — human-like behaviors (DESIGN_PHASE2). All spell/item ids are VERIFY-AT-RUNTIME and
// degrade to silent no-ops (guarded casts) or harmless empty visuals, never a crash.
// =============================================================================================

// --- gold share ------------------------------------------------------------------------------

void BotAI::KilledUnit(Unit* victim)
{
    // This bot landed the killing blow; credit the owner (DESIGN_PHASE2 SS4a). Non-creature
    // victims (e.g. a player) are ignored — CreditGoldForKill only handles creatures.
    if (Player* owner = GetOwnerPlayer())
        if (Creature* creature = victim ? victim->ToCreature() : nullptr)
            BotMgr::CreditGoldForKill(owner, creature);
}

// --- combat depth ----------------------------------------------------------------------------

uint32 BotAI::CountNearbyEnemies(float range) const
{
    // Union of "things fighting the owner" and "things attacking this bot" within range. Built
    // from Phase-1-verified combat/threat APIs — no grid search (DESIGN_PHASE2 SS2).
    GuidUnorderedSet seen;

    if (Player* owner = GetOwnerPlayer())
        for (auto const& [guid, ref] : owner->GetCombatManager().GetPvECombatRefs())
            if (Unit* other = ref->GetOther(owner))
                if (other->IsAlive() && other->GetExactDist(me) <= range)
                    seen.insert(other->GetGUID());

    for (Unit* attacker : me->getAttackers())
        if (attacker && attacker->IsAlive() && attacker->GetExactDist(me) <= range)
            seen.insert(attacker->GetGUID());

    return uint32(seen.size());
}

bool BotAI::TryInterruptTarget(Unit* target)
{
    uint32 interruptSpellId = GetInterruptSpellId();  // 0 = kit has no interrupt (Priest/Hunter)
    if (!interruptSpellId || !target)
        return false;

    // Only worth attempting while the target is mid-cast.
    if (!target->HasUnitState(UNIT_STATE_CASTING) && !target->IsNonMeleeSpellCast(false))
        return false;

    return DoCastChecked(target, interruptSpellId, BOT_INTERRUPT_CD_MS);
}

std::vector<std::pair<uint8, uint32>> const& BotAI::GetDefensiveCooldowns() const
{
    static std::vector<std::pair<uint8, uint32>> const empty;
    return empty;
}

void BotAI::TryDefensiveCooldowns()
{
    std::vector<std::pair<uint8, uint32>> const& cds = GetDefensiveCooldowns();
    if (cds.empty())
        return;

    // First threshold we are at/below that has a ready spell wins (self-cast through DoCastChecked
    // so cooldown/GCD/existence guards all apply uniformly — DESIGN_PHASE2 SS2).
    uint8 hpPct = uint8(me->GetHealthPct());
    for (auto const& [threshold, spellId] : cds)
        if (hpPct <= threshold && DoCastChecked(me, spellId, BOT_DEFENSIVE_CD_MS))
            break;
}

// --- human behaviors -------------------------------------------------------------------------

bool BotAI::TryFleeIfCritical()
{
    if (_role == BOT_ROLE_TANK)
        return false;
    if (me->GetHealthPct() > BOT_FLEE_HP_PCT)
        return false;

    Player* owner = GetOwnerPlayer();
    if (!owner || !owner->IsInWorld())
        return false;

    // Disengage and run back to the owner at our slot angle. FollowOwner reissues MoveFollow
    // because the motion master is idle again after Clear() (DESIGN_PHASE2 SS3).
    me->AttackStop();
    me->GetMotionMaster()->Clear();
    FollowOwner();
    return true;
}

void BotAI::TickIdleFlavor(uint32 diff)
{
    // Stand back up once the post-combat sit/eat window elapses (checked every tick, independent
    // of the flavor timer below).
    if (_isSeated)
    {
        if (_seatedMs > diff)
            _seatedMs -= diff;
        else
        {
            me->SetStandState(UNIT_STAND_STATE_STAND);
            _isSeated = false;
        }
    }

    if (_idleFlavorMs > diff)
    {
        _idleFlavorMs -= diff;
        return;
    }
    _idleFlavorMs = urand(BOT_IDLE_FLAVOR_MIN_MS, BOT_IDLE_FLAVOR_MAX_MS);

    if (me->IsInCombat() || !me->IsAlive())
        return;

    uint32 roll = urand(0, 99);
    if (roll < BOT_IDLE_ROLL_EMOTE)
    {
        me->HandleEmoteCommand(BOT_IDLE_EMOTES[urand(0, uint32(BOT_IDLE_EMOTES.size() - 1))]);
    }
    else if (roll < BOT_IDLE_ROLL_SAY)
    {
        std::vector<std::string> const& lines = GetFlavorLines();
        if (!lines.empty())
            me->Say(lines[urand(0, uint32(lines.size() - 1))], LANG_UNIVERSAL);
    }
    else
    {
        TryPostCombatSitDrink();
    }
}

void BotAI::TryPostCombatSitDrink()
{
    if (_isSeated || me->IsInCombat())
        return;

    me->SetStandState(UNIT_STAND_STATE_SIT);
    me->HandleEmoteCommand(EMOTE_ONESHOT_EAT);   // "drink" reuses the eat/drink oneshot pool
    _isSeated = true;
    _seatedMs = urand(BOT_SEATED_MIN_MS, BOT_SEATED_MAX_MS);
}

std::vector<std::string> const& BotAI::GetFlavorLines() const
{
    // Shared default one-liners (Russian, game chat) so every bot has flavor even before a
    // per-class kit overrides this. Plain UTF-8 literals — no creature_text/broadcast_text rows
    // and no localization involvement (deliberately out of scope for MVP, DESIGN_PHASE2 SS3).
    static std::vector<std::string> const lines = {
        "За мной!",
        "Прикрою!",
        "Готов к бою.",
        "Веди, командир.",
        "Держимся вместе.",
        "Славная будет драка!",
        "Я рядом, не бойся.",
        "Не отставай."
    };
    return lines;
}

// --- mounts ----------------------------------------------------------------------------------

void BotAI::SyncMount()
{
    Player* owner = GetOwnerPlayer();
    if (!owner)
        return;

    // Copy the owner's mount display (a plain update-field getter — no aura scan) and match run/
    // flight speed while mounted so the bot keeps up (DESIGN_PHASE2 SS1). Note: this is cosmetic
    // only — it does not grant real flight capability (risk #1).
    bool ownerMounted = owner->IsMounted();
    if (ownerMounted != _wasOwnerMounted)
    {
        if (ownerMounted)
            me->Mount(owner->GetMountDisplayId());
        else
            me->Dismount();
        _wasOwnerMounted = ownerMounted;
    }

    if (ownerMounted)
    {
        me->SetSpeedRate(MOVE_RUN, owner->GetSpeedRate(MOVE_RUN));
        me->SetSpeedRate(MOVE_FLIGHT, owner->GetSpeedRate(MOVE_FLIGHT));
    }
    else
    {
        me->SetSpeedRate(MOVE_RUN, 1.0f);
        me->SetSpeedRate(MOVE_FLIGHT, 1.0f);
    }
}

// --- gear illusion ---------------------------------------------------------------------------

void BotAI::ApplyGearIllusion()
{
    Player* owner = GetOwnerPlayer();
    if (!owner)
        return;

    // Scale visible combat numbers off the owner's average item level via TOTAL_PCT UnitMods —
    // the same primitive Creature::UpdateLevelDependantStats/Pet.cpp use (DESIGN_PHASE2 SS4c).
    // BOT_ILVL_BASELINE is a placeholder tuning constant (risk #4), expect a balance pass.
    float factor = std::clamp(owner->GetAverageItemLevel() / BOT_ILVL_BASELINE,
                              BOT_POWER_FACTOR_MIN, BOT_POWER_FACTOR_MAX);

    me->SetStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, factor);
    me->UpdateMaxHealth();
    me->SetStatPctModifier(UNIT_MOD_DAMAGE_MAINHAND, TOTAL_PCT, factor);
    me->SetStatPctModifier(UNIT_MOD_DAMAGE_OFFHAND, TOTAL_PCT, factor);
    me->SetStatPctModifier(UNIT_MOD_DAMAGE_RANGED, TOTAL_PCT, factor);
    me->UpdateAttackPowerAndDamage(false);
    me->UpdateAttackPowerAndDamage(true);

    SetVirtualItemsForBracket(uint8(owner->GetLevel()));
}

void BotAI::SetVirtualItemsForBracket(uint8 ownerLevel)
{
    // Bracket index 0..4 by owner level: [1-19][20-39][40-59][60-79][80+].
    uint8 b = ownerLevel < 20 ? 0 : ownerLevel < 40 ? 1 : ownerLevel < 60 ? 2 : ownerLevel < 80 ? 3 : 4;

    // Iconic classic-through-Legion weapon item ids per class. PURELY COSMETIC: SetVirtualItem
    // writes a display field only — no Item object, no inventory, no loot (DESIGN_PHASE2 SS4b/SS8).
    // These are NOT server-verified (the server hotfix DB lacks classic weapons); the retail client
    // renders appearance from its own DB2s, and a stale/wrong id harmlessly renders nothing.
    switch (me->GetEntry())
    {
        case BOT_ENTRY_WARRIOR:
        {
            static constexpr uint32 mainhand[5] = {
                25,     // Worn Shortsword
                1728,   // Krol Blade
                19019,  // Thunderfury, Blessed Blade of the Windseeker
                18348,  // Quel'Serrar
                22802   // Iblis, Blade of the Fallen Seraph
            };
            static constexpr uint32 offhand[5] = {
                25,     // Worn Shortsword
                1728,   // Krol Blade
                18348,  // Quel'Serrar
                22802,  // Iblis, Blade of the Fallen Seraph
                19019   // Thunderfury, Blessed Blade of the Windseeker
            };
            me->SetVirtualItem(BOT_SLOT_MAINHAND, mainhand[b]);
            me->SetVirtualItem(BOT_SLOT_OFFHAND, offhand[b]);   // dual-wield fury flavor
            break;
        }
        case BOT_ENTRY_PALADIN:
        {
            static constexpr uint32 mainhand[5] = {
                25,     // Worn Shortsword
                6953,   // Verigan's Fist (paladin class-quest mace)
                17182,  // Sulfuras, Hand of Ragnaros
                17182,  // Sulfuras, Hand of Ragnaros
                17182   // Sulfuras, Hand of Ragnaros
            };
            me->SetVirtualItem(BOT_SLOT_MAINHAND, mainhand[b]);
            break;
        }
        case BOT_ENTRY_PRIEST:
        {
            static constexpr uint32 mainhand[5] = {
                35,     // Bent Staff
                35,     // Bent Staff
                18608,  // Benediction
                18608,  // Benediction
                22589   // Atiesh, Greatstaff of the Guardian
            };
            me->SetVirtualItem(BOT_SLOT_MAINHAND, mainhand[b]);
            break;
        }
        case BOT_ENTRY_MAGE:
        {
            static constexpr uint32 mainhand[5] = {
                35,     // Bent Staff
                35,     // Bent Staff
                22589,  // Atiesh, Greatstaff of the Guardian
                22589,  // Atiesh, Greatstaff of the Guardian
                22589   // Atiesh, Greatstaff of the Guardian
            };
            me->SetVirtualItem(BOT_SLOT_MAINHAND, mainhand[b]);
            break;
        }
        case BOT_ENTRY_HUNTER:
        {
            static constexpr uint32 mainhand[5] = {
                35, 35, 35, 35, 35   // Bent Staff (melee slot; hunter fights from range)
            };
            static constexpr uint32 ranged[5] = {
                2504,   // Worn Shortbow
                2504,   // Worn Shortbow
                18713,  // Rhok'delar, Longbow of the Ancient Keepers
                18713,  // Rhok'delar, Longbow of the Ancient Keepers
                32838   // Thori'dal, the Stars' Fury
            };
            me->SetVirtualItem(BOT_SLOT_MAINHAND, mainhand[b]);
            me->SetVirtualItem(BOT_SLOT_RANGED, ranged[b]);
            break;
        }
        default:
            break;
    }
}
