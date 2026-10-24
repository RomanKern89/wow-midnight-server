/*
 * Companion Bots — travel implementation. See pbot_travel.h for why long walks must be stepped.
 */

#include "pbot_travel.h"

#include "GameTime.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"        // the live path itself — the generator is not a reliable witness
#include "MovementDefines.h"   // IDLE_MOTION_TYPE
#include "Player.h"
#include "Random.h"
#include "pbot_flight.h"
#include "pbot_personality.h"

#include <cmath>
#include <unordered_map>

namespace
{
    constexpr uint32 TRAVEL_POINT_ID = 0xC04;   // distinct from gather/quest/loot/bg MovePoint ids

    // How far a roam takes a bot. Far enough that it visibly goes somewhere and meets different
    // creatures, short enough that it stays inside its zone rather than wandering into content that
    // will kill it.
    constexpr float ROAM_MIN = 120.0f;
    constexpr float ROAM_MAX = 300.0f;

    // How long a freshly issued hop is given to become a live spline before it is written off.
    //
    // Movement orders are applied AFTER the update that requested them, so a brand-new order always
    // looks dead for at least one tick. Anything shorter than a couple of ticks re-creates the
    // cancel-your-own-order loop this exists to end; much longer and a genuinely failed order stalls
    // the bot for no reason.
    constexpr uint32 HOP_START_GRACE_MS = 1000;

    // When each bot last asked for a hop. Keyed by guid rather than carried through the call, so
    // that every existing caller of StepToward keeps its two-argument signature.
    uint32& LastHopIssuedAt(ObjectGuid const& guid)
    {
        static std::unordered_map<ObjectGuid, uint32> issued;
        return issued[guid];
    }

    // Orders written off because they never produced a spline. Read back through PbotTravel::FailedHops
    // wherever a failed walk is explained.
    uint32& FailedHopsFor(ObjectGuid const& guid)
    {
        static std::unordered_map<ObjectGuid, uint32> failed;
        return failed[guid];
    }

    // Hops accepted by the engine (a spline really launched). Read back through
    // PbotTravel::HopsIssued, and cleared by the caller each time a new destination is chosen, so
    // the count belongs to one journey rather than to the bot's whole life.
    uint32& HopsIssuedFor(ObjectGuid const& guid)
    {
        static std::unordered_map<ObjectGuid, uint32> hops;
        return hops[guid];
    }
}

uint32 PbotTravel::FailedHops(Player const* bot)
{
    return bot ? FailedHopsFor(bot->GetGUID()) : 0;
}

uint32 PbotTravel::HopsIssued(Player const* bot)
{
    return bot ? HopsIssuedFor(bot->GetGUID()) : 0;
}

void PbotTravel::ResetHops(Player const* bot)
{
    if (bot)
        HopsIssuedFor(bot->GetGUID()) = 0;
}

bool PbotTravel::SnapToGround(Player* bot, Position& pos)
{
    if (!bot || !bot->IsInWorld())
        return false;

    float const ground = bot->GetMapHeight(pos.GetPositionX(), pos.GetPositionY(),
        pos.GetPositionZ() + UNDERGROUND_BY);

    // The engine's own "no ground here" answer. Not a height to be corrected — an answer to a
    // question about a place that does not exist.
    if (ground <= INVALID_HEIGHT)
        return false;

    pos.Relocate(pos.GetPositionX(), pos.GetPositionY(), ground, pos.GetOrientation());
    return true;
}

bool PbotTravel::IsStandable(Player* bot, Position const& pos)
{
    if (!bot || !bot->IsInWorld())
        return false;

    // Search from ABOVE the given z: asking from underneath finds the cave floor the character is
    // already lying on and cheerfully reports that everything is fine.
    float const ground = bot->GetMapHeight(pos.GetPositionX(), pos.GetPositionY(),
        pos.GetPositionZ() + UNDERGROUND_BY);

    if (ground <= INVALID_HEIGHT)
        return false;

    return pos.GetPositionZ() >= ground - UNDERGROUND_BY;
}

bool PbotTravel::StepToward(Player* bot, Position const& dest)
{
    if (!bot || !bot->IsInWorld())
        return false;

    // One place decides that a long journey should be flown, and it is here rather than in each of
    // the errands. The quest goal, the turn-in, the repair trip and moving house all reach their
    // destination through this function, and none of them has any business knowing that flight
    // paths exist.
    //
    // Note the order: flight is offered BEFORE the arrival test, because a bot in the air has not
    // arrived and must not be handed back to the caller to be walked somewhere.
    if (bot->IsInFlight())
        return true;

    float const remaining = bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());

    if (remaining > PbotFlight::WORTH_FLYING && PbotFlight::TryTravel(bot, dest))
        return true;

    return StepGround(bot, dest);
}

bool PbotTravel::StepGround(Player* bot, Position const& dest)
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

    // Finalized but not idle USED to mean "the last order died on arrival at nothing", and the code
    // cleared the generator and issued a fresh hop. That reasoning had a hole big enough to stop the
    // whole population walking.
    //
    // MotionMaster::MovePoint does not take effect when it is called: during a world update the
    // engine queues it (_delayedActions / MOTIONMASTER_DELAYED_ADD) and applies it afterwards. So on
    // the very next tick the generator is already a PointMovementGenerator while the spline has not
    // started yet — which reads as exactly the failure above. The bot cancelled its own order and
    // reissued it, every tick, forever. Measured: 3 to 32 yards covered per 180 seconds of travel,
    // against roughly 1260 at walking pace, across the entire population.
    //
    // So a young order is given time to start. Only one that has had its chance and still produced
    // no spline is treated as dead.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
    {
        uint32 const now = GameTime::GetGameTimeMS();
        uint32& issuedAt = LastHopIssuedAt(bot->GetGUID());

        if (issuedAt && now - issuedAt < HOP_START_GRACE_MS)
            return true;   // issued a moment ago; let the engine get to it

        bot->GetMotionMaster()->Clear();
        issuedAt = 0;
        ++FailedHopsFor(bot->GetGUID());
    }

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
    LastHopIssuedAt(bot->GetGUID()) = GameTime::GetGameTimeMS();
    ++HopsIssuedFor(bot->GetGUID());

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
