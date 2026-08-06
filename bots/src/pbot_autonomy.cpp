/*
 * Companion Bots — Phase 6 autonomous behaviour implementation.
 * See pbot_autonomy.h for the scope of this first pass.
 */

#include "pbot_autonomy.h"

#include "pbot_personality.h"   // Caution decides how early this bot gives up on a fight
#include "pbot_world_spots.h"   // somewhere level-appropriate to escape a death loop to

#include <cmath>

#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "Player.h"
#include "Random.h"
#include "Unit.h"
#include "UnitDefines.h"

namespace
{
    // How far above the bot's own level a creature may be and still be considered a fair fight.
    //
    // Zero on purpose. A bot is levelled up to match its owner (or the spawn argument) but still
    // wears the level-1 starter kit that Player::Create gave it, so it is far weaker than its level
    // suggests — level-20 bots sent to a level-20 zone were dying repeatedly. Until bots can equip
    // gear, "same level or lower" is the honest bar; raising this again only makes sense alongside
    // real gear.
    constexpr uint8 MAX_LEVEL_ABOVE = 0;

    // ...and how far below, before it is not worth the walk.
    constexpr uint8 MAX_LEVEL_BELOW = 8;

    // Seconds a dead world bot lies there before getting back up. Long enough that death reads as
    // a real setback rather than a flicker.
    constexpr uint32 DEATH_RECOVERY_MS = 12000;

    // MovePoint id for wander legs. Any non-zero constant works; it only distinguishes our points
    // from other systems' in movement-inform callbacks we do not use.
    constexpr uint32 WANDER_POINT_ID = 0xB07;
    constexpr uint32 RETREAT_POINT_ID = 0xB08;   // distinct, so a flee is not mistaken for a wander

    // The creature filter described in the header. Written as a searcher check rather than a
    // post-filter so the grid visit itself rejects candidates and we never build a list.
    class FairFightCreatureCheck
    {
    public:
        FairFightCreatureCheck(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive())
                return false;

            if (!_bot->IsWithinDist(creature, _range))
                return false;

            // ★★★ DO NOT USE IsValidAttackTarget HERE.
            //
            // It answers a stricter question than "may I attack this": among other things it runs
            // CanSeeOrDetect, and for a socket-less bot that returns false for EVERY creature in
            // the world. The consequence was total and invisible — measured across many runs, up to
            // 2362 creatures in range and 0 acceptable, every one rejected, while the bots kept
            // reporting "in combat" merely because things were attacking THEM.
            //
            // Unit::Attack does not apply that gate: probed on a live server, IsValidAttackTarget
            // said no and canSee said no for the same creature that Attack then accepted with TRUE.
            // So the prey filter asks the question that actually matters — is it hostile, alive and
            // touchable — and lets Attack be the authority on the rest.
            if (_bot->IsFriendlyTo(creature) || creature->IsFriendlyTo(_bot))
                return false;

            if (creature->IsImmuneToPC() || creature->IsUninteractible())
                return false;

            if (creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
                return false;

            // Critters are scenery. Attacking them is the classic tell of a broken bot.
            if (creature->IsCritter())
                return false;

            // Elites and bosses are group content; a lone bot picking one up dies for nothing and
            // then dies again the moment it resurrects next to it.
            if (creature->IsElite() || creature->isWorldBoss())
                return false;

            // Do not steal someone else's fight.
            if (creature->IsInCombat())
                return false;

            uint8 const botLevel = _bot->GetLevel();
            uint8 const mobLevel = creature->GetLevel();
            if (mobLevel > botLevel + MAX_LEVEL_ABOVE)
                return false;
            if (botLevel > mobLevel + MAX_LEVEL_BELOW)
                return false;

            return true;
        }

    private:
        Player const* _bot;
        float _range;
    };
}

namespace
{
    // Collector that runs the same filter as FairFightCreatureCheck but records WHY each candidate
    // was rejected, instead of just answering yes or no.
    class TargetExplainer
    {
    public:
        TargetExplainer(Player const* bot, float range, PbotAutonomy::RejectionTally& tally)
            : _bot(bot), _range(range), _tally(tally) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive() || !_bot->IsWithinDist(creature, _range))
                return false;

            ++_tally.Considered;

            // Mirrors the live filter above — see the note there on why IsValidAttackTarget is not
            // the right question for a client-less bot.
            if (_bot->IsFriendlyTo(creature) || creature->IsFriendlyTo(_bot)
                || creature->IsImmuneToPC() || creature->IsUninteractible()
                || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE)) { ++_tally.NotAttackable; return false; }
            if (creature->IsCritter())                { ++_tally.Critter;          return false; }
            if (creature->IsElite() || creature->isWorldBoss()) { ++_tally.Elite;  return false; }
            if (creature->IsInCombat())               { ++_tally.AlreadyInCombat;  return false; }

            uint8 const botLevel = _bot->GetLevel();
            uint8 const mobLevel = creature->GetLevel();
            if (mobLevel > botLevel + MAX_LEVEL_ABOVE) { ++_tally.TooHighLevel; return false; }
            if (botLevel > mobLevel + MAX_LEVEL_BELOW) { ++_tally.TooHighLevel; return false; }

            ++_tally.Accepted;
            return false;   // never actually "find" anything; this only counts
        }

    private:
        Player const* _bot;
        float _range;
        PbotAutonomy::RejectionTally& _tally;
    };
}

uint32 PbotAutonomy::CountTargets(Player* bot, float range)
{
    return ExplainTargets(bot, range).Accepted;
}

PbotAutonomy::RejectionTally PbotAutonomy::ExplainTargets(Player* bot, float range)
{
    RejectionTally tally;
    if (!bot || !bot->IsInWorld())
        return tally;

    Creature* ignored = nullptr;
    TargetExplainer explainer(bot, range, tally);
    Trinity::CreatureLastSearcher<TargetExplainer> searcher(bot, ignored, explainer);
    Cell::VisitAllObjects(bot, searcher, range);

    return tally;
}

Unit* PbotAutonomy::FindTarget(Player* bot, float range)
{
    if (!bot || !bot->IsInWorld())
        return nullptr;

    Creature* found = nullptr;
    FairFightCreatureCheck check(bot, range);
    Trinity::CreatureLastSearcher<FairFightCreatureCheck> searcher(bot, found, check);
    Cell::VisitAllObjects(bot, searcher, range);

    return found;
}

void PbotAutonomy::Wander(Player* bot, Position const& home)
{
    if (!bot)
        return;

    // Already walking somewhere — let it finish rather than re-rolling a destination every tick,
    // which would leave the bot vibrating in place.
    MovementGeneratorType const current = bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (current == POINT_MOTION_TYPE || current == CHASE_MOTION_TYPE || current == FOLLOW_MOTION_TYPE)
        return;

    float const angle = frand(0.0f, 2.0f * float(M_PI));
    float const distance = frand(5.0f, WANDER_RADIUS);

    float x = home.GetPositionX() + distance * std::cos(angle);
    float y = home.GetPositionY() + distance * std::sin(angle);
    float z = home.GetPositionZ();

    // Snap the destination onto the actual ground/liquid surface. Without this a point picked on
    // flat maths can sit inside a hill or float above a cliff, and the bot either refuses to move
    // or walks a nonsense path to it.
    bot->UpdateAllowedPositionZ(x, y, z);

    bot->GetMotionMaster()->MovePoint(WANDER_POINT_ID, x, y, z);
}

void PbotAutonomy::Rest(Player* bot)
{
    if (!bot)
        return;

    // Stop moving first: sitting while a movement generator is active makes the bot slide along
    // the ground in a seated pose.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
        bot->GetMotionMaster()->Clear();

    if (bot->GetStandState() != UNIT_STAND_STATE_SIT)
        bot->SetStandState(UNIT_STAND_STATE_SIT);
}

void PbotAutonomy::StopResting(Player* bot)
{
    if (!bot)
        return;

    if (bot->GetStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);
}

bool PbotAutonomy::ShouldRetreat(Player* bot, Unit* victim)
{
    if (!bot || !victim || !bot->IsAlive() || !victim->IsAlive())
        return false;

    // Running inside a battleground abandons the objective, and duels end by yielding, not fleeing.
    if (bot->InBattleground() || bot->duel)
        return false;

    // Finish what is nearly finished. A kill one hit away is worth more than a whole skin.
    if (victim->GetHealthPct() < FINISH_TARGET_BELOW_PCT)
        return false;

    // A cautious bot leaves at a third of its health, a reckless one only at the very end.
    float const threshold = PbotPersonality::Scale(
        PbotPersonality::Of(bot->GetGUID()).Caution, RETREAT_BELOW_HEALTH_PCT, 0.5f, 1.6f);

    if (bot->GetHealthPct() > threshold)
        return false;

    // The decisive test, and the reason this is not simply "flee when hurt": is the bot LOSING?
    // Being at 25% health against an enemy at 10% is a fight nearly won, and running from it would
    // be idiotic. Being at 25% against an enemy at 90% is a fight already lost.
    return victim->GetHealthPct() > bot->GetHealthPct();
}

bool PbotAutonomy::Retreat(Player* bot, uint32& retreatMs, uint32 diff)
{
    if (!bot || !bot->IsAlive())
    {
        retreatMs = 0;
        return false;
    }

    if (!retreatMs)
    {
        TC_LOG_INFO("scripts.bots", "pbot retreat: {} (level {}, hp {:.0f}%) breaks off and runs",
            bot->GetName(), uint32(bot->GetLevel()), bot->GetHealthPct());

        bot->AttackStop();
        bot->GetMotionMaster()->Clear();
    }

    retreatMs += diff;

    // Out of it, or out of patience.
    if (!bot->IsInCombat() || retreatMs > RETREAT_BUDGET_MS)
    {
        retreatMs = 0;
        bot->GetMotionMaster()->Clear();
        return false;
    }

    // Keep moving directly away from whatever is closest to killing it. Re-issued only when the
    // previous leg finished, or the motion generator would be restarted every tick and the bot
    // would stand still while "fleeing" — the same mistake that once froze travel.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
    {
        Unit* threat = bot->GetVictim();
        float const angle = threat ? threat->GetAbsoluteAngle(bot) : frand(0.0f, 2.0f * float(M_PI));

        float x = bot->GetPositionX() + std::cos(angle) * RETREAT_DISTANCE;
        float y = bot->GetPositionY() + std::sin(angle) * RETREAT_DISTANCE;
        float z = bot->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);

        bot->GetMotionMaster()->MovePoint(RETREAT_POINT_ID, x, y, z);
    }

    return true;
}

bool PbotAutonomy::HandleDeath(Player* bot, uint32 diff, uint32& deathTimerMs, Position& home,
    uint32& homeMapId, uint8& consecutiveDeaths, uint32& sinceLastDeathMs)
{
    if (!bot)
        return true;

    if (bot->IsAlive())
    {
        deathTimerMs = 0;

        // Survived long enough that whatever killed it is no longer killing it.
        sinceLastDeathMs += diff;
        if (sinceLastDeathMs > LOOP_WINDOW_MS)
            consecutiveDeaths = 0;

        // A living bot must not still be a ghost. The ghost aura gates the engine's visibility mask
        // so that a ghost sees only other ghosts — every creature in the world disappears from it,
        // and since visibility is the FIRST gate in IsValidAttackTarget, such a bot can be attacked
        // but can never choose to attack anything again. It looks healthy the whole time.
        //
        // Whether our revive path can leave this behind is the open question; clearing it here is
        // cheap, idempotent, and turns a permanent failure into at worst one wasted tick.
        if (bot->HasAuraType(SPELL_AURA_GHOST))
            bot->RemoveAurasByType(SPELL_AURA_GHOST);

        return false;
    }

    // First tick of being dead: start the timer, and say so.
    //
    // Deaths were never logged, which quietly poisoned every earlier measurement — a probe grepping
    // for them reported "0 deaths", and that zero was the absence of a log line, not an absence of
    // dying. Without this there is no kill-to-death ratio, and no way to see whether repaired gear
    // actually changes how a fight ends.
    if (!deathTimerMs)
    {
        deathTimerMs = DEATH_RECOVERY_MS;

        if (sinceLastDeathMs <= LOOP_WINDOW_MS)
            ++consecutiveDeaths;
        else
            consecutiveDeaths = 1;
        sinceLastDeathMs = 0;

        TC_LOG_INFO("scripts.bots", "pbot death: {} (level {}) died in zone {} (death {} in a row)",
            bot->GetName(), uint32(bot->GetLevel()), bot->GetZoneId(), uint32(consecutiveDeaths));
    }

    if (deathTimerMs > diff)
    {
        deathTimerMs -= diff;
        return true;
    }

    deathTimerMs = 0;

    // Back to full health at the home anchor — the client-less equivalent of releasing and running
    // back. Reviving in place at partial health (the first version) put the bot straight back under
    // whatever killed it and produced an endless death loop; moving it away is what breaks that.
    bot->ResurrectPlayer(1.0f);
    bot->SpawnCorpseBones();

    // Dying here over and over means this is not a place this bot can live. Moving it back to the
    // anchor only works when the anchor is somewhere survivable; when it is not, the anchor IS the
    // trap. So the bot moves house entirely, to a zone matched to its level.
    if (consecutiveDeaths >= DEATHS_BEFORE_RELOCATING)
    {
        PbotWorldSpots::Spot refuge;
        if (PbotWorldSpots::PickForLevel(bot->GetLevel(), refuge))
        {
            TC_LOG_INFO("scripts.bots", "pbot death: {} died {} times over and is giving up on zone "
                "{} — moving to zone {} (suggested level {})", bot->GetName(),
                uint32(consecutiveDeaths), bot->GetZoneId(), refuge.ZoneId,
                uint32(refuge.SuggestedLevel));

            home = refuge.Pos;
            homeMapId = refuge.MapId;
            consecutiveDeaths = 0;

            bot->TeleportTo(refuge.MapId, refuge.Pos.GetPositionX(), refuge.Pos.GetPositionY(),
                refuge.Pos.GetPositionZ(), bot->GetOrientation());
            StopResting(bot);
            return true;
        }
    }

    bot->NearTeleportTo(home);
    StopResting(bot);
    return true;   // spend this tick coming back; act normally from the next one
}
