/*
 * Companion Bots — Phase 4A chat command parser (TrinityCore master, retail 12.0.7).
 *
 * Pure text -> PbotChatCommand translation. No engine state is touched here; the dispatcher in
 * pbot_chat_script.cpp owns resolving names and driving PbotAI. Keeping the parser free of engine
 * objects is deliberate: it is the piece most likely to grow (new verbs, new phrasings) and it can
 * be reasoned about without the world running.
 *
 * Matching strategy: normalise the line once (UTF-8 -> wide -> lowercase -> UTF-8, via the engine's
 * own Util.h helpers so Cyrillic folds correctly), strip the address token, then substring-search
 * the remainder for verb phrases. Substring rather than exact-token matching is intentional — the
 * owner types "боты, а ну-ка в бой!", not "bots attack" — and the address-token requirement on
 * public channels is what keeps that looseness from hijacking ordinary conversation.
 *
 * Phrase ordering in VERB_TABLE is load-bearing: negated forms must precede the forms they contain
 * ("не атакуй" before "атакуй"), or a passive order would read as an attack order.
 */

#include "pbot_chat.h"

#include "Util.h"

#include <algorithm>
#include <array>
#include <string>

namespace
{
    // Tokens that mean "this line is for my bots" on a public channel. Compared against the first
    // word of the line with surrounding punctuation stripped.
    constexpr std::string_view ADDRESS_TOKENS[] =
    {
        "боты", "бот", "все", "всем", "отряд",
        "bots", "bot", "all", "party"
    };

    struct VerbPhrase
    {
        std::string_view Phrase;
        PbotVerb         Verb;
    };

    // Ordered: negations and multi-word phrases first (see file header). Everything is already
    // lowercase — the input is normalised before lookup.
    constexpr VerbPhrase VERB_TABLE[] =
    {
        // Passive must outrank Attack: "не атакуй" contains "атакуй".
        { "не атакуй",   PbotVerb::Passive },
        { "не нападай",  PbotVerb::Passive },
        { "не лезь",     PbotVerb::Passive },
        { "пассив",      PbotVerb::Passive },
        { "passive",     PbotVerb::Passive },

        { "ко мне",      PbotVerb::Come    },
        { "сюда",        PbotVerb::Come    },
        { "come",        PbotVerb::Come    },
        { "here",        PbotVerb::Come    },

        { "за мной",     PbotVerb::Follow  },
        { "следуй",      PbotVerb::Follow  },
        { "фолов",       PbotVerb::Follow  },
        { "follow",      PbotVerb::Follow  },

        { "в бой",       PbotVerb::Attack  },
        { "атакуй",      PbotVerb::Attack  },
        { "атака",       PbotVerb::Attack  },
        { "бей",         PbotVerb::Attack  },
        { "фас",         PbotVerb::Attack  },
        { "attack",      PbotVerb::Attack  },
        { "kill",        PbotVerb::Attack  },

        { "защищай",     PbotVerb::Defend  },
        { "защита",      PbotVerb::Defend  },
        { "прикрой",     PbotVerb::Defend  },
        { "defend",      PbotVerb::Defend  },
        { "protect",     PbotVerb::Defend  },
        { "guard",       PbotVerb::Defend  },

        { "помогай",     PbotVerb::Assist  },
        { "ассист",      PbotVerb::Assist  },
        { "assist",      PbotVerb::Assist  },

        // Stay before Stop so "стой" is not shadowed; they are distinct strings but keeping the
        // hold-position family together makes the precedence obvious to the next editor.
        { "стоять",      PbotVerb::Stay    },
        { "стой",        PbotVerb::Stay    },
        { "на месте",    PbotVerb::Stay    },
        { "ждать",       PbotVerb::Stay    },
        { "жди",         PbotVerb::Stay    },
        { "stay",        PbotVerb::Stay    },
        { "wait",        PbotVerb::Stay    },
        { "hold",        PbotVerb::Stay    },

        { "отбой",       PbotVerb::Stop    },
        { "хватит",      PbotVerb::Stop    },
        { "стоп",        PbotVerb::Stop    },
        { "stop",        PbotVerb::Stop    },
        { "disengage",   PbotVerb::Stop    },

        { "статус",      PbotVerb::Status  },
        { "доклад",      PbotVerb::Status  },
        { "как дела",    PbotVerb::Status  },
        { "инфо",        PbotVerb::Status  },
        { "status",      PbotVerb::Status  },
        { "report",      PbotVerb::Status  },
        { "info",        PbotVerb::Status  },

        { "помощь",      PbotVerb::Help    },
        { "команды",     PbotVerb::Help    },
        { "хелп",        PbotVerb::Help    },
        { "help",        PbotVerb::Help    },
        { "commands",    PbotVerb::Help    },
    };

    // Punctuation the owner is likely to type around the address token ("боты,", "бот:", "боты!").
    constexpr std::string_view ADDRESS_TRIM = " \t,.:;!?-—";

    // Lowercases a UTF-8 line with correct Cyrillic folding by round-tripping through the engine's
    // wide-string helpers (Util.h:103 Utf8toWStr, :368 wstrToLower, :113 WStrToUtf8). Returns the
    // input unchanged if the text is not valid UTF-8 — a malformed line simply won't match any
    // keyword, which is the correct failure mode for a chat parser.
    std::string NormaliseLine(std::string_view msg)
    {
        std::wstring wide;
        if (!Utf8toWStr(msg, wide))
            return std::string(msg);

        wstrToLower(wide);

        std::string lowered;
        if (!WStrToUtf8(wide, lowered))
            return std::string(msg);

        return lowered;
    }

    // First whitespace-delimited word of the line, with ADDRESS_TRIM characters shaved off both
    // ends, plus the offset where the remainder begins.
    std::string_view FirstToken(std::string_view line, size_t& remainderStart)
    {
        size_t const begin = line.find_first_not_of(" \t");
        if (begin == std::string_view::npos)
        {
            remainderStart = line.size();
            return {};
        }

        size_t end = line.find_first_of(" \t", begin);
        if (end == std::string_view::npos)
            end = line.size();

        remainderStart = end;

        std::string_view token = line.substr(begin, end - begin);
        size_t const tokenBegin = token.find_first_not_of(ADDRESS_TRIM);
        if (tokenBegin == std::string_view::npos)
            return {};
        size_t const tokenEnd = token.find_last_not_of(ADDRESS_TRIM);
        return token.substr(tokenBegin, tokenEnd - tokenBegin + 1);
    }

    bool IsAddressToken(std::string_view token)
    {
        return std::find(std::begin(ADDRESS_TOKENS), std::end(ADDRESS_TOKENS), token) != std::end(ADDRESS_TOKENS);
    }

    // Could this first word plausibly be a bot's name?
    //
    // Without this gate, ANY sentence whose first word is not an address token gets treated as
    // "<name> <verb>", so ordinary Russian speech like "я сейчас в бой пойду" parses as an attack
    // order addressed to a bot named "я". The dispatcher would find no such bot and drop it, but a
    // parser that says "yes, an order" about plain conversation is one refactor away from being a
    // real bug — the selftest caught exactly this.
    //
    // Bot names come from PbotIdentity::PickName, which draws from a Latin-alphabet pool, so a real
    // name is always pure ASCII letters. Requiring that (plus a sane minimum length) rejects every
    // Cyrillic word and every short particle, while still accepting "Aldric follow".
    constexpr size_t MIN_NAME_TOKEN_LEN = 3;

    bool CouldBeBotName(std::string_view token)
    {
        if (token.size() < MIN_NAME_TOKEN_LEN)
            return false;

        for (char c : token)
            if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z'))
                return false;

        return true;
    }

    PbotVerb FindVerb(std::string_view text)
    {
        for (VerbPhrase const& entry : VERB_TABLE)
            if (text.find(entry.Phrase) != std::string_view::npos)
                return entry.Verb;

        return PbotVerb::None;
    }
}

bool PbotChat::Parse(std::string_view msg, bool addressImplied, PbotChatCommand& out)
{
    if (msg.empty())
        return false;

    std::string const lowered = NormaliseLine(msg);
    std::string_view line(lowered);

    std::string targetName;

    if (!addressImplied)
    {
        // Public channel: the line MUST open with an address token or a candidate bot name,
        // otherwise ordinary talk containing "стой" would order the whole squad around.
        size_t remainderStart = 0;
        std::string_view const token = FirstToken(line, remainderStart);
        if (token.empty())
            return false;

        if (!IsAddressToken(token))
        {
            if (!CouldBeBotName(token))
                return false;         // ordinary conversation, not an order

            targetName.assign(token); // candidate name; the dispatcher decides if it resolves
        }

        line = line.substr(remainderStart);
    }

    PbotVerb const verb = FindVerb(line);
    if (verb == PbotVerb::None)
        return false;

    out.Verb = verb;
    out.TargetName = std::move(targetName);
    return true;
}

char const* PbotChat::Acknowledgement(PbotVerb verb)
{
    switch (verb)
    {
        case PbotVerb::Follow:  return "Иду за тобой.";
        case PbotVerb::Stay:    return "Стою здесь.";
        case PbotVerb::Come:    return "Уже иду!";
        case PbotVerb::Attack:  return "В атаку!";
        case PbotVerb::Assist:  return "Помогаю тебе в бою.";
        case PbotVerb::Defend:  return "Прикрываю тебя.";
        case PbotVerb::Passive: return "Понял, в бой не лезу.";
        case PbotVerb::Stop:    return "Отхожу.";
        default:                return "Понял.";
    }
}

char const* PbotChat::HelpLine(uint32 index)
{
    static constexpr std::array<char const*, 9> LINES =
    {
        "Команды (скажи вслух с обращением \"боты ...\" или напиши в личку):",
        "  за мной / follow — идти за мной",
        "  стой / stay — стоять на месте",
        "  ко мне / come — вернуться ко мне",
        "  в бой / attack — атаковать мою цель",
        "  защищай / defend — прикрывать меня",
        "  не атакуй / passive — не вступать в бой",
        "  отбой / stop — выйти из боя",
        "  статус / status — доложить состояние",
    };

    return index < LINES.size() ? LINES[index] : nullptr;
}
