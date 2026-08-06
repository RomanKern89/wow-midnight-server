/*
 * Companion Bots — travel implementation. See pbot_travel.h for why long walks must be stepped.
 */

#include "pbot_travel.h"

#include "MotionMaster.h"
#include "MoveSpline.h"        // the live path itself — the generator is not a reliable witness
#include "MovementDefines.h"   // IDLE_MOTION_TYPE
#include "Player.h"
#include "Random.h"
#include "pbot_personality.h"

#include <cmath>

namespace
{
    constexpr uint32 TRAVEL_POINT_ID = 0xC04;   // distinct from gather/quest/loot/bg MovePoint ids

    // How far a roam takes a bot. Far enough that it visibly goes somewhere and meets different
    // creatures, short enough that it stays inside its zone rather than wandering into content that
    // will kill it.
    constexpr float ROAM_MIN = 120.0f;
    constexpr float ROAM_MAX = 300.0f;
}

bool PbotTravel::StepToward(Player* bot, Position const& dest)
{
    if (!bot || !bot->IsInWorld())
        return false;

    float const remaining = bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
    if (remaining <= ARRIVED_RANGE)
        return false;   // arrived

    // Only issue the next hop once the previous one finished, or the path would be restarted every
    // tick and the bot would never actually move.
    //
    // Ask the SPLINE, not the generator. A generator outlives its spline: when MoveSplineInit fails
    // validation — a destination inside a rock, a gap in the navmesh — nothing is ever launched, yet
    // the generator does not return to idle either. The old test read that as "still travelling" and
    // said so forever. Measured: a bot set out for an anvil 528 yards away, spent the entire
    // ten-minute trip budget, and ended 549 yards away, having never moved; then started the
    // identical journey again. Every long errand had this — the "gave up walking to a repairer"
    // lines predate the crafting work by weeks.
    if (!bot->movespline->Finalized())
        return true;   // genuinely gliding along a live path

    // Finalized but not idle means the last order died on arrival at nothing. Clear it, so the hop
    // below is actually issued instead of being swallowed by a generator that will never finish.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
        bot->GetMotionMaster()->Clear();

    float x = dest.GetPositionX();
    float y = dest.GetPositionY();
    float z = dest.GetPositionZ();

    if (remaining > MAX_HOP)
    {
        float const ratio = MAX_HOP / remaining;
        x = bot->GetPositionX() + (x - bot->GetPositionX()) * ratio;
        y = bot->GetPositionY() + (y - bot->GetPositionY()) * ratio;
        z = bot->GetPositionZ();   // the ground snap below fixes the height for this hop
    }

    bot->UpdateAllowedPositionZ(x, y, z);
    bot->GetMotionMaster()->MovePoint(TRAVEL_POINT_ID, x, y, z);
    return true;
}

bool PbotTravel::PickRoamPoint(Player* bot, Position& out)
{
    if (!bot || !bot->IsInWorld())
        return false;

    uint8 const wanderlust = PbotPersonality::Of(bot->GetGUID()).Wanderlust;
    float const angle = frand(0.0f, 2.0f * float(M_PI));
    float const distance = frand(PbotPersonality::Scale(wanderlust, ROAM_MIN, 0.5f, 1.5f),
                                 PbotPersonality::Scale(wanderlust, ROAM_MAX, 0.5f, 1.5f));

    float x = bot->GetPositionX() + std::cos(angle) * distance;
    float y = bot->GetPositionY() + std::sin(angle) * distance;
    float z = bot->GetPositionZ();

    // Snap to the surface. A destination hanging in the air or buried in terrain yields no path,
    // and the bot would simply stand still — indistinguishable from having no destination at all.
    bot->UpdateAllowedPositionZ(x, y, z);

    out.Relocate(x, y, z, bot->GetOrientation());
    return true;
}
