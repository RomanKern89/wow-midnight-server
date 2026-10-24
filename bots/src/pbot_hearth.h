/*
 * Companion Bots — going home when there is nothing to do (TrinityCore master, retail 12.0.7).
 *
 * A player who runs out of quests in a zone does not stand in a field forever: they hearth back to
 * their own people and pick up work there. A bot needs the same escape hatch, because "no quest
 * within reach" is a state it can otherwise sit in indefinitely — roaming, killing the occasional
 * boar, going nowhere.
 *
 * "Home" is derived, not listed: ObjectMgr's PlayerInfo carries the create position for every
 * race/class pair — the spot the game itself starts that race at. So an orc returns to Durotar and
 * a dwarf to Dun Morogh with no table of ours to maintain, and races added later work for free.
 */

#ifndef TRINITYCORE_PBOT_HEARTH_H
#define TRINITYCORE_PBOT_HEARTH_H

#include "Define.h"
#include "Position.h"

class Player;

namespace PbotHearth
{
    // Sends the bot to its race's home. Returns false if there is no create position for it or the
    // teleport is refused. The caller updates its wander anchor from home/homeMapId on success.
    bool GoHome(Player* bot, Position& home, uint32& homeMapId);
}

#endif // TRINITYCORE_PBOT_HEARTH_H
