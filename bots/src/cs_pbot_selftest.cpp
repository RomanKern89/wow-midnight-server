/*
 * Companion Bots — in-process self test (".pbot selftest", console-capable).
 *
 * Why this exists: every behavioural part of the bot system needs a live owner Player, which needs
 * a game client. That makes the pure logic added in Phase 4 — the chat parser, the race/class
 * matrix, and the spell ids the rotations fire — untestable by any means available to the server
 * operator, right up until someone logs in and finds it broken.
 *
 * So these three things are checked from inside the running server instead, against the same data
 * the bots use at runtime:
 *   PARSER  — sample command lines are pushed through PbotChat::Parse and the resulting verb is
 *             compared against the expected one, including the traps: an unaddressed line must NOT
 *             be an order, and "не атакуй" must not read as an attack order.
 *   RACES   — every class is asked for a race on both factions via the same PickRaceForTeam the
 *             spawn path calls, so a class no faction can field shows up here, not on a failed
 *             ".pbot spawn".
 *   SPELLS  — every rotation spell id is resolved through sSpellMgr and printed with the name the
 *             server holds for it. A wrong id is a silently dead ladder step at runtime; here it
 *             prints as MISSING, and a right-id-wrong-spell shows up as a name mismatch a human
 *             can spot at a glance.
 *
 * Console::Yes on purpose — the whole point is that it runs with nobody logged in.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Common.h"            // LOCALE_enUS
#include "DBCEnums.h"          // DIFFICULTY_NONE
#include "RBAC.h"
#include "SharedDefines.h"     // ALLIANCE / HORDE
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "pbot_chat.h"
#include "pbot_gear.h"
#include "pbot_mgr.h"

#include <cstring>             // strcmp (spell name comparison)
#include <iterator>            // std::size

using namespace Trinity::ChatCommands;

namespace
{
    struct ParserCase
    {
        char const* Line;
        bool        AddressImplied;
        bool        ShouldParse;
        PbotVerb    Expected;
    };

    // Both the happy paths and the ways a naive parser goes wrong.
    constexpr ParserCase PARSER_CASES[] =
    {
        // Addressed public orders.
        { "боты за мной",              false, true,  PbotVerb::Follow  },
        { "боты, стой",                false, true,  PbotVerb::Stay    },
        { "бот ко мне",                false, true,  PbotVerb::Come    },
        { "боты в бой!",               false, true,  PbotVerb::Attack  },
        { "боты защищай меня",         false, true,  PbotVerb::Defend  },
        { "боты отбой",                false, true,  PbotVerb::Stop    },
        { "боты статус",               false, true,  PbotVerb::Status  },
        { "боты помощь",               false, true,  PbotVerb::Help    },
        { "bots follow",               false, true,  PbotVerb::Follow  },
        { "all stay",                  false, true,  PbotVerb::Stay    },

        // Negation must outrank the word it contains.
        { "боты не атакуй",            false, true,  PbotVerb::Passive },
        { "bots passive",              false, true,  PbotVerb::Passive },

        // Case folding, including Cyrillic uppercase, which a plain ASCII tolower would miss.
        { "БОТЫ СТОЙ",                 false, true,  PbotVerb::Stay    },
        { "Боты За Мной",              false, true,  PbotVerb::Follow  },

        // Whispers need no address token.
        { "за мной",                   true,  true,  PbotVerb::Follow  },
        { "stay",                      true,  true,  PbotVerb::Stay    },

        // Must NOT fire: ordinary conversation on a public channel, even when it contains a verb.
        // The second and third cases are the ones a naive "<first word> is a name" rule gets wrong,
        // which is exactly what this selftest caught on its first run.
        { "стой тут красиво",          false, false, PbotVerb::None    },
        { "я сейчас в бой пойду",      false, false, PbotVerb::None    },
        { "мы тут стоять будем",       false, false, PbotVerb::None    },
        { "ok stay here guys",         false, false, PbotVerb::None    },  // "ok" is too short to be
                                                                          // a bot name, so this is
                                                                          // chatter, not an order
        { "Aldric follow",             false, true,  PbotVerb::Follow  },  // name-addressed order
                                                                          // still works
        { "боты",                      false, false, PbotVerb::None    },  // address, no verb
        { "",                          false, false, PbotVerb::None    },
    };

    char const* VerbName(PbotVerb verb)
    {
        switch (verb)
        {
            case PbotVerb::None:    return "None";
            case PbotVerb::Follow:  return "Follow";
            case PbotVerb::Stay:    return "Stay";
            case PbotVerb::Come:    return "Come";
            case PbotVerb::Attack:  return "Attack";
            case PbotVerb::Assist:  return "Assist";
            case PbotVerb::Defend:  return "Defend";
            case PbotVerb::Passive: return "Passive";
            case PbotVerb::Stop:    return "Stop";
            case PbotVerb::Status:  return "Status";
            case PbotVerb::Help:    return "Help";
        }
        return "?";
    }

    struct SpellCheck
    {
        uint32      Id;
        char const* Expected;
    };

    // Every id the rotations can cast, paired with the name it is supposed to be.
    constexpr SpellCheck SPELL_CHECKS[] =
    {
        // Phase 1/2 five
        { 100,    "Charge" },            { 6343,   "Thunder Clap" },
        { 355,    "Taunt" },             { 23922,  "Shield Slam" },
        { 20271,  "Judgment" },          { 35395,  "Crusader Strike" },
        { 82326,  "Holy Light" },        { 2061,   "Flash Heal" },
        { 139,    "Renew" },             { 17,     "Power Word: Shield" },
        { 585,    "Smite" },             { 133,    "Fireball" },
        { 116,    "Frostbolt" },         { 185358, "Arcane Shot" },
        { 56641,  "Steady Shot" },       { 2643,   "Multi-Shot" },
        { 5384,   "Feign Death" },
        // Rogue
        { 1752,   "Sinister Strike" },   { 196819, "Eviscerate" },
        { 51723,  "Fan of Knives" },     { 1766,   "Kick" },
        { 5277,   "Evasion" },           { 185311, "Crimson Vial" },
        // Warlock
        { 686,    "Shadow Bolt" },       { 172,    "Corruption" },
        { 348,    "Immolate" },          { 234153, "Drain Life" },
        { 5740,   "Rain of Fire" },      { 104773, "Unending Resolve" },
        // Druid
        { 5176,   "Wrath" },             { 8921,   "Moonfire" },
        { 93402,  "Sunfire" },           { 774,    "Rejuvenation" },
        { 8936,   "Regrowth" },          { 22812,  "Barkskin" },
        // Shaman
        { 188196, "Lightning Bolt" },    { 188443, "Chain Lightning" },
        { 188389, "Flame Shock" },       { 8004,   "Healing Surge" },
        { 57994,  "Wind Shear" },        { 108271, "Astral Shift" },
        // Monk
        { 100780, "Tiger Palm" },        { 100784, "Blackout Kick" },
        { 107428, "Rising Sun Kick" },   { 101546, "Spinning Crane Kick" },
        { 116670, "Vivify" },            { 115203, "Fortifying Brew" },
        // Death Knight
        { 49998,  "Death Strike" },      { 47541,  "Death Coil" },
        { 50842,  "Blood Boil" },        { 43265,  "Death and Decay" },
        { 47528,  "Mind Freeze" },       { 48792,  "Icebound Fortitude" },
        // Probe only — not used by any ladder yet. Icy Touch (45477) turned out to be MISSING on
        // this build, so the DK ranged pull fell back to Death Coil; if Chains of Ice resolves it
        // is the better slow-and-pull and can take that step over.
        { 45524,  "Chains of Ice" },
        // Demon Hunter
        { 162243, "Demon's Bite" },      { 162794, "Chaos Strike" },
        { 188499, "Blade Dance" },       { 258920, "Immolation Aura" },
        { 185123, "Throw Glaive" },      { 183752, "Disrupt" },
        { 198589, "Blur" },
        // Evoker
        { 361469, "Living Flame" },      { 362969, "Azure Strike" },
        { 356995, "Disintegrate" },      { 357208, "Fire Breath" },
        { 355913, "Emerald Blossom" },   { 363916, "Obsidian Scales" },
        { 351338, "Quell" },
    };

    uint32 RunParserChecks(ChatHandler* handler)
    {
        uint32 failures = 0;
        handler->PSendSysMessage("--- parser ---");

        for (ParserCase const& test : PARSER_CASES)
        {
            PbotChatCommand cmd;
            bool const parsed = PbotChat::Parse(test.Line, test.AddressImplied, cmd);
            bool const ok = (parsed == test.ShouldParse) && (!parsed || cmd.Verb == test.Expected);

            if (!ok)
            {
                ++failures;
                handler->PSendSysMessage("FAIL \"%s\" -> parsed=%u verb=%s (expected parsed=%u verb=%s)",
                    test.Line, uint32(parsed), VerbName(parsed ? cmd.Verb : PbotVerb::None),
                    uint32(test.ShouldParse), VerbName(test.Expected));
            }
        }

        handler->PSendSysMessage("parser: %u cases, %u failures",
            uint32(std::size(PARSER_CASES)), failures);
        return failures;
    }

    uint32 RunRaceChecks(ChatHandler* handler)
    {
        uint32 failures = 0;
        handler->PSendSysMessage("--- races per class (alliance / horde) ---");

        for (uint8 classId = uint8(PbotClass::Warrior); classId <= uint8(PbotClass::Evoker); ++classId)
        {
            uint8 const allianceRace = PbotIdentity::PickRaceForTeam(ALLIANCE, classId);
            uint8 const hordeRace    = PbotIdentity::PickRaceForTeam(HORDE, classId);

            // A class with no race on EITHER faction would make ".pbot spawn" fail with no
            // explanation, so it counts as a failure here even though nothing crashes.
            if (!allianceRace && !hordeRace)
            {
                ++failures;
                handler->PSendSysMessage("FAIL class %s (%u): no race on either faction",
                    PbotIdentity::ClassNameForClassId(classId), uint32(classId));
                continue;
            }

            handler->PSendSysMessage("%-13s a=%-3u h=%-3u",
                PbotIdentity::ClassNameForClassId(classId), uint32(allianceRace), uint32(hordeRace));
        }

        return failures;
    }

    uint32 RunSpellChecks(ChatHandler* handler)
    {
        uint32 missing = 0;
        uint32 mismatched = 0;
        handler->PSendSysMessage("--- rotation spell ids ---");

        for (SpellCheck const& check : SPELL_CHECKS)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(check.Id, DIFFICULTY_NONE);
            if (!info)
            {
                ++missing;
                handler->PSendSysMessage("MISSING %-7u expected \"%s\"", check.Id, check.Expected);
                continue;
            }

            char const* name = info->SpellName ? info->SpellName->Str[LOCALE_enUS] : "";
            if (!name || !*name)
            {
                ++missing;
                handler->PSendSysMessage("NONAME  %-7u expected \"%s\"", check.Id, check.Expected);
                continue;
            }

            // Reported rather than counted as a hard failure: a renamed-but-correct spell is fine,
            // a genuinely wrong id usually shows up here as an obviously unrelated name.
            if (strcmp(name, check.Expected) != 0)
            {
                ++mismatched;
                handler->PSendSysMessage("DIFFERS %-7u server=\"%s\" expected=\"%s\"",
                    check.Id, name, check.Expected);
            }
        }

        handler->PSendSysMessage("spells: %u checked, %u missing, %u name mismatches",
            uint32(std::size(SPELL_CHECKS)), missing, mismatched);
        return missing;
    }
}

class pbot_selftest_commandscript : public CommandScript
{
public:
    pbot_selftest_commandscript() : CommandScript("pbot_selftest_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable pbotCommandTable =
        {
            // GM-only and console-capable: this is an operator diagnostic, not a player feature.
            { "selftest", HandlePbotSelfTestCommand, rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "pbot", pbotCommandTable },
        };
        return commandTable;
    }

    static bool HandlePbotSelfTestCommand(ChatHandler* handler)
    {
        handler->PSendSysMessage("=== pbot selftest ===");

        uint32 failures = 0;
        failures += RunParserChecks(handler);
        failures += RunRaceChecks(handler);
        failures += RunSpellChecks(handler);

        handler->PSendSysMessage("=== selftest %s (%u hard failures) ===",
            failures ? "FAILED" : "PASSED", failures);
        return true;
    }
};

void AddSC_pbot_selftest_commandscript()
{
    new pbot_selftest_commandscript();
}
