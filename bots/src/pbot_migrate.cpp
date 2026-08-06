/*
 * Companion Bots — zone migration implementation. See pbot_migrate.h for the two constraints.
 */

#include "pbot_migrate.h"

#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "pbot_world_spots.h"

namespace
{
    // Between checks. The question ("has my level run past this zone?") only changes when the bot
    // levels, which is minutes apart at best, so asking once a minute is already generous.
    constexpr uint32 CHECK_INTERVAL_MS = 60000;

    // After an actual move, wait longer before considering another. Without this a bot that lands
    // in a zone whose band is still below it would hop again on the very next check, and a bot that
    // spends its life teleporting is no more player-like than one that never moves.
    constexpr uint32 AFTER_MOVE_MS = 300000;   // 5 minutes
}

bool PbotMigrate::Tick(Player* bot, uint32& cooldownMs, uint32 diff, Position& home, uint32& homeMapId)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat() || bot->InBattleground())
        return false;

    // A bot mid-teleport has no meaningful map or zone to reason about yet.
    if (bot->IsBeingTeleportedFar() || !bot->IsInWorld())
        return false;

    if (cooldownMs > diff)
    {
        cooldownMs -= diff;
        return false;
    }
    cooldownMs = CHECK_INTERVAL_MS;

    uint8 const band = PbotWorldSpots::BandForZone(bot->GetMapId(), bot->GetZoneId());
    if (!band)
        return false;   // no tuning data for this zone — nothing to compare against

    if (bot->GetLevel() <= band + OUTGROWN_BY)
        return false;   // still belongs here

    PbotWorldSpots::Spot destination;
    if (!PbotWorldSpots::PickForLevel(bot->GetLevel(), destination))
        return false;   // nothing suitable on a loaded map; staying put beats loading a continent

    if (destination.MapId == bot->GetMapId() && destination.ZoneId == bot->GetZoneId())
        return false;   // the picker chose the zone we are already in

    if (!bot->TeleportTo(destination.MapId, destination.Pos.GetPositionX(),
                         destination.Pos.GetPositionY(), destination.Pos.GetPositionZ(),
                         bot->GetOrientation()))
    {
        TC_LOG_ERROR("scripts.bots", "pbot migrate: {} could not travel to map {} zone {}",
            bot->GetName(), destination.MapId, destination.ZoneId);
        return false;
    }

    TC_LOG_INFO("scripts.bots", "pbot migrate: {} (level {}) outgrew zone {} (band {}) and moved to "
        "map {} zone {} (band {})", bot->GetName(), uint32(bot->GetLevel()), bot->GetZoneId(),
        uint32(band), destination.MapId, destination.ZoneId, uint32(destination.SuggestedLevel));

    // The new anchor is the destination — otherwise the bot would wander back toward coordinates on
    // the map it just left, which is the failure that sent battleground bots marching off the map.
    home = destination.Pos;
    homeMapId = destination.MapId;

    bot->GetMotionMaster()->Clear();
    cooldownMs = AFTER_MOVE_MS;
    return true;
}
