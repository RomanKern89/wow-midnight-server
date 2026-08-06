/*
 * Companion Bot kit — Mage (RANGED DPS)
 * See DESIGN.md SS9 (class kits) and SS10 (BotAI interface).
 *
 * Role: stay at 20-30yd from the combat target and nuke.
 *   - Fireball is the primary filler.
 *   - Frostbolt is the alternate filler used when Fireball is unavailable.
 */

#include "bot_ai.h"
#include "bot_rotations.h"
#include "ScriptMgr.h"
#include "Creature.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "Unit.h"
#include <string>
#include <utility>
#include <vector>

namespace
{
    // VERIFY-AT-RUNTIME spell ids (DESIGN SS9). The nuke ids (Fireball / Frostbolt) live in
    // bot_rotations.cpp now; this kit retains the interrupt/defensive ids and the ranged-
    // positioning constants used by KeepRangedPosition.
    constexpr uint32 SPELL_COUNTERSPELL = 2139;  // interrupt (Phase 2 SS6)
    constexpr uint32 SPELL_ICE_BLOCK    = 45438; // defensive CD @ 15% HP (Phase 2 SS6)

    constexpr float RANGE_MIN = 20.0f;
    constexpr float RANGE_MAX = 30.0f;

    constexpr uint32 REPOSITION_INTERVAL_MS = 2000;
}

class boss_bot_mage : public BotAI
{
public:
    explicit boss_bot_mage(Creature* creature) noexcept
        : BotAI(creature, BOT_ROLE_RANGED, /*isRanged*/ true) { }

protected:
    // The nuke ladder lives in RunMageRotation (bot_rotations.cpp); this adapter binds the guarded
    // cast and the kit's ranged positioning (KeepRangedPosition) into the context.
    void UpdateBotCombatAI(uint32 diff) override
    {
        Unit* victim = me->GetVictim();
        if (!victim)
            return;

        BotCombatContext ctx{ me, victim, IsAoEMode(),
            [this](Unit* target, uint32 id, uint32 cd) { return DoCastChecked(target, id, cd); } };
        ctx.Reposition = [this, victim, diff]() { KeepRangedPosition(victim, diff); };
        RunMageRotation(ctx);
    }

    // --- Phase 2 kit data (SS5/SS6). Interrupt + defensive plumbing is driven by the
    // BotAI base UpdateAI; the kit only supplies the ids/lines.
    uint32 GetInterruptSpellId() const override { return SPELL_COUNTERSPELL; }

    std::vector<std::pair<uint8, uint32>> const& GetDefensiveCooldowns() const override
    {
        static std::vector<std::pair<uint8, uint32>> const cds = {
            { uint8(15), SPELL_ICE_BLOCK } // Ice Block at 15% HP
        };
        return cds;
    }

    std::vector<std::string> const& GetFlavorLines() const override
    {
        static std::vector<std::string> const lines = {
            "\xD0\x9C\xD0\xB0\xD0\xB3\xD0\xB8\xD1\x8F \xE2\x80\x94 \xD1\x8D\xD1\x82\xD0\xBE \xD0\xBD\xD0\xB5 \xD1\x84\xD0\xBE\xD0\xBA\xD1\x83\xD1\x81, \xD1\x8D\xD1\x82\xD0\xBE \xD0\xBD\xD0\xB0\xD1\x83\xD0\xBA\xD0\xB0.", // Магия — это не фокус, это наука.
            "\xD0\xA2\xD1\x8B \xD1\x83\xD0\xB6\xD0\xB5 \xD0\xB3\xD0\xBE\xD1\x80\xD0\xB8\xD1\x88\xD1\x8C? \xD0\x9D\xD0\xB5\xD1\x82? \xD0\xA1\xD0\xB5\xD0\xB9\xD1\x87\xD0\xB0\xD1\x81 \xD0\xB8\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8E.", // Ты уже горишь? Нет? Сейчас исправлю.
            "\xD0\x97\xD0\xB0\xD0\xBC\xD1\x91\xD1\x80\xD0\xB7\xD0\xBD\xD0\xB5\xD1\x88\xD1\x8C \xE2\x80\x94 \xD0\xBD\xD0\xB5 \xD0\xB6\xD0\xB0\xD0\xBB\xD1\x83\xD0\xB9\xD1\x81\xD1\x8F.", // Замёрзнешь — не жалуйся.
            "\xD0\x97\xD0\xBD\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5 \xE2\x80\x94 \xD1\x81\xD0\xB8\xD0\xBB\xD0\xB0, \xD0\xB0 \xD0\xBE\xD0\xB3\xD0\xBE\xD0\xBD\xD1\x8C \xE2\x80\x94 \xD0\xB5\xD1\x89\xD1\x91 \xD0\xB1\xD0\xBE\xD0\xBB\xD1\x8C\xD1\x88\xD0\xB0\xD1\x8F.", // Знание — сила, а огонь — ещё большая.
            "\xD0\x9E, \xD1\x82\xD1\x8B \xD0\xB2\xD1\x81\xD1\x91 \xD0\xB5\xD1\x89\xD1\x91 \xD0\xB6\xD0\xB8\xD0\xB2? \xD0\x97\xD0\xB0\xD0\xB1\xD0\xB0\xD0\xB2\xD0\xBD\xD0\xBE.", // О, ты всё ещё жив? Забавно.
            "\xD0\xA0\xD0\xB0\xD1\x81\xD1\x81\xD1\x82\xD1\x83\xD0\xBF\xD0\xB8\xD1\x81\xD1\x8C, \xD0\xB3\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB9 \xD0\xB7\xD0\xB0 \xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB9.", // Расступись, гений за работой.
            "\xD0\x9D\xD0\xB5\xD0\xBC\xD0\xBD\xD0\xBE\xD0\xB3\xD0\xBE \xD0\xBE\xD0\xB3\xD0\xBD\xD1\x8F \xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB2\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82... \xD1\x82\xD0\xB5\xD0\xB1\xD0\xB5-\xD1\x82\xD0\xBE \xD0\xBF\xD0\xBE\xD0\xB2\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82." // Немного огня не повредит... тебе-то повредит.
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

void AddSC_bot_mage()
{
    RegisterCreatureAI(boss_bot_mage);
}
