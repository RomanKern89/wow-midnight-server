/*
 * Companion Bots — questing for autonomous world bots (TrinityCore master, retail 12.0.7).
 *
 * A bot that grinds mobs forever does not read as a player. Players walk up to the yellow
 * exclamation mark, take the quest, kill the things it named, and walk back. That whole loop is
 * expressible with the engine's own quest API, and — importantly — it needs NO quest data of our
 * own: which creature offers what, and who takes it back, is already in the world DB
 * (creature_queststarter / creature_questender, exposed as ObjectMgr quest relations).
 *
 * What this deliberately does NOT do:
 *   - solve objectives it cannot solve. Kill and loot objectives complete by themselves as a
 *     side effect of the bot fighting; explore, use-item and talk-to objectives do not. Rather
 *     than script each objective type, the bot simply lets those quests sit and prunes them when
 *     the log fills, exactly the way a player quietly abandons a quest they gave up on.
 *   - use gameobject questgivers. Creature givers cover the overwhelming majority and keep the
 *     search to a single grid visit.
 */

#ifndef TRINITYCORE_PBOT_QUEST_H
#define TRINITYCORE_PBOT_QUEST_H

#include "ObjectGuid.h"

#include <string>
#include <unordered_set>

class Player;

namespace PbotQuest
{
    // How far the bot looks for a questgiver. Larger than the gathering radius: questgivers are
    // sparse, and a bot that only notices one it is already standing on would never quest at all.
    //
    // Raised from 60 after the first live run: with 60 yards only 4 bots in 30 ever held a quest,
    // because a bot wandering around its spawn point rarely comes within sight of a quest hub.
    constexpr float SEARCH_RANGE = 120.0f;

    // Drives one bot's questing for this tick. Returns true when it took over the tick (walking to
    // a giver, or interacting with one), so the caller can skip wandering.
    //
    // giverGuid/cooldownMs are the bot's persistent questing state, owned by PbotAI exactly like
    // the gathering state — this module holds no per-bot state of its own.
    // speakCooldownMs is the bot's shared "quiet time" from PbotAI: taking or handing in a quest is
    // one of the few moments a real player actually says something, so the hook lives here rather
    // than in a separate poll that would have to re-detect what just happened.
    // walkMs is how long this bot has been trying to reach the giver it chose. Without a budget a
    // bot whose path cannot be built re-issues the same failing move every tick FOREVER: it never
    // arrives, never gives up, and never falls through to anything else. Measured consequence —
    // 50 of 59 bots had a median displacement of zero yards over twenty minutes and 52 of 60 earned
    // no experience at all. They were not idle by design; they were stuck.
    // refused holds the quest ids the engine declined to actually add for THIS bot, so it stops
    // asking. CanTakeQuest can say yes and the add still not stick — scenario and timewalking chains
    // do exactly that — and a bot with no memory of the refusal walks back and asks again forever.
    // Measured: 3498 "accepted" lines against nine real hand-ins, one bot asking Chromie 1235 times.
    // Per bot rather than shared, because a quest another bot cannot take may be perfectly available
    // to this one.
    bool Tick(Player* bot, ObjectGuid& giverGuid, uint32& cooldownMs, uint32& walkMs,
              uint32& speakCooldownMs, std::unordered_set<uint32>& refused, uint32 diff);

    // One-line summary of a bot's quest log for the console diagnostic: how many quests it holds,
    // how many are complete, and the name of the newest one.
    std::string Describe(Player* bot);
}

#endif // TRINITYCORE_PBOT_QUEST_H
