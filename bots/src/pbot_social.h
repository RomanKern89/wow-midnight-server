/*
 * Companion Bots — speech (TrinityCore master, retail 12.0.7).
 *
 * The loudest tell that a populated world is fake is silence. A real server is noisy: people greet
 * each other, swear at a bad pull, announce what they are doing, ask for a hand. Our world bots
 * quest, fight, loot and travel and never say a word, and a player standing next to one knows
 * instantly what it is.
 *
 * Two rules shape this:
 *   - only speak when there is someone to hear it. A bot talking to an empty valley is worse than
 *     silence — it is spam in a log nobody reads.
 *   - speak rarely. One line every few minutes per bot reads as a living world; one line per event
 *     reads as a machine, because real people do not narrate everything they do.
 */

#ifndef TRINITYCORE_PBOT_SOCIAL_H
#define TRINITYCORE_PBOT_SOCIAL_H

#include "Define.h"

#include <string>

class Player;

namespace PbotSocial
{
    // How far a bot looks for an audience before deciding to say anything.
    constexpr float AUDIENCE_RANGE = 40.0f;

    // Idle chatter: greetings, remarks about the zone, small talk. Called from the AI tick; does
    // nothing unless somebody is nearby and the throttle has expired.
    void TickChatter(Player* bot, uint32& cooldownMs, uint32 diff);

    // Event lines. Each is throttled by the same per-bot cooldown as the chatter, so a bot that has
    // just spoken stays quiet — which is what stops a quest hub turning into a wall of text.
    void OnQuestAccepted(Player* bot, uint32& cooldownMs, std::string const& questTitle);
    void OnQuestTurnedIn(Player* bot, uint32& cooldownMs, std::string const& questTitle);
    void OnLevelUp(Player* bot, uint32& cooldownMs);
    void OnNearDeath(Player* bot, uint32& cooldownMs);

    // The generated-line pool. A separate program (on the operator's machine, next to whatever runs
    // the model) writes `characters.pbot_llm_lines`; this side only ever reads it, and only when
    // told to. Called once at world start and by `.pbot llm reload`.
    void LoadGeneratedPool();

    // What the pool holds right now, per event — for `.pbot llm stats`.
    std::string DescribeGeneratedPool();

    // Has any event's in-memory pool run low? The world consumes lines and never reports which ones,
    // so a long run drifts back to the built-in lines unless someone reloads. Checked on a timer by
    // PbotWorldScript, which reloads only when this says yes.
    bool PoolNeedsRefill();

    // A generated line for this event, shaped to this bot's level band and personality, or `fallback`
    // when the pool has nothing suitable. Consumes what it returns, so no line is said twice in a run.
    std::string PickLine(Player* bot, char const* event, char const* fallback);
}

#endif // TRINITYCORE_PBOT_SOCIAL_H
