/*
 * Companion Bot kit — Priest (HEALER)
 * See DESIGN.md SS9 (class kits) and SS10 (BotAI interface).
 *
 * Role: keep owner + allies alive. Priority ladder each combat tick:
 *   1. Flash Heal the lowest ally under 50% (urgent direct heal)
 *   2. Renew the lowest ally under 90% (HoT maintenance)
 *   3. Power Word: Shield the owner when unshielded / hurt
 *   4. Smite the current enemy as a dps filler when nobody needs healing
 * Positioning: stay at ranged distance from the combat target so the priest
 * never wanders into melee (DESIGN's healer-positioning guidance).
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
#include <vector>

namespace
{
    // The heal / shield / Smite ids + thresholds live in bot_rotations.cpp now. This kit retains
    // only the heal-search range (feeds HealLowestAlly in the adapter) and the ranged-positioning
    // constants used by KeepRangedPosition.
    constexpr float HEAL_RANGE = 40.0f;

    constexpr float RANGE_MIN = 20.0f;
    constexpr float RANGE_MAX = 30.0f;

    constexpr uint32 REPOSITION_INTERVAL_MS = 2000;
}

class boss_bot_priest : public BotAI
{
public:
    explicit boss_bot_priest(Creature* creature) noexcept
        : BotAI(creature, BOT_ROLE_HEALER, /*isRanged*/ true) { }

protected:
    // The heal/shield/Smite ladder lives in RunPriestRotation (bot_rotations.cpp); this adapter
    // preserves the kit's exact heal-target (HealLowestAlly), owner (GetOwnerPlayer) and ranged
    // positioning (KeepRangedPosition), feeding all three into the context.
    void UpdateBotCombatAI(uint32 diff) override
    {
        Unit* victim = me->GetVictim();
        if (!victim)
            return;

        BotCombatContext ctx{ me, victim, IsAoEMode(),
            [this](Unit* target, uint32 id, uint32 cd) { return DoCastChecked(target, id, cd); } };
        ctx.Reposition = [this, victim, diff]() { KeepRangedPosition(victim, diff); };
        ctx.HealTarget = HealLowestAlly(HEAL_RANGE, /*includeSelf*/ true);
        ctx.Owner = GetOwnerPlayer();
        RunPriestRotation(ctx);
    }

    // --- Phase 2 kit data (SS5/SS6). Priest has no interrupt/defensive-CD/AoE override
    // this phase — only gentle-healer flavor lines.
    std::vector<std::string> const& GetFlavorLines() const override
    {
        static std::vector<std::string> const lines = {
            "\xD0\x94\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB8\xD1\x82\xD0\xB5\xD1\x81\xD1\x8C, \xD1\x8F \xD0\xBB\xD0\xB5\xD1\x87\xD1\x83 \xD0\xB2\xD0\xB0\xD1\x81.",                       // Держитесь, я лечу вас.
            "\xD0\xA1\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xB8\xD1\x81\xD1\x86\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x82 \xD0\xB2\xD0\xB0\xD1\x88\xD0\xB8 \xD1\x80\xD0\xB0\xD0\xBD\xD1\x8B.",         // Свет исцелит ваши раны.
            "\xD0\x9D\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xB9\xD1\x82\xD0\xB5\xD1\x81\xD1\x8C, \xD1\x8F \xD1\x80\xD1\x8F\xD0\xB4\xD0\xBE\xD0\xBC.",                                       // Не бойтесь, я рядом.
            "\xD0\x94\xD1\x8B\xD1\x88\xD0\xB8\xD1\x82\xD0\xB5, \xD0\xB2\xD1\x81\xD1\x91 \xD0\xB1\xD1\x83\xD0\xB4\xD0\xB5\xD1\x82 \xD1\x85\xD0\xBE\xD1\x80\xD0\xBE\xD1\x88\xD0\xBE.", // Дышите, всё будет хорошо.
            "\xD0\xAF \xD0\xBE\xD0\xB1\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB3\xD0\xB0\xD1\x8E \xD0\xB2\xD0\xB0\xD1\x81.",                                                               // Я оберегаю вас.
            "\xD0\xA0\xD0\xB0\xD0\xBD\xD1\x8B \xD0\xB7\xD0\xB0\xD1\x82\xD1\x8F\xD0\xBD\xD1\x83\xD1\x82\xD1\x81\xD1\x8F, \xD0\xBF\xD0\xBE\xD1\x82\xD0\xB5\xD1\x80\xD0\xBF\xD0\xB8\xD1\x82\xD0\xB5.", // Раны затянутся, потерпите.
            "\xD0\xA1\xD0\xBF\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xB9\xD1\x81\xD1\x82\xD0\xB2\xD0\xB8\xD0\xB5, \xD1\x8F \xD1\x81 \xD0\xB2\xD0\xB0\xD0\xBC\xD0\xB8." // Спокойствие, я с вами.
        };
        return lines;
    }

private:
    // Re-issue MoveChase only when the target changes or the throttle expires;
    // MoveChase adds a fresh generator every call, so an unthrottled per-tick
    // call would reset movement continuously.
    void KeepRangedPosition(Unit* target, uint32 diff)
    {
        if (_repositionMs > diff)
            _repositionMs -= diff;
        else
            _repositionMs = 0;

        if (target->GetGUID() == _chaseTargetGuid && _repositionMs > 0)
            return;

        _chaseTargetGuid = target->GetGUID();
        _repositionMs = REPOSITION_INTERVAL_MS;
        me->GetMotionMaster()->MoveChase(target, ChaseRange(RANGE_MIN, RANGE_MAX));
    }

    ObjectGuid _chaseTargetGuid;
    uint32 _repositionMs = 0;
};

void AddSC_bot_priest()
{
    RegisterCreatureAI(boss_bot_priest);
}
