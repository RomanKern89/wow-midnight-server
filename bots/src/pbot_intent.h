/*
 * Companion Bots — what is this bot DOING (TrinityCore master, retail 12.0.7).
 *
 * Every other module in this directory answers "how do I do X". This one answers "which X, and for
 * how long", which is the question that was missing: the ladder in PbotAI was a fixed order, so the
 * order decided the bot's life rather than any judgement about its situation. Measured, the cost was
 * plain — 40 % of the day went to fighting, town errands and gathering, "hand in what I have already
 * finished" sat second from the bottom, and the queue of finished work stayed flat at ~266 for eight
 * hours while bots walked to objectives they had not finished.
 *
 * Two things make this a plan rather than a coin flip:
 *   - the answer is COMMITTED (see Decide's hysteresis), so a bot finishes what it started;
 *   - the weights are the bot's own, and a generated profile can override them — that is the channel
 *     through which a model shapes behaviour without ever running inside the world tick.
 */

#ifndef TRINITYCORE_PBOT_INTENT_H
#define TRINITYCORE_PBOT_INTENT_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

class Player;

namespace PbotIntent
{
    // The states a bot can be in. Deliberately few, and each maps onto modules that already exist —
    // an intent is a choice, not an implementation.
    enum class Kind : uint8
    {
        DeliverFinished = 0,   // hand in what is already complete — the reward leg
        AdvanceQuest,          // work an objective the bot has not finished
        TakeWork,              // go and ask somebody for work
        SupplyAndRepair,       // vendor, repair, make room in the bags
        GatherCraft,           // nodes, reagents, recipes
        Explore,               // see the zone; hearth home when it runs dry
        Count
    };

    char const* Name(Kind kind);

    // Which one governs this bot right now. `current` is what the caller was told last time — the
    // arbiter keeps it unless something else scores clearly better, so a bot does not oscillate
    // between two nearly equal options every thirty seconds. cooldownMs is the caller's state.
    Kind Decide(Player* bot, Kind current, uint32& cooldownMs, uint32 diff);

    // Per-bot multipliers, clamped to 0.25 .. 3.0. With no profile they are derived from the bot's
    // personality, so even a world with no generator running has bots with different priorities.
    float Weight(ObjectGuid guid, Kind kind);

    // Reads `characters.pbot_llm_profiles` into memory: one row per bot, written by the generator on
    // the operator's machine. Called at world start and by `.pbot intent reload`; a bot never reads
    // the database to decide anything.
    void LoadProfiles();

    // For the console.
    std::string Describe();
    std::string DescribeBot(Player* bot);
}

#endif // TRINITYCORE_PBOT_INTENT_H
