/*
 * Companion Bot kit — Hunter (RANGED DPS)
 * See DESIGN.md SS9 (class kits) and SS10 (BotAI interface).
 *
 * Role: stay at 20-35yd from the combat target and fire.
 *   - Multi-Shot when 3+ enemies are engaged with the bot (AoE).
 *   - Arcane Shot on cooldown (burst).
 *   - Steady Shot as the default filler.
 *
 * Enemy count uses the bot's own threat list size (Unit::GetThreatManager),
 * a standard engine accessor — DESIGN SS10 exposes no dedicated enemy-count
 * helper, so this is the closest in-interface approximation of "3+ enemies".
 */

#include "bot_ai.h"
#include "bot_rotations.h"
#include "ScriptMgr.h"
#include "Creature.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "Unit.h"
#include "ThreatManager.h"
#include <string>
#include <vector>

namespace
{
    // The shot/Feign-Death ladder ids + HP threshold live in bot_rotations.cpp now. This kit retains
    // the ranged-positioning constants (KeepRangedPosition) and the Multi-Shot cluster threshold,
    // which the adapter evaluates and passes to RunHunterRotation as its aoeCluster argument.
    constexpr float RANGE_MIN = 20.0f;
    constexpr float RANGE_MAX = 35.0f;

    constexpr std::size_t MULTI_SHOT_ENEMY_THRESHOLD = 3;
    constexpr uint32 REPOSITION_INTERVAL_MS = 2000;
}

class boss_bot_hunter : public BotAI
{
public:
    explicit boss_bot_hunter(Creature* creature) noexcept
        : BotAI(creature, BOT_ROLE_RANGED, /*isRanged*/ true) { }

protected:
    // The shot/Feign-Death ladder lives in RunHunterRotation (bot_rotations.cpp); this adapter binds
    // the guarded cast and the kit's ranged positioning, and evaluates the Multi-Shot cluster signal
    // (the bot's own threat-list size) to pass as aoeCluster — the base IsAoEMode 3+-nearby signal
    // rides on the context.
    void UpdateBotCombatAI(uint32 diff) override
    {
        Unit* victim = me->GetVictim();
        if (!victim)
            return;

        bool const aoeCluster = me->GetThreatManager().GetThreatListSize() >= MULTI_SHOT_ENEMY_THRESHOLD;

        BotCombatContext ctx{ me, victim, IsAoEMode(),
            [this](Unit* target, uint32 id, uint32 cd) { return DoCastChecked(target, id, cd); } };
        ctx.Reposition = [this, victim, diff]() { KeepRangedPosition(victim, diff); };
        RunHunterRotation(ctx, aoeCluster);
    }

    // --- Phase 2 kit data (SS5/SS6). No interrupt (base default 0); no GetDefensiveCooldowns
    // override — the Feign Death + AttackStop special case lives in UpdateBotCombatAI above.
    std::vector<std::string> const& GetFlavorLines() const override
    {
        static std::vector<std::string> const lines = {
            "\xD0\xA6\xD0\xB5\xD0\xBB\xD1\x8C \xD0\xBD\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xB8\xD1\x86\xD0\xB5\xD0\xBB\xD0\xB5.",                                 // Цель на прицеле.
            "\xD0\x9E\xD0\xB4\xD0\xB8\xD0\xBD \xD0\xB2\xD1\x8B\xD1\x81\xD1\x82\xD1\x80\xD0\xB5\xD0\xBB \xE2\x80\x94 \xD0\xBE\xD0\xB4\xD0\xB8\xD0\xBD \xD1\x82\xD1\x80\xD1\x83\xD0\xBF.", // Один выстрел — один труп.
            "\xD0\xA2\xD0\xB8\xD1\x85\xD0\xBE. \xD0\xA1\xD0\xBB\xD1\x83\xD1\x88\xD0\xB0\xD0\xB9 \xD0\xBB\xD0\xB5\xD1\x81.",                                 // Тихо. Слушай лес.
            "\xD0\x9C\xD0\xBE\xD0\xB9 \xD0\xB7\xD0\xB2\xD0\xB5\xD1\x80\xD1\x8C \xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xB2\xD0\xB5\xD0\xB4\xD1\x91\xD1\x82.", // Мой зверь не подведёт.
            "\xD0\x94\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB8 \xD0\xB4\xD0\xB8\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD1\x86\xD0\xB8\xD1\x8E.",                           // Держи дистанцию.
            "\xD0\xA1\xD1\x82\xD1\x80\xD0\xB5\xD0\xBB\xD0\xB0 \xD0\xB1\xD1\x8B\xD1\x81\xD1\x82\xD1\x80\xD0\xB5\xD0\xB5 \xD1\x81\xD0\xBB\xD0\xBE\xD0\xB2.",   // Стрела быстрее слов.
            "\xD0\x93\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2." // Готов.
        };
        return lines;
    }

private:
    // See bot_priest.cpp — MoveChase adds a generator per call, so throttle it.
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

void AddSC_bot_hunter()
{
    RegisterCreatureAI(boss_bot_hunter);
}
