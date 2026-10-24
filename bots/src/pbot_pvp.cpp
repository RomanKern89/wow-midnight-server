/*
 * Companion Bots — Phase 7 PvP implementation.
 * See pbot_pvp.h for why the duel accept is open-coded rather than routed through the packet handler.
 */

#include "pbot_pvp.h"

#include "Cell.h"
#include "CellImpl.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Player.h"

namespace
{
    // The countdown the engine uses between accepting a duel and the duel starting.
    constexpr int32 DUEL_COUNTDOWN_SECONDS = 3;

    class HostilePlayerCheck
    {
    public:
        HostilePlayerCheck(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Player* target) const
        {
            if (!target || target == _bot || !target->IsAlive())
                return false;

            if (!_bot->IsWithinDist(target, _range))
                return false;

            // NOT IsValidAttackTarget — it runs CanSeeOrDetect, which is false for every target a
            // socket-less bot looks at, so this check silently rejected everyone. That is why
            // cross-faction fights only ever happened when someone attacked first. See the long
            // note in pbot_autonomy.cpp; Unit::Attack itself does not apply that gate.
            if (_bot->IsFriendlyTo(target) || target->IsFriendlyTo(_bot))
                return false;

            if (target->IsImmuneToPC() || target->IsUninteractible())
                return false;

            // Both sides must be flagged for open-world PvP, or this is not a fight at all.
            return _bot->IsPvP() && target->IsPvP();
        }

    private:
        Player const* _bot;
        float _range;
    };
}

bool PbotPvP::AcceptPendingDuel(Player* bot)
{
    if (!bot || !bot->duel)
        return false;

    // Only the challenged side accepts, and only while the challenge is still outstanding.
    if (bot->duel->Initiator == bot || bot->duel->State != DUEL_STATE_CHALLENGED)
        return false;

    Player* opponent = bot->duel->Opponent;
    if (!opponent || !opponent->duel)
        return false;

    // Same transition as WorldSession::HandleDuelAccepted: both duellists move to COUNTDOWN with a
    // shared start time, and both get PvP rules enabled.
    time_t const startTime = GameTime::GetGameTime() + DUEL_COUNTDOWN_SECONDS;

    bot->duel->StartTime = startTime;
    opponent->duel->StartTime = startTime;
    bot->duel->State = DUEL_STATE_COUNTDOWN;
    opponent->duel->State = DUEL_STATE_COUNTDOWN;

    bot->EnablePvpRules();
    opponent->EnablePvpRules();

    TC_LOG_INFO("scripts.bots", "PbotPvP: bot {} accepted a duel from {}.",
        bot->GetName(), opponent->GetName());
    return true;
}

Unit* PbotPvP::FindHostilePlayer(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return nullptr;

    Player* found = nullptr;
    HostilePlayerCheck check(bot, PVP_SEARCH_RANGE);
    Trinity::PlayerLastSearcher<HostilePlayerCheck> searcher(bot, found, check);
    Cell::VisitAllObjects(bot, searcher, PVP_SEARCH_RANGE);

    return found;
}
