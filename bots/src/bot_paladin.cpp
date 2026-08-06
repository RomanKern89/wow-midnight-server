/*
 * Companion Bots — Paladin kit (MELEE HYBRID role)
 *
 * boss_bot_paladin : public BotAI  (see bot_ai.h / DESIGN.md SS1, SS9).
 * Registered under the ScriptName "boss_bot_paladin" (matches creature_template.ScriptName
 * in DESIGN SS7). Melee dps rotation (Judgment + Crusader Strike) with an emergency direct
 * heal that pre-empts dps, plus out-of-combat resurrect of a dead owner. Only the BotAI
 * interface from DESIGN SS10 is used; every cast goes through the guarded DoCastChecked.
 */

#include "bot_ai.h"
#include "bot_rotations.h"
#include "ScriptMgr.h"
#include "Creature.h"
#include "Player.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "Unit.h"
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Kit spells (VERIFY-AT-RUNTIME per DESIGN SS9 — a stale id is a silent no-op). The combat
    // rotation ids (Judgment / Crusader Strike) live in bot_rotations.cpp; the ids below drive the
    // retained UpdateBotOutOfCombat + interrupt/defensive hooks. SPELL_HOLY_LIGHT / CD_HOLY_LIGHT_MS
    // are intentionally mirrored in bot_rotations.cpp (combat emergency heal); keep both in sync.
    constexpr uint32 SPELL_HOLY_LIGHT      = 82326;  // emergency / top-off direct heal
    constexpr uint32 SPELL_REDEMPTION      = 7328;   // out-of-combat resurrect (dead owner)
    constexpr uint32 SPELL_REBUKE          = 96231;  // interrupt (Phase 2 SS6)
    constexpr uint32 SPELL_DIVINE_SHIELD   = 642;    // defensive CD @ 20% HP (Phase 2 SS6)

    // Per-spell cooldowns in ms (GCD emulated separately by DoCastChecked).
    constexpr uint32 CD_HOLY_LIGHT_MS      = 0;      // GCD-gated only, spammable in an emergency
    constexpr uint32 CD_REDEMPTION_MS      = 10000;  // avoid re-spamming resurrect requests

    // Healing search range and thresholds (health %).
    constexpr float  HEAL_RANGE            = 40.0f;
    constexpr float  HEAL_THRESHOLD_OOC    = 80.0f;  // top-off between pulls

    // Rear-positioning (Phase 2 SS3): chase to melee range at M_PI = behind the target's
    // facing. Throttled like the ranged kits since MoveChase adds a generator per call.
    constexpr float  MELEE_CHASE_DIST      = 2.0f;
    constexpr uint32 REPOSITION_INTERVAL_MS = 2000;
}

class boss_bot_paladin : public BotAI
{
public:
    explicit boss_bot_paladin(Creature* creature) noexcept
        : BotAI(creature, BOT_ROLE_MELEE, /*isRanged*/ false) { }

protected:
    // Called by BotAI::UpdateAI only while engaged with a valid combat target. The ladder lives in
    // RunPaladinRotation (bot_rotations.cpp); this adapter preserves the kit's exact heal-target
    // acquisition (HealLowestAlly) and rear-positioning (KeepRearPosition), binding both into the
    // context so the rotation runs them at the same points as before.
    void UpdateBotCombatAI(uint32 diff) override
    {
        Unit* victim = me->GetVictim();

        BotCombatContext ctx{ me, victim, IsAoEMode(),
            [this](Unit* target, uint32 id, uint32 cd) { return DoCastChecked(target, id, cd); } };
        // Lowest-HP living ally in range (owner + group + owner's other bots, self included).
        ctx.HealTarget = HealLowestAlly(HEAL_RANGE, /*includeSelf*/ true);
        // Stand behind the target while meleeing (Phase 2 SS3).
        ctx.Reposition = [this, victim, diff]() { KeepRearPosition(victim, diff); };
        RunPaladinRotation(ctx);
    }

    // --- Phase 2 kit data (SS5/SS6). Interrupt + defensive plumbing is driven by the
    // BotAI base UpdateAI; the kit only supplies the ids/lines.
    uint32 GetInterruptSpellId() const override { return SPELL_REBUKE; }

    std::vector<std::pair<uint8, uint32>> const& GetDefensiveCooldowns() const override
    {
        static std::vector<std::pair<uint8, uint32>> const cds = {
            { uint8(20), SPELL_DIVINE_SHIELD } // Divine Shield at 20% HP
        };
        return cds;
    }

    std::vector<std::string> const& GetFlavorLines() const override
    {
        static std::vector<std::string> const lines = {
            "\xD0\xA1\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBD\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8F.",   // Свет направляет меня.
            "\xD0\x92\xD0\xBE \xD0\xB8\xD0\xBC\xD1\x8F \xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBB\xD0\xB8\xD0\xB2\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8!", // Во имя справедливости!
            "\xD0\x97\xD0\xBB\xD0\xBE \xD0\xB1\xD1\x83\xD0\xB4\xD0\xB5\xD1\x82 \xD0\xBF\xD0\xBE\xD0\xBA\xD0\xB0\xD1\x80\xD0\xB0\xD0\xBD\xD0\xBE.",                     // Зло будет покарано.
            "\xD0\x94\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB1\xD1\x83\xD0\xB4\xD0\xB5\xD1\x82 \xD1\x81 \xD1\x82\xD0\xBE\xD0\xB1\xD0\xBE\xD0\xB9 \xD0\xA1\xD0\xB2\xD0\xB5\xD1\x82.", // Да пребудет с тобой Свет.
            "\xD0\xAF \xD0\xB7\xD0\xB0\xD1\x89\xD0\xB8\xD1\x89\xD0\xB0\xD1\x8E \xD1\x81\xD0\xBB\xD0\xB0\xD0\xB1\xD1\x8B\xD1\x85.",                                   // Я защищаю слабых.
            "\xD0\x92\xD0\xB5\xD1\x80\xD0\xB0 \xD0\xBC\xD0\xBE\xD1\x8F \xD0\xBD\xD0\xB5\xD1\x80\xD1\x83\xD1\x88\xD0\xB8\xD0\xBC\xD0\xB0.",                           // Вера моя нерушима.
            "\xD0\x9E\xD1\x87\xD0\xB8\xD1\x81\xD1\x82\xD0\xB8\xD0\xBC \xD1\x8D\xD1\x82\xD1\x83 \xD0\xB7\xD0\xB5\xD0\xBC\xD0\xBB\xD1\x8E \xD0\xBE\xD1\x82 \xD1\x81\xD0\xBA\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBD\xD1\x8B." // Очистим эту землю от скверны.
        };
        return lines;
    }

private:
    // Re-issue the angled chase only on target change or throttle expiry — MoveChase adds a
    // fresh generator every call, so an unthrottled per-tick call resets movement continuously.
    void KeepRearPosition(Unit* target, uint32 diff)
    {
        if (_repositionMs > diff)
            _repositionMs -= diff;
        else
            _repositionMs = 0;

        if (target->GetGUID() == _chaseTargetGuid && _repositionMs > 0)
            return;

        _chaseTargetGuid = target->GetGUID();
        _repositionMs = REPOSITION_INTERVAL_MS;
        me->GetMotionMaster()->MoveChase(target, MELEE_CHASE_DIST, float(M_PI));
    }

    ObjectGuid _chaseTargetGuid;
    uint32 _repositionMs = 0;

    // Idle logic between pulls: revive a dead owner, otherwise top off wounded allies.
    void UpdateBotOutOfCombat(uint32 /*diff*/) override
    {
        Player* owner = GetOwnerPlayer();
        if (!owner)
            return;

        // Resurrect the owner only when the bot itself is safely out of combat. Guarded by
        // DoCastChecked: if Redemption is renumbered/absent on this build, or the cast is
        // rejected, it is a silent no-op (DESIGN SS9 risk note).
        if (!owner->IsAlive() && !me->IsInCombat())
        {
            DoCastChecked(owner, SPELL_REDEMPTION, CD_REDEMPTION_MS);
            return;
        }

        // Between-pull upkeep. HealLowestAlly already filters out dead candidates, so the
        // dead-owner case above is handled independently.
        if (Unit* wounded = HealLowestAlly(HEAL_RANGE, /*includeSelf*/ true))
            if (wounded->GetHealthPct() < HEAL_THRESHOLD_OOC)
                DoCastChecked(wounded, SPELL_HOLY_LIGHT, CD_HOLY_LIGHT_MS);
    }
};

void AddSC_bot_paladin()
{
    RegisterCreatureAI(boss_bot_paladin);
}
