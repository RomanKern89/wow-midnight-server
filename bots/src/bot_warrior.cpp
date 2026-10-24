/*
 * Companion Bots — Warrior kit (TANK role)
 *
 * boss_bot_warrior : public BotAI  (see bot_ai.h / DESIGN.md SS1, SS9).
 * Registered under the ScriptName "boss_bot_warrior" (matches creature_template.ScriptName
 * in DESIGN SS7). Only the public/protected BotAI interface from DESIGN SS10 is used here;
 * every cast is funnelled through BotAI::DoCastChecked (guarded, GCD + per-spell cooldown).
 */

#include "bot_ai.h"
#include "bot_rotations.h"
#include "ScriptMgr.h"
#include "Creature.h"
#include "ThreatManager.h"
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Kit spells retained by this file (the combat-rotation ids live in bot_rotations.cpp now).
    // VERIFY-AT-RUNTIME per DESIGN SS9 — a stale id degrades to a silent no-op, never a crash.
    constexpr uint32 SPELL_PUMMEL       = 6552;   // interrupt (Phase 2 SS6)
    constexpr uint32 SPELL_SHIELD_WALL  = 871;    // defensive CD @ 30% HP (Phase 2 SS6)
}

class boss_bot_warrior : public BotAI
{
public:
    explicit boss_bot_warrior(Creature* creature) noexcept
        : BotAI(creature, BOT_ROLE_TANK, /*isRanged*/ false) { }

protected:
    // Called by BotAI::UpdateAI only while engaged with a valid combat target. The tank ladder
    // itself lives in RunWarriorRotation (bot_rotations.cpp); this adapter binds the guarded cast
    // and the threat sustain to this kit's BotAI helpers and hands over a valid victim.
    void UpdateBotCombatAI(uint32 /*diff*/) override
    {
        Unit* victim = me->GetVictim();
        if (!victim || !victim->IsAlive())
            return;

        BotCombatContext ctx{ me, victim, IsAoEMode(),
            [this](Unit* target, uint32 id, uint32 cd) { return DoCastChecked(target, id, cd); } };
        ctx.SustainThreat = [this, victim](float amount) { AddThreat(victim, amount); };
        RunWarriorRotation(ctx);
    }

    // --- Phase 2 kit data (SS5/SS6). Interrupt + defensive plumbing is driven by the
    // BotAI base UpdateAI; kits only supply the ids/lines. No rear positioning: the tank
    // must stay in front of its target to hold aggro.
    uint32 GetInterruptSpellId() const override { return SPELL_PUMMEL; }

    std::vector<std::pair<uint8, uint32>> const& GetDefensiveCooldowns() const override
    {
        static std::vector<std::pair<uint8, uint32>> const cds = {
            { uint8(30), SPELL_SHIELD_WALL } // Shield Wall at 30% HP
        };
        return cds;
    }

    std::vector<std::string> const& GetFlavorLines() const override
    {
        static std::vector<std::string> const lines = {
            "\xD0\x97\xD0\xB0 \xD0\x9E\xD1\x80\xD0\xB4\xD1\x83!",                                                                     // За Орду!
            "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xBA\xD1\x80\xD1\x8B\xD0\xB2\xD0\xB0\xD0\xB9 \xD1\x81\xD0\xBF\xD0\xB8\xD0\xBD\xD1\x83.",       // Прикрывай спину.
            "\xD0\x94\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB8 \xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9!",                                       // Держи строй!
            "\xD0\x9A\xD1\x80\xD0\xBE\xD0\xB2\xD1\x8C \xD0\xB8 \xD1\x81\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C.",                              // Кровь и сталь.
            "\xD0\x9C\xD0\xBE\xD1\x8F \xD1\x81\xD0\xB5\xD0\xBA\xD0\xB8\xD1\x80\xD0\xB0 \xD0\xB6\xD0\xB0\xD0\xB6\xD0\xB4\xD0\xB5\xD1\x82 \xD0\xB1\xD0\xBE\xD1\x8F.", // Моя секира жаждет боя.
            "\xD0\xA1\xD1\x82\xD0\xBE\xD0\xB9 \xD0\xB7\xD0\xB0 \xD0\xBC\xD0\xBD\xD0\xBE\xD0\xB9, \xD1\x8F \xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD1\x83 \xD1\x83\xD0\xB4\xD0\xB0\xD1\x80.", // Стой за мной, я приму удар.
            "\xD0\x92\xD1\x80\xD0\xB0\xD0\xB3 \xD0\xBD\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB9\xD0\xB4\xD1\x91\xD1\x82.",               // Враг не пройдёт.
            "\xD0\x92 \xD0\xB0\xD1\x82\xD0\xB0\xD0\xBA\xD1\x83, \xD0\xBD\xD0\xB5 \xD0\xBE\xD1\x82\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xB0\xD0\xB9!" // В атаку, не отставай!
        };
        return lines;
    }
};

void AddSC_bot_warrior()
{
    RegisterCreatureAI(boss_bot_warrior);
}
