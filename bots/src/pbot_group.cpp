/*
 * Companion Bots — partying implementation. See pbot_group.h.
 */

#include "pbot_group.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Player.h"
#include "pbot_mgr.h"
#include "pbot_personality.h"

#include <vector>

namespace
{
    // Checked rarely: this walks grid cells, and two bots who did not group this minute will still
    // be standing next to each other the next one.
    constexpr uint32 CHECK_INTERVAL_MS = 60000;

    // Leave room in a party rather than filling it to the brim, so a bot arriving later can still
    // join rather than starting a second party beside the first.
    constexpr uint32 PARTY_SOFT_CAP = 4;

    // Below this sociability a bot simply prefers to play alone.
    constexpr uint8 MIN_SOCIABILITY_TO_GROUP = 40;

    // Is this a world bot we may party with — not a real player, not somebody's companion?
    bool IsFreeWorldBot(Player const* candidate)
    {
        if (!candidate || !candidate->IsInWorld() || !candidate->IsAlive())
            return false;

        // A registered bot with no owner is a world bot. A real player has no AI record at all, and
        // a companion bot has an owner whose party it belongs in.
        if (!PbotMgr::GetBotAI(candidate->GetGUID()))
            return false;

        return PbotMgr::GetOwnerOf(candidate->GetGUID()).IsEmpty();
    }

    // Own check class: the engine ships no general "other players in range" predicate, only a
    // position-range one. Same shape the PvP module already uses for hostile players.
    class PartyCandidateCheck
    {
    public:
        PartyCandidateCheck(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Player* other) const
        {
            return other && other != _bot && _bot->IsWithinDist(other, _range) && IsFreeWorldBot(other);
        }

    private:
        Player const* _bot;
        float _range;
    };

    std::vector<Player*> NearbyBots(Player* bot)
    {
        std::vector<Player*> result;

        PartyCandidateCheck check(bot, PbotGroup::PARTY_RANGE);
        Trinity::PlayerListSearcher<PartyCandidateCheck> searcher(bot, result, check);
        Cell::VisitWorldObjects(bot, searcher, PbotGroup::PARTY_RANGE);

        return result;
    }
}

bool PbotGroup::Tick(Player* bot, uint32& cooldownMs, uint32 diff)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->InBattleground())
        return false;

    if (cooldownMs > diff)
    {
        cooldownMs -= diff;
        return false;
    }
    cooldownMs = CHECK_INTERVAL_MS;

    // Only world bots recruit; a companion bot already belongs to its owner's party.
    if (!PbotMgr::GetOwnerOf(bot->GetGUID()).IsEmpty())
        return false;

    // Not everyone wants company. A loner passing a stranger and saying nothing is as much a real
    // player as the one who invites; making every bot group on sight would trade one uniform
    // behaviour for another.
    if (PbotPersonality::Of(bot->GetGUID()).Sociability < MIN_SOCIABILITY_TO_GROUP)
        return false;

    Group* myGroup = bot->GetGroup();
    if (myGroup && myGroup->GetMembersCount() >= PARTY_SOFT_CAP)
        return false;

    for (Player* candidate : NearbyBots(bot))
    {
        if (candidate == bot)
            continue;   // the search check already filtered to free world bots

        // Same side only. A cross-faction party is not a thing, and these two should be fighting.
        if (candidate->GetTeam() != bot->GetTeam())
            continue;

        Group* theirGroup = candidate->GetGroup();

        if (myGroup && theirGroup)
            continue;                      // both already have company

        if (theirGroup)
        {
            if (theirGroup->GetMembersCount() >= PARTY_SOFT_CAP)
                continue;

            theirGroup->AddMember(bot);
            TC_LOG_INFO("scripts.bots", "pbot group: {} joined {}'s party", bot->GetName(), candidate->GetName());
            return true;
        }

        if (myGroup)
        {
            myGroup->AddMember(candidate);
            TC_LOG_INFO("scripts.bots", "pbot group: {} recruited {}", bot->GetName(), candidate->GetName());
            return true;
        }

        // Neither is grouped — start one. Same construction the companion-bot path uses.
        Group* group = new Group();
        if (!group->Create(bot))
        {
            delete group;
            return false;
        }

        sGroupMgr->AddGroup(group);
        group->AddMember(candidate);
        TC_LOG_INFO("scripts.bots", "pbot group: {} and {} formed a party", bot->GetName(), candidate->GetName());
        return true;
    }

    return false;
}
