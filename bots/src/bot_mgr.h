/*
 * Companion Bots — BotMgr registry & lifecycle (TrinityCore master, retail 12.0.7).
 *
 * The engine has no "give me every creature a player summoned" API, so we keep our own
 * process-wide registry: owner ObjectGuid -> that player's bots. BotMgr owns summon ordering
 * (DESIGN SS2), the max-4 cap, level sync, and the owner-event handlers wired from
 * bot_playerscript.cpp. Free-function facade over a single file-static map. See DESIGN SS2/SS10.
 */

#ifndef TRINITYCORE_BOT_MGR_H
#define TRINITYCORE_BOT_MGR_H

#include "ObjectGuid.h"
#include "bot_common.h"
#include <string>
#include <vector>

class Player;
class Creature;

namespace BotMgr
{
    // Phase 2: award the owner a small gold bonus for a creature a bot helped kill. Fires from
    // BotAI::KilledUnit (bot lands the blow) and bot_playerscript::OnCreatureKill (owner lands
    // the blow with a bot that had threat). A short per-victim TTL guard prevents double credit
    // in the structurally-unreachable case both fire (DESIGN_PHASE2 SS4a).
    void CreditGoldForKill(Player* owner, Creature const* victim);

    // Phase 2 gold share, engagement tracking (review finding #1): BotAI notes "one of the
    // owner's bots is fighting victim X" here (JustEngagedWith + throttled refresh in UpdateAI);
    // OnCreatureKill consults the note (10s TTL) instead of the victim's threat list, which the
    // engine has already cleared by the time that hook fires.
    void NoteBotEngagement(ObjectGuid ownerGuid, ObjectGuid victimGuid);
    bool WasBotEngagedRecently(ObjectGuid ownerGuid, ObjectGuid victimGuid);

    // Summons one bot of the given class for the owner. Returns false and fills 'err' with a
    // player-facing reason on any failure (max reached, unknown class, summon/AI failure).
    bool AddBot(Player* owner, BotClass botClass, std::string& err);

    // Dismisses the owner's most-recently-added bot. Returns false if the owner has none.
    bool RemoveBot(Player* owner);
    void RemoveAllBots(Player* owner);

    // Snapshot of the owner's current bot guids (<= MAX_BOTS_PER_PLAYER). Returned by value on
    // purpose — callers iterate a stable copy while the registry may mutate underneath them.
    std::vector<ObjectGuid> GetBots(Player* owner);
    std::vector<ObjectGuid> GetBots(ObjectGuid ownerGuid);
    uint32 GetBotCount(Player* owner);

    // Owner-event handlers (called from PlayerScript hooks).
    void OnOwnerLevelChanged(Player* owner);   // re-sync every bot's level to the owner
    void OnOwnerMapChanged(Player* owner);      // despawn + resummon on the new map
    void OnOwnerLogout(Player* owner);          // dismiss all
}

#endif // TRINITYCORE_BOT_MGR_H
