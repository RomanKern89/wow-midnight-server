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
}

#endif // TRINITYCORE_PBOT_SOCIAL_H
