/*
 * Companion Bots — hearthstone home. See pbot_hearth.h for why "home" is derived.
 */

#include "pbot_hearth.h"

#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"

namespace
{
    // Close enough to count as "already home".
    constexpr float SAME_PLACE_RANGE = 200.0f;
}

bool PbotHearth::GoHome(Player* bot, Position& home, uint32& homeMapId)
{
    if (!bot || !bot->IsInWorld() || bot->IsInCombat() || bot->InBattleground())
        return false;

    if (bot->IsBeingTeleported())
        return false;

    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(bot->GetRace(), bot->GetClass());
    if (!info)
        return false;

    WorldLocation const& start = info->createPosition.Loc;

    // Already there — no point teleporting onto ourselves, and doing so would just restart the
    // "nothing to do" timer without changing anything.
    if (start.GetMapId() == bot->GetMapId() && bot->GetExactDist(start) < SAME_PLACE_RANGE)
        return false;

    if (!bot->TeleportTo(start.GetMapId(), start.GetPositionX(), start.GetPositionY(),
                         start.GetPositionZ(), start.GetOrientation()))
        return false;

    TC_LOG_INFO("scripts.bots", "pbot hearth: {} found nothing to do and went home to map {} ({:.0f} {:.0f})",
        bot->GetName(), start.GetMapId(), start.GetPositionX(), start.GetPositionY());

    home = start;
    homeMapId = start.GetMapId();
    bot->GetMotionMaster()->Clear();
    return true;
}
