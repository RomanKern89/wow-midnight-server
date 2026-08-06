/*
 * Companion Bots — Phase 4A chat command layer (TrinityCore master, retail 12.0.7).
 *
 * Bots have no client and therefore no inbound packet path, so commands do NOT arrive as
 * CMSG_* opcodes addressed to the bot. Instead we hook the OWNER's outgoing chat server-side
 * (PlayerScript::OnChat, ScriptMgr.h:741-749), parse the text, and drive the bot's PbotAI
 * directly. Replies go back out through the bot's own Unit::Say / Player::Whisper, which are
 * null-socket safe (the bot session never receives, it only broadcasts to nearby real clients).
 *
 * Everything here is pure string work executed inline on the world tick that processed the
 * owner's chat packet — no I/O, no allocation beyond one lowercased copy of the line, and an
 * early-out on PbotMgr::OwnerHasBots() so players without bots pay ~one hash lookup per line.
 *
 * Vocabulary is bilingual (Russian first, English second) because commands are typed by a
 * Russian-speaking owner. Matching is done on a properly lowercased wide-string form via the
 * engine's Utf8toWStr/wstrToLower (Util.h:103/368) so Cyrillic case folding is correct — a
 * naive ASCII tolower() would silently fail on "СТОЙ".
 */

#ifndef TRINITYCORE_PBOT_CHAT_H
#define TRINITYCORE_PBOT_CHAT_H

#include "Define.h"
#include <string>
#include <string_view>

// What the bot was told to do. Verbs split into two kinds:
//   - stance/mode verbs (Follow/Stay/Assist/Defend/Passive) latch persistent state on PbotAI;
//   - one-shot verbs (Come/Attack/Stop/Status/Help) act now and leave state alone.
enum class PbotVerb : uint8
{
    None = 0,
    Follow,     // resume formation movement
    Stay,       // hold position, stop following
    Come,       // one-shot: clear Stay and reform on the owner now
    Attack,     // one-shot: engage the owner's current target immediately
    Assist,     // stance: fight what the owner fights (default)
    Defend,     // stance: prioritise whatever is attacking the owner
    Passive,    // stance: never engage on our own
    Stop,       // one-shot: disengage now (does not change stance)
    Status,     // report level/health/stance
    Help        // list the vocabulary
};

// A parsed command line plus who it was aimed at.
struct PbotChatCommand
{
    PbotVerb    Verb = PbotVerb::None;
    // Empty means "every bot this owner has". Otherwise the (lowercased) name the owner typed,
    // matched case-insensitively against bot names by the dispatcher.
    std::string TargetName;
};

namespace PbotChat
{
    // Parses one line of owner chat.
    //
    // addressImplied=true is used for whispers: the owner already picked the recipient, so no
    // "боты, ..." prefix is required and the whole line is treated as the command.
    // addressImplied=false is used for say/yell/party, where the line must open with an address
    // token ("бот", "боты", "bots", "все", or a bot's name) or we ignore it entirely — otherwise
    // ordinary conversation containing the word "стой" would order the party around.
    //
    // Returns false when the line is not a bot command; out is then untouched.
    bool Parse(std::string_view msg, bool addressImplied, PbotChatCommand& out);

    // Short acknowledgement the bot says back, e.g. Follow -> "Иду за тобой.". Never null.
    char const* Acknowledgement(PbotVerb verb);

    // Multi-line vocabulary dump for PbotVerb::Help, one line per call index; returns nullptr
    // past the end so callers can loop until exhausted without a separate count.
    char const* HelpLine(uint32 index);
}

#endif // TRINITYCORE_PBOT_CHAT_H
