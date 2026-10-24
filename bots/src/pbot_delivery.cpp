#include "pbot_delivery.h"

#include "pbot_questgoal.h"
#include "pbot_travel.h"   // never make an unstandable spot a bot's home

#include "GameTime.h"
#include "Log.h"
#include "StringFormat.h"
#include "MotionMaster.h"
#include "Player.h"

#include <unordered_map>

namespace
{
    // Rechecked at this interval, not every tick: the answer only changes when a quest completes.
    constexpr uint32 CHECK_INTERVAL_MS = 30000;

    // "Nothing to deliver" is a state, not an event, and saying it every thirty seconds made it
    // the loudest thing on the console: 18,696 lines in seven and a half hours. Noise is not
    // harmless here - a four-gigabyte error loop once hid inside exactly this kind of chatter.
    constexpr uint32 NO_REMOTE_REPORT_MS = 600000;
}

bool PbotDelivery::Tick(Player* bot, uint32& cooldownMs, uint32 diff, Position& home,
                        uint32& homeMapId)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat() || bot->InBattleground())
        return false;

    if (bot->IsBeingTeleportedFar() || !bot->IsInWorld())
        return false;

    if (cooldownMs > diff)
    {
        cooldownMs -= diff;
        return false;
    }
    cooldownMs = CHECK_INTERVAL_MS;

    // Say why, every time it declines. Six bots on this realm each held three or more finished
    // quests whose takers stand on another map, and not one trip was made in an hour — with no way
    // to tell which of the three gates below was the one saying no. A silent refusal is how the
    // last four defects in this codebase stayed hidden for a soak apiece.
    uint32 mapId = 0;
    Position pos;
    if (!PbotQuestGoal::FindRemoteTurnIn(bot, mapId, pos))
    {
        static std::unordered_map<ObjectGuid, uint32> lastReport;
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& last = lastReport[bot->GetGUID()];
        if (!last || nowMs - last >= NO_REMOTE_REPORT_MS)
        {
            last = nowMs;
            TC_LOG_INFO("scripts.bots", "pbot delivery: {} has no remote turn-in at all", bot->GetName());
        }
        return false;
    }

    // NOT skipped when the best map is the one the bot is standing on.
    //
    // That looked obviously right and was the largest single reason nothing moved: FindRemoteTurnIn
    // has ALREADY excluded everything within walking range, so when it answers with the bot's own
    // map it is describing work on this continent that the bot can never walk to — 339 refusals in
    // half an hour read exactly that. Those quests fell through every path there was: too far to
    // walk, no flight path that reaches them, and a delivery trip that declined because the map
    // matched. They are precisely the ones that need a lift.
    bool const sameMap = mapId == bot->GetMapId();

    // How many quests the bot is carrying at all. A full log is the pressure that makes a trip for
    // a single reward worth making — see STUCK_QUEST_COUNT.
    uint32 held = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (bot->GetQuestSlotQuestId(slot))
            ++held;

    // A lift across the map the bot is already on is not a journey — it is the walk it could not
    // make. One reward justifies it. Changing continent is a real trip and keeps the higher bar,
    // unless the bot is so full that a log slot is worth more than the flight (STUCK_QUEST_COUNT).
    uint32 const threshold = (sameMap || held >= STUCK_QUEST_COUNT) ? 1 : WORTH_THE_TRIP;

    uint32 const waiting = PbotQuestGoal::CountRemoteTurnInsOnMap(bot, mapId);
    if (waiting < threshold)
    {
        // Rest properly after declining, not just after travelling.
        //
        // The check cooldown is thirty seconds, which is right for "has anything changed" and wrong
        // for a bot whose answer will be identical until it finishes another quest. Two bots
        // produced 3,025 of the 3,048 refusals in one soak between them, saying the same sentence
        // about the same map for eight hours.
        cooldownMs = DECLINED_RETRY_MS;

        TC_LOG_INFO("scripts.bots", "pbot delivery: {} — only {} waiting on map {} (needs {}), not worth "
            "the trip", bot->GetName(), waiting, mapId, threshold);
        return false;
    }

    // Put the destination on the ground before travelling to it — but only when the destination is
    // on the map the bot is STANDING ON.
    //
    // PbotTravel::SnapToGround ends in WorldObject::GetMapHeight, which reads the map the BOT is on
    // (Object.cpp: GetMap()->GetHeight(...)). Asked about a coordinate on another map it is not a
    // stricter test, it is a meaningless one, and it answers INVALID_HEIGHT over perfectly good
    // ground. Measured in one run, every single refusal read exactly that way — Bryndel standing on
    // 2570 asked about map 0, Kessa on 1 asked about 530, Cadoc on 1642 asked about 1, Rowan on 2694
    // asked about 1, Torvald on 0 asked about 429 — ten refusals, ten map mismatches; and the trips
    // that did go out were the ones where the WRONG map happened to have terrain under those
    // coordinates. The entire return leg was gated behind that coincidence: 229 of the 266
    // undelivered quests had their taker off the bot's map.
    //
    // A cross-map destination is not left unchecked. A turn-in spot is a real SPAWN position — a
    // creature or object the server itself placed on the ground of that map — so its z is ground
    // truth there and its x/y/z is exactly what a player walks up to; Player::TeleportTo refuses an
    // invalid map or out-of-bounds coordinates before anything moves (MapManager::IsValidMapCoord),
    // and the stuck watchdog remains the net for a spawn that sits inside geometry.
    //
    // The same-map case keeps the gate exactly as it was: this is still where a bad home anchor
    // would come from, and one unchecked coordinate becomes a permanent home under the terrain that
    // every later rescue faithfully returns the bot to.
    if (sameMap)
    {
        Position landing = pos;
        if (!PbotTravel::SnapToGround(bot, landing))
        {
            TC_LOG_INFO("scripts.bots", "pbot delivery: {} — no ground at the turn-in spot on map {}, "
                "not travelling", bot->GetName(), mapId);
            cooldownMs = DECLINED_RETRY_MS;
            return false;
        }

        pos = landing;
    }

    if (!bot->TeleportTo(mapId, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                         bot->GetOrientation()))
    {
        TC_LOG_ERROR("scripts.bots", "pbot delivery: {} could not travel to map {} to hand in {} "
            "finished quests", bot->GetName(), mapId, waiting);
        return false;
    }

    TC_LOG_INFO("scripts.bots", "pbot delivery: {} travelled {} to collect {} rewards it had "
        "walked away from", bot->GetName(),
        sameMap ? Trinity::StringFormat("across map {}", mapId) : Trinity::StringFormat("to map {}", mapId),
        waiting);

    home = pos;
    homeMapId = mapId;

    bot->GetMotionMaster()->Clear();
    cooldownMs = AFTER_TRIP_MS;
    return true;
}
