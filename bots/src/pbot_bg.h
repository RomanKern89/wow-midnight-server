/*
 * Companion Bots — Phase 7 battlegrounds (TrinityCore master, retail 12.0.7).
 *
 * A battleground is the one place where "a bot is just a Player" pays off completely: scoring,
 * objectives, resurrection, win conditions and rewards are all engine systems that already work on
 * any Player. What a bot lacks is the two client-side decisions — pressing "join" and pressing
 * "enter battle" — plus the worldport acknowledgement that carries it into the instance map.
 *
 * Those three gaps are what this file closes:
 *   Queue     — the same BattlegroundQueue::AddGroup the join handler calls.
 *   TickQueue — watches for the invite and performs the entry sequence the port handler performs,
 *               minus the anti-cheat checks that exist to distrust a client.
 *   (entry)   — the actual arrival happens in WorldSession::HandleMoveWorldportAck, which the
 *               engine itself notes is where a player is added to the battleground ("add only in
 *               HandleMoveWorldPortAck()", BattleGroundHandler.cpp). PbotWorldScript already
 *               answers that worldport for bots, so entry completes without anything extra here.
 *
 * NOT included: objective tactics. A bot in a battleground currently fights whatever is near it
 * via the normal autonomous combat path. Flag carrying, node capturing and map-specific play are a
 * separate body of work per battleground and are deliberately not faked here.
 */

#ifndef TRINITYCORE_PBOT_BG_H
#define TRINITYCORE_PBOT_BG_H

#include "Define.h"
#include "Position.h"
#include <string>

class Player;

// Per-bot battleground objective state, owned by PbotAI.
//
// The cache exists because Warsong Gulch is roughly 350 yards end to end: a bot standing at its own
// base cannot see the enemy flag at all, so the search radius has to cover most of the map — and a
// map-wide grid sweep every tick for every bot in a 20-bot match is not affordable. So the sweep
// runs rarely, and once a flag has been seen its position is remembered and simply walked to.
struct PbotBgState
{
    uint32   SearchCooldownMs = 0;
    Position Target;               // last known position of the flag we are heading for
    bool     HasTarget = false;
};

namespace PbotBG
{
    // Puts the bot into the queue for the given battlemaster list id (e.g. 2 = Warsong Gulch).
    // Returns false and fills err on failure — the common causes are a level outside every bracket
    // for that battleground, or an unknown battlemaster list id.
    bool Queue(Player* bot, uint16 battlemasterListId, std::string& err);

    // Called every tick for a queued bot: if the queue has invited it, run the entry sequence.
    // Returns true when an entry was started this tick (the bot is then mid-teleport and should not
    // be given other orders).
    bool TickQueue(Player* bot);

    // Is this bot sitting in any battleground queue right now?
    bool IsQueued(Player const* bot);

    // One tick of objective play for a bot already inside a battleground.
    //
    // Currently covers Warsong Gulch only, and only its core loop: take the enemy flag, run it to
    // your own base, and hunt the enemy carrier. Everything else in the battleground is left to the
    // normal autonomous combat path, which is why a bot with nothing objective to do still fights.
    //
    // Returns true when it issued an objective action this tick, meaning the caller should not also
    // send the bot wandering.
    //
    // Flag objects are located by searching the map for the flag GameObject entries rather than by
    // hardcoding base coordinates: the flags are spawned by the battleground's own script (map 489
    // has no static gameobject rows at all), and a flag's own position IS the base it belongs to.
    bool TickInBattleground(Player* bot, PbotBgState& state, uint32 diff);

    // One human-readable line describing what this bot sees inside a battleground: the match's own
    // status, whether it is carrying a flag, and whether the flag search finds anything and how far
    // away. Written because two rounds of "no flag grabs" gave no way to tell apart "the match
    // never started", "the bot is stuck behind the gates" and "the search finds nothing".
    std::string DescribeBattlegroundState(Player* bot);
}

#endif // TRINITYCORE_PBOT_BG_H
