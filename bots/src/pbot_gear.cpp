/*
 * Fake-player companion bots — identity data implementation.
 * See pbot_gear.h and DESIGN_PHASE3.md SS8.7.
 */

#include "pbot_gear.h"
#include "CharacterCache.h"
#include "DB2Stores.h"      // sChrRacesStore — race existence + iteration (Phase 4B)
#include "ObjectGuid.h"
#include "ObjectMgr.h"      // sObjectMgr->GetPlayerInfo — the creatability oracle (Phase 4B)
#include "Player.h"         // Player::TeamForRace
#include "Random.h"
#include "SharedDefines.h"
#include "Util.h"

#include <iterator>
#include <limits>
#include <vector>

namespace
{
    // One row per playable class. Token is the canonical spelling accepted by ".pbot spawn" and
    // printed in usage messages; Aliases are the shorthands players actually type, including the
    // Russian ones, since the owner of this realm types Russian. Display is what the bot list and
    // status lines show.
    struct ClassEntry
    {
        PbotClass        Class;
        char const*      Token;
        char const*      Display;
        std::string_view Aliases[4];
    };

    constexpr ClassEntry CLASS_TABLE[] =
    {
        { PbotClass::Warrior,     "warrior", "Warrior",      { "war",    "warr",  "воин",     "вар"     } },
        { PbotClass::Paladin,     "paladin", "Paladin",      { "pala",   "pal",   "паладин",  "пал"     } },
        { PbotClass::Hunter,      "hunter",  "Hunter",       { "hunt",   "hunt",  "охотник",  "хант"    } },
        { PbotClass::Rogue,       "rogue",   "Rogue",        { "rog",    "",      "разбойник","рога"    } },
        { PbotClass::Priest,      "priest",  "Priest",       { "pri",    "",      "жрец",     "прист"   } },
        { PbotClass::DeathKnight, "dk",      "Death Knight", { "death",  "deathknight", "рыцарь", "дк"  } },
        { PbotClass::Shaman,      "shaman",  "Shaman",       { "sham",   "",      "шаман",    "шам"     } },
        { PbotClass::Mage,        "mage",    "Mage",         { "mag",    "",      "маг",      ""        } },
        { PbotClass::Warlock,     "warlock", "Warlock",      { "lock",   "wl",    "чернокнижник", "лок" } },
        { PbotClass::Monk,        "monk",    "Monk",         { "mnk",    "",      "монах",    ""        } },
        { PbotClass::Druid,       "druid",   "Druid",        { "dru",    "",      "друид",    "дру"     } },
        { PbotClass::DemonHunter, "dh",      "Demon Hunter", { "demon",  "demonhunter", "охотник на демонов", "дх" } },
        { PbotClass::Evoker,      "evoker",  "Evoker",       { "evo",    "",      "аспект",   "эвокер"  } },
    };

    ClassEntry const* FindClass(uint8 unitClassId)
    {
        for (ClassEntry const& entry : CLASS_TABLE)
            if (uint8(entry.Class) == unitClassId)
                return &entry;
        return nullptr;
    }
    // Player-style names: latin alphabet, 4-8 chars, capitalized, no three-in-a-row letters,
    // no reserved/profane tokens. Player::Create's owning session normalizes casing; these are
    // already in canonical form. 50 entries keeps collisions rare even with many bots spawned.
    constexpr char const* NamePool[] =
    {
        "Aldric",  "Brannor", "Caedwyn", "Doralin", "Eowin",   "Fenwick", "Gorim",   "Halvar",
        "Ivorn",   "Jareth",  "Kaelen",  "Lorwyn",  "Maddox",  "Nyrelle", "Orlin",   "Perrin",
        "Quorra",  "Rowan",   "Selwyn",  "Tavish",  "Ulric",   "Varenne", "Wystan",  "Xandri",
        "Yorik",   "Zephra",  "Bryndel", "Corwin",  "Daelin",  "Emberly", "Farrow",  "Galwen",
        "Hesper",  "Ithric",  "Kessa",   "Lyanna",  "Merrick", "Norwyn",  "Ondrel",  "Pellin",
        "Riona",   "Sarael",  "Torvald", "Verric",  "Wrenna",  "Aldous",  "Brisa",   "Cadoc",
        "Delwin",  "Faelar"
    };
    constexpr std::size_t NamePoolCount = sizeof(NamePool) / sizeof(NamePool[0]);

    // Max canonical player name length on retail is 12; we cap our generated names a touch
    // shorter so the single-letter fallback suffix never overflows.
    constexpr std::size_t MaxGeneratedNameLen = 11;

    bool NameIsFree(std::string const& name)
    {
        // SS9 A.4 risk 3 (roster name collisions) is covered HERE, not by a separate pbot_roster
        // lookup: every rostered bot — live OR temporarily dismissed — keeps its `characters` row in
        // Phase 3.2 (DismissBot no longer deletes it), so its name is present in sCharacterCache and
        // this check already rejects it. A rostered bot's name therefore cannot be handed to a new
        // bot while the original exists. (pbot_roster stores no name column; it would only ever
        // resolve back to these same cached character names, so an extra query would be redundant.)
        if (!sCharacterCache->GetCharacterGuidByName(name).IsEmpty())
            return false; // taken by an existing character (including any live/dismissed rostered bot)
        if (sObjectMgr->CheckPlayerName(name, LOCALE_enUS, true) != CHAR_NAME_SUCCESS)
            return false; // rejected by this realm's strictness / DB2 validation
        return true;
    }
}

bool PbotIdentity::ParseClass(std::string_view token, PbotClass& out)
{
    for (ClassEntry const& entry : CLASS_TABLE)
    {
        if (StringEqualI(token, entry.Token))
        {
            out = entry.Class;
            return true;
        }

        for (std::string_view alias : entry.Aliases)
        {
            if (!alias.empty() && StringEqualI(token, alias))
            {
                out = entry.Class;
                return true;
            }
        }
    }
    return false;
}

char const* PbotIdentity::ClassName(PbotClass classId)
{
    if (ClassEntry const* entry = FindClass(uint8(classId)))
        return entry->Display;
    return "Unknown";
}

uint8 PbotIdentity::ClassId(PbotClass classId)
{
    // PbotClass values ARE the engine CLASS_* ids (see pbot_mgr.h), so this is a pure cast.
    return uint8(classId);
}

char const* PbotIdentity::ClassNameForClassId(uint8 unitClassId)
{
    if (ClassEntry const* entry = FindClass(unitClassId))
        return entry->Display;
    return "Unknown";
}

char const* PbotIdentity::ClassTokenList()
{
    return "warrior|paladin|hunter|rogue|priest|dk|shaman|mage|warlock|monk|druid|dh|evoker|random";
}

PbotClass PbotIdentity::PickRandomClass()
{
    return CLASS_TABLE[urand(0, uint32(std::size(CLASS_TABLE) - 1))].Class;
}

bool PbotIdentity::IsValidCombo(uint8 raceId, uint8 classId)
{
    // The single source of truth is the same lookup Player::Create uses: a PlayerInfo exists only
    // for a race/class pair this realm can actually create (283 rows on our world DB). Checking
    // ChrRaces first keeps us from probing ids that are not races at all.
    if (!sChrRacesStore.LookupEntry(raceId))
        return false;

    return sObjectMgr->GetPlayerInfo(raceId, classId) != nullptr;
}

uint8 PbotIdentity::PickRaceForTeam(uint32 team, uint8 classId)
{
    // Collected fresh per call rather than cached: spawns are rare (a handful per session, capped
    // at PBOT_GLOBAL_MAX), the store is a flat in-memory array, and a cache would silently go stale
    // if the world data were reloaded. Correctness is worth more than the microseconds here.
    std::vector<uint8> candidates;
    candidates.reserve(16);

    for (ChrRacesEntry const* race : sChrRacesStore)
    {
        if (!race || race->ID > std::numeric_limits<uint8>::max())
            continue;

        uint8 const raceId = uint8(race->ID);

        if (uint32(Player::TeamForRace(raceId)) != team)
            continue;

        if (!sObjectMgr->GetPlayerInfo(raceId, classId))
            continue;

        candidates.push_back(raceId);
    }

    if (candidates.empty())
        return 0;

    return candidates[urand(0, uint32(candidates.size() - 1))];
}

std::string PbotIdentity::PickName()
{
    uint32 const start = urand(0, uint32(NamePoolCount - 1));

    // First pass: a free, valid base name from the pool.
    for (std::size_t i = 0; i < NamePoolCount; ++i)
    {
        std::string candidate = NamePool[(start + i) % NamePoolCount];
        if (NameIsFree(candidate))
            return candidate;
    }

    // Fallback: append a single lowercase letter (stays fully alphabetic so it keeps passing
    // CheckPlayerName, which rejects digits). CheckPlayerName also rejects any accidental
    // three-consecutive-letter result, so we simply skip anything it refuses.
    for (std::size_t i = 0; i < NamePoolCount; ++i)
    {
        std::string const base = NamePool[(start + i) % NamePoolCount];
        for (char c = 'a'; c <= 'z'; ++c)
        {
            std::string candidate = base + c;
            if (candidate.size() > MaxGeneratedNameLen)
                break; // this base is too long to suffix; try the next base
            if (NameIsFree(candidate))
                return candidate;
        }
    }

    return std::string(); // exhausted — caller reports spawn failure
}
