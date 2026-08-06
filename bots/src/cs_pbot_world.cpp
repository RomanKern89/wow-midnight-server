/*
 * Companion Bots — Phase 6 world-bot commands (TrinityCore master, retail 12.0.7).
 *
 *   .pbot world spawn <count> [level] [class]   — in-game: populate around where you stand
 *   .pbot world spawnat <map> <x> <y> <z> <count> [level]  — console: populate an explicit point
 *   .pbot world list                            — how many are live
 *   .pbot world clear                           — remove them all
 *
 * "spawnat" exists specifically so the server operator can create and observe bots with no game
 * client attached. Every earlier phase of this system needs a logged-in owner to do anything at
 * all, which makes it unverifiable from the console; a bot that answers to nobody can be spawned,
 * watched and cleaned up entirely from a terminal.
 *
 * GM-gated (RBAC_PERM_COMMAND_SERVER) rather than open to players: these bots cost a full Player
 * tick each and are not owned by anyone who could clean them up.
 */

#include "pbot_ai.h"
#include "pbot_profession.h" // the craft diagnostic: recipes known vs materials held
#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Battleground.h"
#include "BattlegroundMgr.h" // battleground template -> map mapping for "world bglist"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DB2Stores.h"       // sFactionStore, for the attackability probe
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ReputationMgr.h"
#include "GameObject.h"     // node name/entry in the "world nodes" diagnostic
#include "Map.h"
#include "MapManager.h"
#include "Player.h"
#include "Position.h"
#include "RBAC.h"
#include "SharedDefines.h"
#include "Util.h"
#include "World.h"          // sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)
#include "WorldSession.h"
#include "pbot_bg.h"        // battleground queueing (Phase 7)
#include "pbot_common.h"
#include "pbot_gather.h"    // FindNode + SEARCH_RANGE for the node diagnostic
#include "pbot_gear.h"
#include "pbot_mgr.h"
#include "pbot_migrate.h"     // OUTGROWN_BY, for the band diagnostic
#include "pbot_autonomy.h"    // target filter tally, for the "hunt" diagnostic
#include "pbot_personality.h" // per-bot character, for the "who" diagnostic
#include "pbot_quest.h"       // per-bot quest log summary (world quests)
#include "pbot_upkeep.h"      // vendor/repair need, for the "upkeep" diagnostic
#include "pbot_auction.h"     // goods, auctioneer reach and mail, for the "market" diagnostic
#include "pbot_world_spots.h" // zone-spread population points (world populate)

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Trinity::ChatCommands;

namespace
{
    // Gathers live creatures in range for the hunt probe. The engine's searchers return a single
    // result, and the question here is about a handful of neighbours, not the nearest one.
    class PbotNearbyCreatureCollector
    {
    public:
        PbotNearbyCreatureCollector(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* creature)
        {
            if (creature && creature->IsAlive() && _bot->IsWithinDist(creature, _range))
                Found.push_back(creature);
            return false;
        }

        std::vector<Creature*> Found;

    private:
        Player const* _bot;
        float _range;
    };

    // Spread successive bots around the anchor so a batch does not stack into one pillar of bodies.
    constexpr float WORLD_SPAWN_SPREAD = 6.0f;

    // Alternate factions across a batch so a populated area has both sides in it, the way a real
    // contested zone does.
    uint32 TeamForIndex(uint32 index)
    {
        return (index % 2) ? uint32(HORDE) : uint32(ALLIANCE);
    }

    Position OffsetAround(Position const& anchor, uint32 index)
    {
        float const angle = float(index) * 1.13f;   // irrational-ish step: no repeating pattern
        return Position(
            anchor.GetPositionX() + WORLD_SPAWN_SPREAD * std::cos(angle),
            anchor.GetPositionY() + WORLD_SPAWN_SPREAD * std::sin(angle),
            anchor.GetPositionZ(),
            anchor.GetOrientation());
    }

    // Shared body for both spawn entry points. Reports per-bot failures rather than aborting the
    // batch, so "8 of 10 spawned" is visible instead of silently getting fewer bots than asked for.
    uint32 SpawnBatch(ChatHandler* handler, uint32 mapId, Position const& anchor, uint32 count,
                      uint8 level, bool randomClass, PbotClass fixedClass)
    {
        uint32 spawned = 0;
        for (uint32 i = 0; i < count; ++i)
        {
            PbotClass const cls = randomClass ? PbotIdentity::PickRandomClass() : fixedClass;
            Position const pos = OffsetAround(anchor, i);

            PbotSpawnError err;
            if (PbotMgr::SpawnWorldBot(mapId, pos, cls, level, TeamForIndex(i), &err))
            {
                ++spawned;
                continue;
            }

            handler->PSendSysMessage("bot %u/%u failed: %s", i + 1, count,
                err.Reason.empty() ? "unknown error" : err.Reason.c_str());
        }
        return spawned;
    }

    uint8 ClampLevel(uint32 requested)
    {
        if (!requested)
            return 1;

        uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        return uint8(std::min<uint32>(requested, maxLevel ? maxLevel : 80));
    }
}

class pbot_world_commandscript : public CommandScript
{
public:
    pbot_world_commandscript() : CommandScript("pbot_world_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable worldCommandTable =
        {
            { "spawn",   HandleWorldSpawnCommand,   rbac::RBAC_PERM_COMMAND_SERVER, Console::No  },
            { "spawnat", HandleWorldSpawnAtCommand, rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "populate",HandleWorldPopulateCommand,rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "list",    HandleWorldListCommand,    rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "nodes",   HandleWorldNodesCommand,   rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "quests",  HandleWorldQuestsCommand,  rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "bands",   HandleWorldBandsCommand,   rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "upkeep",  HandleWorldUpkeepCommand,  rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "combat",  HandleWorldCombatCommand,  rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "market",  HandleWorldMarketCommand,  rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "time",    HandleWorldTimeCommand,    rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "craft",   HandleWorldCraftCommand,   rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "who",     HandleWorldWhoCommand,     rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "hunt",    HandleWorldHuntCommand,    rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "tele",    HandleWorldTeleCommand,    rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "bg",      HandleWorldBgCommand,      rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "bgdiag",  HandleWorldBgDiagCommand,  rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "bglist",  HandleWorldBgListCommand,  rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
            { "clear",   HandleWorldClearCommand,   rbac::RBAC_PERM_COMMAND_SERVER, Console::Yes },
        };
        static ChatCommandTable pbotCommandTable =
        {
            { "world", worldCommandTable },
        };
        static ChatCommandTable commandTable =
        {
            { "pbot", pbotCommandTable },
        };
        return commandTable;
    }

    // .pbot world spawn <count> [level] [class]
    static bool HandleWorldSpawnCommand(ChatHandler* handler, uint32 count,
                                        Optional<uint32> level, Optional<std::string_view> classToken)
    {
        Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!gm || !gm->IsInWorld() || !gm->GetMap())
            return false;

        if (!count || count > PBOT_GLOBAL_MAX)
        {
            handler->PSendSysMessage("Count must be between 1 and %u.", PBOT_GLOBAL_MAX);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Default to the GM's own level so the spawned bots suit the zone they land in.
        uint8 const useLevel = ClampLevel(level ? *level : gm->GetLevel());

        PbotClass fixedClass = PbotClass::Warrior;
        bool randomClass = true;
        if (classToken && !StringEqualI(*classToken, "random") && !StringEqualI(*classToken, "any"))
        {
            if (!PbotIdentity::ParseClass(*classToken, fixedClass))
            {
                handler->PSendSysMessage("Unknown class. Use %s", PbotIdentity::ClassTokenList());
                handler->SetSentErrorMessage(true);
                return false;
            }
            randomClass = false;
        }

        uint32 const spawned = SpawnBatch(handler, gm->GetMapId(), gm->GetPosition(), count,
                                          useLevel, randomClass, fixedClass);
        handler->PSendSysMessage("Spawned %u/%u world bots at level %u (%u live).",
            spawned, count, uint32(useLevel), PbotMgr::CountWorldBots());
        return true;
    }

    // .pbot world spawnat <map> <x> <y> <z> <count> [level] — the console-usable form.
    static bool HandleWorldSpawnAtCommand(ChatHandler* handler, uint32 mapId, float x, float y, float z,
                                          uint32 count, Optional<uint32> level)
    {
        if (!count || count > PBOT_GLOBAL_MAX)
        {
            handler->PSendSysMessage("Count must be between 1 and %u.", PBOT_GLOBAL_MAX);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // The map is resolved (and created if it is not loaded yet) inside SpawnWorldBot, using the
        // bot itself as the anchoring player — see the note on PbotMgr::SpawnWorldBot. Validating
        // the id here would just duplicate a check that has to happen there anyway.
        uint8 const useLevel = ClampLevel(level ? *level : 10);
        Position const anchor(x, y, z, 0.0f);

        uint32 const spawned = SpawnBatch(handler, mapId, anchor, count, useLevel, /*randomClass*/ true,
                                          PbotClass::Warrior);
        handler->PSendSysMessage("Spawned %u/%u world bots on map %u at (%.1f %.1f %.1f) level %u (%u live).",
            spawned, count, mapId, x, y, z, uint32(useLevel), PbotMgr::CountWorldBots());
        return true;
    }

    // .pbot world populate <count> [level] [map] — scatter bots over the world, one zone each.
    //
    // The difference from "spawnat" is the whole point of it: spawnat puts a crowd on one spot,
    // which is a test fixture, not a populated world. This spreads the batch over as many distinct
    // zones as it has members, using real creature spawn points so every bot lands on walkable
    // ground inside a zone the world actually uses. Omitting [map] uses every world map.
    static bool HandleWorldPopulateCommand(ChatHandler* handler, uint32 count,
                                           Optional<uint32> level, Optional<int32> mapFilter,
                                           Optional<uint32> maxMaps)
    {
        uint32 const live = PbotMgr::CountWorldBots() + PbotMgr::PopulateQueueSize();
        if (!count || count + live > PBOT_GLOBAL_MAX)
        {
            handler->PSendSysMessage("Count must be 1..%u and %u are already live or queued.",
                PBOT_GLOBAL_MAX - std::min(live, PBOT_GLOBAL_MAX), live);
            handler->SetSentErrorMessage(true);
            return false;
        }

        int32 const filter = mapFilter ? *mapFilter : -1;

        // Naming one map means one map; otherwise stay within the default breadth. Loading a
        // continent costs gigabytes (PbotWorldSpots::DEFAULT_MAX_MAPS), so this is the knob that
        // decides whether a populate call fits in RAM, not `count`.
        uint32 const mapBudget = maxMaps ? *maxMaps
                                         : (mapFilter ? 1u : PbotWorldSpots::DEFAULT_MAX_MAPS);

        std::vector<PbotWorldSpots::Spot> const spots = PbotWorldSpots::Pick(count, filter, mapBudget);
        if (spots.empty())
        {
            handler->PSendSysMessage("No population spots for map filter %d (%u spots / %u zones loaded).",
                filter, PbotWorldSpots::SpotCount(), PbotWorldSpots::ZoneCount(filter));
            handler->SetSentErrorMessage(true);
            return false;
        }

        // No explicit level means "whatever suits the zone" — see Spot::SuggestedLevel. An explicit
        // level still wins, because forcing one is how you set up a battleground bracket test.
        //
        // The batch is QUEUED, not spawned here. Creating a bot can pull a map, its grids, its
        // vmaps and its mmap tiles off disk synchronously; thirty of those inside this one command
        // held the world thread for over a minute and the anti-freeze watchdog killed the server.
        uint8 lowest = 255;
        uint8 highest = 0;
        std::set<uint32> maps;
        std::set<uint32> zones;
        std::vector<PbotPopulateRequest> batch;
        batch.reserve(spots.size());

        for (size_t i = 0; i < spots.size(); ++i)
        {
            PbotPopulateRequest req;
            req.MapId = spots[i].MapId;
            req.Pos   = spots[i].Pos;
            req.Class = PbotIdentity::PickRandomClass();
            req.Level = ClampLevel(level ? *level
                                         : (spots[i].SuggestedLevel ? spots[i].SuggestedLevel : 10));
            req.Team  = TeamForIndex(uint32(i));

            lowest = std::min(lowest, req.Level);
            highest = std::max(highest, req.Level);
            maps.insert(req.MapId);
            zones.insert(spots[i].ZoneId);
            batch.push_back(req);
        }

        PbotMgr::QueuePopulate(batch);

        handler->PSendSysMessage("Queued %u bots at levels %u-%u across %u zones on %u maps; they "
                                 "appear over about %u seconds (%u live, cap %u).",
            uint32(batch.size()), uint32(lowest), uint32(highest),
            uint32(zones.size()), uint32(maps.size()),
            uint32((batch.size() * PBOT_POPULATE_INTERVAL_MS) / 1000),
            PbotMgr::CountWorldBots(), PBOT_GLOBAL_MAX);
        return true;
    }

    // Prints one line per world bot INCLUDING its current position. The position matters: with no
    // game client there is no other way to see whether a bot is actually alive and moving, and
    // ".gps" is Console::No. Running this twice a few seconds apart is the observation that shows
    // the autonomous wander/chase behaviour is really running.
    static bool HandleWorldListCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();
        handler->PSendSysMessage("World bots live: %u (%u still queued, server-wide bot cap %u).",
            uint32(bots.size()), PbotMgr::PopulateQueueSize(), PBOT_GLOBAL_MAX);

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
            {
                handler->PSendSysMessage("  %s -- not resolvable", guid.ToString().c_str());
                continue;
            }

            handler->PSendSysMessage("  %-12s %-13s lvl %-3u hp %3.0f%% map %-4u (%.1f %.1f %.1f)%s%s",
                bot->GetName().c_str(),
                PbotIdentity::ClassNameForClassId(bot->GetClass()),
                uint32(bot->GetLevel()),
                bot->GetHealthPct(),
                bot->GetMapId(),
                bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                bot->IsInCombat() ? " [combat]" : "",
                bot->IsAlive() ? "" : " [dead]");
        }
        return true;
    }

    // Reports, per live world bot, what the gathering search actually sees from where it stands —
    // and the bot's own gathering skill. Without this, a bot that harvests nothing is ambiguous
    // between "no nodes here", "skill too low", and "the search is broken", which is exactly the
    // ambiguity that left gathering unverified after the first soak.
    static bool HandleWorldNodesCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();
        if (bots.empty())
        {
            handler->PSendSysMessage("No world bots live — spawn some first.");
            return true;
        }

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            GameObject* node = PbotGather::FindNode(bot);
            if (node)
            {
                handler->PSendSysMessage("  %-12s herb %-4u mine %-4u -> node \"%s\" (entry %u) at %.1fy",
                    bot->GetName().c_str(),
                    uint32(bot->GetSkillValue(SKILL_HERBALISM)),
                    uint32(bot->GetSkillValue(SKILL_MINING)),
                    node->GetName().c_str(), node->GetEntry(), bot->GetExactDist(node));
            }
            else
            {
                handler->PSendSysMessage("  %-12s herb %-4u mine %-4u -> no gatherable node within %.0fy",
                    bot->GetName().c_str(),
                    uint32(bot->GetSkillValue(SKILL_HERBALISM)),
                    uint32(bot->GetSkillValue(SKILL_MINING)),
                    PbotGather::SEARCH_RANGE);
            }
        }
        return true;
    }

    // What is in each bot's quest log? The only way to see questing working with no client: a bot
    // whose log stays empty is not questing, and one whose log fills but never shows "complete" is
    // taking quests it cannot finish.
    static bool HandleWorldQuestsCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();
        uint32 withQuests = 0;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            std::string const desc = PbotQuest::Describe(bot);
            if (desc != "quest log empty")
                ++withQuests;

            if (shown++ < 15)
                handler->PSendSysMessage("  %-12s zone %-5u %s", bot->GetName().c_str(),
                    bot->GetZoneId(), desc.c_str());
        }

        handler->PSendSysMessage("%u of %u world bots hold at least one quest.",
            withQuests, uint32(bots.size()));
        return true;
    }

    // Is the bot economy actually trading?
    //
    // Three things have to be true for auction selling to be worth anything, and they fail
    // independently: bots must HAVE goods worth listing, they must be able to REACH an auctioneer,
    // and the money must come back to them as mail they actually open. A count of listings alone
    // hides which of the three is missing.
    // Where does the day actually go?
    //
    // Every other diagnostic here answers "is X working". This one answers "what are they DOING",
    // which is a different question and the one that was missing: with the crafting chain finally
    // working end to end and still producing two items in forty minutes, the bottleneck stopped
    // being any single mechanism and became the share of the day each one gets.
    //
    // Pass anything as an argument to zero the counters and start a fresh window.
    static bool HandleWorldTimeCommand(ChatHandler* handler, Optional<std::string_view> reset)
    {
        if (reset)
        {
            PbotAI::ResetActivityBudget();
            handler->SendSysMessage("Activity budget reset; measuring from now.");
            return true;
        }

        // One SendSysMessage per line: the report is built as a block, and the chat handler shows
        // an embedded newline as a single unreadable run.
        std::string const report = PbotAI::DescribeActivityBudget();
        for (size_t start = 0; start <= report.size(); )
        {
            size_t const end = report.find('\n', start);
            handler->SendSysMessage(report.substr(start,
                end == std::string::npos ? std::string::npos : end - start).c_str());

            if (end == std::string::npos)
                break;
            start = end + 1;
        }

        return true;
    }

    // Why is a population that knows thousands of recipes making almost nothing?
    //
    // "Recipes known" and "crafts made" are far apart and every explanation for the gap sounds
    // plausible, so this counts the steps between them instead: knows a recipe, holds the materials
    // for one, needs a workbench for it. Whichever number collapses is the answer — and the reagents
    // bots are one short of naming what the world is not supplying.
    static bool HandleWorldCraftCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();

        uint32 withCraft = 0;
        uint32 withRecipes = 0;
        uint32 withMaterials = 0;
        uint32 needingWorkbench = 0;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            uint32 const craft = PbotProfession::CraftFor(bot);
            if (craft && bot->HasSkill(craft))
                ++withCraft;

            std::string const line = PbotProfession::Describe(bot);

            // Describe reports "N recipes known, M makeable now"; a bot with none of either is the
            // interesting case, so both are counted rather than only the happy one.
            if (line.find(" 0 recipes known") == std::string::npos
                && line.find("recipes known") != std::string::npos)
                ++withRecipes;
            if (line.find(", 0 makeable") == std::string::npos
                && line.find("makeable") != std::string::npos)
                ++withMaterials;

            if (PbotProfession::NeededFocus(bot))
                ++needingWorkbench;

            if (shown++ < 12)
                handler->PSendSysMessage("  %-12s lvl %-3u %s", bot->GetName().c_str(),
                    uint32(bot->GetLevel()), line.c_str());
        }

        handler->PSendSysMessage("%u of %u bots have a craft; %u know a recipe; %u hold materials "
            "for one; %u of those need a workbench.", withCraft, uint32(bots.size()), withRecipes,
            withMaterials, needingWorkbench);
        return true;
    }

    static bool HandleWorldMarketCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();

        uint32 withGoods = 0;
        uint32 nearAuctioneer = 0;
        uint32 withMail = 0;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            bool const goods = PbotAuction::HasSellableGoods(bot);
            if (goods)
                ++withGoods;
            if (bot->GetMailSize())
                ++withMail;

            std::string const line = PbotAuction::Describe(bot);
            if (line.find("auctioneer '") != std::string::npos)
                ++nearAuctioneer;

            if (shown++ < 12)
                handler->PSendSysMessage("  %-12s purse %-9u %s", bot->GetName().c_str(),
                    uint32(bot->GetMoney()), line.c_str());
        }

        handler->PSendSysMessage("%u of %u bots have goods to sell; %u can reach an auctioneer; "
            "%u have mail waiting.", withGoods, uint32(bots.size()), nearAuctioneer, withMail);
        return true;
    }

    // Is a bot in a fight actually WINNING it?
    //
    // The complaint this exists to settle: bots engage, their own health falls, and the thing they
    // are fighting sits at full health. That is consistent with several very different faults —
    // out of reach and swinging at air, attacking something immune, or simply hitting for nothing
    // with broken gear — and a kill count cannot tell them apart. Health on BOTH sides, plus the
    // distance and the state of the bot's weapon, can.
    static bool HandleWorldCombatCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();

        uint32 fighting = 0;
        uint32 withoutVictim = 0;
        uint32 victimUntouched = 0;   // target still at full health while the bot is not
        uint32 outOfReach = 0;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot || !bot->IsInCombat() || !bot->IsAlive())
                continue;

            ++fighting;

            Unit* victim = bot->GetVictim();
            if (!victim)
            {
                ++withoutVictim;
                if (shown++ < 15)
                    handler->PSendSysMessage("  %-12s hp %3.0f%% -- IN COMBAT WITH NO TARGET",
                        bot->GetName().c_str(), bot->GetHealthPct());
                continue;
            }

            float const range = bot->GetDistance(victim);
            bool const melee = bot->IsWithinMeleeRange(victim);
            if (!melee && range > 30.0f)
                ++outOfReach;

            if (victim->GetHealthPct() > 99.0f && bot->GetHealthPct() < 95.0f)
                ++victimUntouched;

            if (shown++ < 15)
                handler->PSendSysMessage("  %-12s hp %3.0f%% vs '%s' hp %3.0f%% at %.0fy%s",
                    bot->GetName().c_str(), bot->GetHealthPct(),
                    victim->GetName().c_str(), victim->GetHealthPct(), range,
                    melee ? " (melee)" : "");
        }

        handler->PSendSysMessage("%u of %u bots are fighting: %u have no target, %u are beyond 30y, "
            "%u are losing health to an untouched target.",
            fighting, uint32(bots.size()), withoutVictim, outOfReach, victimUntouched);
        return true;
    }

    // Is anybody due a trip to town, and could they take it?
    //
    // A soak that logs no vendor visits proves nothing on its own — early on the honest answer is
    // that nobody needed one. This separates the three cases: nobody needs town, somebody needs it
    // but there is no vendor in reach, or somebody needs it and should be going right now.
    static bool HandleWorldUpkeepCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();

        uint32 needing = 0;
        uint32 needingWithVendor = 0;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            bool const needs = PbotUpkeep::NeedsTown(bot);
            if (needs)
            {
                ++needing;
                if (PbotUpkeep::HasVendorInReach(bot))
                    ++needingWithVendor;
            }

            // Anyone who needs a trip is always worth printing; the rest only fill the sample.
            if (needs || shown < 12)
            {
                ++shown;
                handler->PSendSysMessage("  %-12s lvl %-3u %s", bot->GetName().c_str(),
                    uint32(bot->GetLevel()), PbotUpkeep::Describe(bot).c_str());
            }
        }

        handler->PSendSysMessage("%u of %u world bots need town; %u of those have a vendor within %.0fy.",
            needing, uint32(bots.size()), needingWithVendor, PbotUpkeep::SEARCH_RANGE);

        // The repairer table prunes itself from what the bots find on arrival, so its state is part
        // of the diagnosis: a large blocked count means the recorded data disagrees with the world.
        uint32 spots = 0;
        uint32 blocked = 0;
        PbotUpkeep::CountSpots(spots, blocked);
        handler->PSendSysMessage("Repairer positions: %u known, %u struck off as deserted.",
            spots, blocked);
        return true;
    }

    // Why did (or didn't) a bot move house?
    //
    // Migration compares a bot's level against the level band derived for the zone it stands in,
    // and then needs a zone whose band fits on an ALREADY LOADED map. A run where nothing moved is
    // ambiguous between "nobody has outgrown anything", "the zone has no band data" and "there was
    // nowhere to go" — this prints all three inputs so the answer is read, not guessed.
    static bool HandleWorldBandsCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();

        uint32 outgrown = 0;
        uint32 noBand = 0;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            uint8 const band = PbotWorldSpots::BandForZone(bot->GetMapId(), bot->GetZoneId());
            bool const isOutgrown = band && bot->GetLevel() > band + PbotMigrate::OUTGROWN_BY;

            if (!band)
                ++noBand;
            if (isOutgrown)
                ++outgrown;

            if (shown++ < 12)
                handler->PSendSysMessage("  %-12s lvl %-3u map %-5u zone %-6u band %-4u %s",
                    bot->GetName().c_str(), uint32(bot->GetLevel()), bot->GetMapId(),
                    bot->GetZoneId(), uint32(band),
                    !band ? "NO BAND DATA" : (isOutgrown ? "OUTGROWN -> should move" : "in band"));
        }

        handler->PSendSysMessage("%u bots: %u outgrown, %u without band data.",
            uint32(bots.size()), outgrown, noBand);

        // And where could they possibly go? Only loaded maps count, so this is the real menu.
        PbotWorldSpots::Spot probe;
        for (uint32 level : { 10u, 20u, 30u, 40u, 50u, 60u, 70u })
        {
            if (PbotWorldSpots::PickForLevel(uint8(level), probe))
                handler->PSendSysMessage("  a level-%u bot could move to map %u zone %u (band %u)",
                    level, probe.MapId, probe.ZoneId, uint32(probe.SuggestedLevel));
            else
                handler->PSendSysMessage("  a level-%u bot has NOWHERE to go on the loaded maps", level);
        }
        return true;
    }

    // Who are these bots, as characters? Prints each one's traits and the archetype they add up to.
    //
    // "They should all behave differently" is otherwise unfalsifiable from the outside: identical
    // and varied bots look the same for the first minute. This makes the spread readable directly,
    // and the tally at the end says whether the population is actually diverse or accidentally
    // clustered around the middle.
    static bool HandleWorldWhoCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();
        std::map<std::string, uint32> tally;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            PbotPersonality::Traits const& t = PbotPersonality::Of(guid);
            ++tally[t.Archetype];

            // Every bot, not a sample. The first version printed twenty of forty-nine, and the
            // measurement built on it compared eight bots against ten — a spread one lucky kill
            // could invert. A diagnostic that silently truncates produces confident wrong answers.
            ++shown;
            handler->PSendSysMessage("  %-12s %-12s агр %-3u остор %-3u прилеж %-3u общит %-3u бродяж %-3u жадн %-3u",
                bot->GetName().c_str(), t.Archetype,
                uint32(t.Aggression), uint32(t.Caution), uint32(t.Diligence),
                uint32(t.Sociability), uint32(t.Wanderlust), uint32(t.Greed));
        }

        std::string summary;
        for (auto const& kv : tally)
            summary += Trinity::StringFormat("{} x{}  ", kv.first, kv.second);

        handler->PSendSysMessage("%u bots: %s", uint32(bots.size()), summary.c_str());
        return true;
    }

    // Why is the levelling rate so low?
    //
    // Roughly a thousand experience per bot per twenty-five minutes is three to five kills — one
    // every six minutes or so. Three very different causes produce that same number: there is
    // nothing nearby the filter will accept, or there is and the bot is busy doing something else,
    // or it is fighting and killing very slowly. This prints all three at once: what each bot is
    // doing right now, and a breakdown of why the creatures around it were rejected.
    static bool HandleWorldHuntCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();

        uint32 fighting = 0, resting = 0, idleWithTargets = 0, idleNoTargets = 0;
        PbotAutonomy::RejectionTally total;
        uint32 shown = 0;

        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot || !bot->IsInWorld())
                continue;

            PbotAutonomy::RejectionTally const t = PbotAutonomy::ExplainTargets(bot, PbotAutonomy::SEARCH_RANGE);
            total.Considered += t.Considered;
            total.TooHighLevel += t.TooHighLevel;
            total.Elite += t.Elite;
            total.Critter += t.Critter;
            total.AlreadyInCombat += t.AlreadyInCombat;
            total.NotAttackable += t.NotAttackable;
            total.Accepted += t.Accepted;

            char const* state = bot->IsInCombat() ? "fighting"
                              : (bot->GetStandState() == UNIT_STAND_STATE_SIT ? "resting" : "idle");

            if (bot->IsInCombat()) ++fighting;
            else if (bot->GetStandState() == UNIT_STAND_STATE_SIT) ++resting;
            else if (t.Accepted) ++idleWithTargets;
            else ++idleNoTargets;

            // What it is fighting and how that fight is going. "In combat" alone hid the real
            // problem for most of this project; a victim at 100% health minute after minute means
            // the bot is being hit rather than hitting.
            Unit* victim = bot->GetVictim();
            std::string fight = victim
                ? Trinity::StringFormat(" -> '{}' at {:.0f}%", victim->GetName(), victim->GetHealthPct())
                : std::string();

            if (shown++ < 15)
                handler->PSendSysMessage("  %-12s lvl %-3u %-8s hp %3.0f%% zone %-6u near %-3u ok %-3u "
                                         "(high %u elite %u critter %u busy %u hostile-no %u)%s",
                    bot->GetName().c_str(), uint32(bot->GetLevel()), state, bot->GetHealthPct(),
                    bot->GetZoneId(), t.Considered, t.Accepted,
                    t.TooHighLevel, t.Elite, t.Critter, t.AlreadyInCombat, t.NotAttackable,
                    fight.c_str());
        }

        // Every creature in the world being rejected as "not attackable" points at the ATTACKER,
        // not at the creatures: IsValidAttackTarget refuses outright when the attacker is immune to
        // NPCs, and it also refuses anything the attacker cannot see. Both are properties of the
        // bot, so print them for one bot rather than guessing which of the two it is.
        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot || !bot->IsInWorld())
                continue;

            handler->PSendSysMessage("probe %s: immuneToNPC %s, immuneToPC %s, playerControlled %s, "
                                     "gamemaster %s, unitFlags 0x%08X | alive %s, ghostAura %s, "
                                     "deathState %u, hasCorpse %s",
                bot->GetName().c_str(),
                bot->IsImmuneToNPC() ? "YES" : "no",
                bot->IsImmuneToPC() ? "YES" : "no",
                bot->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED) ? "yes" : "NO",
                bot->IsGameMaster() ? "YES" : "no",
                uint32(*bot->m_unitData->Flags),
                // A ghost sees only ghosts: the engine gates every living creature behind the
                // ghost-visibility mask. A bot that died and came back wrong would be permanently
                // blind while looking perfectly healthy, which matches the split between the bots
                // that fight and the ones that never do.
                bot->IsAlive() ? "yes" : "NO",
                bot->HasAuraType(SPELL_AURA_GHOST) ? "YES" : "no",
                uint32(bot->getDeathState()),
                bot->GetCorpse() ? "YES" : "no");

            // Two candidates remain for a blanket refusal, and they need different fixes: either
            // everything nearby is FRIENDLY (wrong placement — bots parked among townsfolk), or the
            // bot is not AT WAR with the creature's faction, which IsValidAttackTarget treats as
            // "may not be attacked" even for otherwise hostile wildlife. Print both per creature.
            PbotNearbyCreatureCollector collector(bot, PbotAutonomy::SEARCH_RANGE);
            Creature* ignored = nullptr;
            Trinity::CreatureLastSearcher<PbotNearbyCreatureCollector> creatureSearcher(bot, ignored, collector);
            Cell::VisitAllObjects(bot, creatureSearcher, PbotAutonomy::SEARCH_RANGE);

            uint32 printed = 0;
            for (Creature* creature : collector.Found)
            {
                bool const friendly = bot->IsFriendlyTo(creature) || creature->IsFriendlyTo(bot);
                bool atWar = false;
                bool haveState = false;
                uint32 factionId = 0;

                if (FactionTemplateEntry const* tmpl = creature->GetFactionTemplateEntry())
                {
                    factionId = tmpl->Faction;
                    if (FactionEntry const* factionEntry = sFactionStore.LookupEntry(tmpl->Faction))
                        if (FactionState const* state = bot->GetReputationMgr().GetState(factionEntry))
                        {
                            haveState = true;
                            atWar = state->Flags.HasFlag(ReputationFlags::AtWar);
                        }
                }

                // CanSeeOrDetect is the first gate in IsValidAttackTarget and the only remaining
                // candidate for a blanket refusal: everything else has now been measured and ruled
                // out. If a bot cannot SEE the world, it can never choose to attack anything —
                // while creatures can still aggro it, which is exactly the one-sided combat seen.
                // CanSeeOrDetect is a chain of independent gates and it reports only the verdict.
                // Print each gate separately: phase, distance against sight range, the never-visible
                // shortcut and detection. Applying a plausible fix without knowing which gate fails
                // has already cost one wasted deploy — the phasing call changed nothing.
                handler->PSendSysMessage("   near '%s' lvl %u: friendly %s, faction %u, repState %s, "
                                         "atWar %s | samePhase %s, dist %.0f/%.0f, neverVisible %s, "
                                         "canDetect %s => canSee %s, valid %s",
                    creature->GetName().c_str(), uint32(creature->GetLevel()),
                    friendly ? "YES" : "no", factionId,
                    haveState ? "yes" : "NONE", atWar ? "yes" : "NO",
                    bot->InSamePhase(creature) ? "yes" : "NO",
                    bot->GetExactDist(creature), bot->GetSightRange(creature),
                    // IsNeverVisibleFor and CanDetect are both non-public, so neither is probed
                    // directly. Its default is "not in world or destroyed", which is observable:
                    // a creature we found by a grid search is in world by definition.
                    creature->IsInWorld() ? "no" : "YES",
                    // Detection is inferred, not measured: if phase, distance and in-world are all
                    // fine and canSee is still no, detection is the only gate left.
                    "n/a",
                    bot->CanSeeOrDetect(creature) ? "yes" : "NO",
                    bot->IsValidAttackTarget(creature) ? "yes" : "no");

                // The decisive experiment. Bots that get attacked DO fight back, and retaliation
                // goes through Unit::Attack, which validates the same way — so the target must be
                // acceptable in that moment. Asking the engine to attack directly separates "the
                // engine forbids it" from "our filter rejects it before ever asking".
                if (printed == 0 && !friendly && creature->IsAlive())
                {
                    handler->PSendSysMessage("   -> Attack('%s') returned %s",
                        creature->GetName().c_str(), bot->Attack(creature, true) ? "TRUE" : "false");

                    // Last unmeasured gate. An observer that is itself INVISIBLE cannot see units
                    // which cannot see it — so a bot carrying any invisibility flag would be blind
                    // to the entire world while remaining a perfectly valid target for it.
                    handler->PSendSysMessage("   -> bot invis 0x%llX detect 0x%llX | creature invis 0x%llX detect 0x%llX",
                        static_cast<unsigned long long>(bot->m_invisibility.GetFlags()),
                        static_cast<unsigned long long>(bot->m_invisibilityDetect.GetFlags()),
                        static_cast<unsigned long long>(creature->m_invisibility.GetFlags()),
                        static_cast<unsigned long long>(creature->m_invisibilityDetect.GetFlags()));
                }

                if (++printed >= 4)
                    break;
            }
            break;
        }

        handler->PSendSysMessage("states: %u fighting, %u resting, %u idle WITH targets, %u idle with none",
            fighting, resting, idleWithTargets, idleNoTargets);
        handler->PSendSysMessage("creatures seen %u -> acceptable %u (rejected: level %u, elite %u, "
                                 "critter %u, already fighting %u, not attackable %u)",
            total.Considered, total.Accepted, total.TooHighLevel, total.Elite, total.Critter,
            total.AlreadyInCombat, total.NotAttackable);
        return true;
    }

    // .pbot world tele <map> <x> <y> <z> — sends every live world bot there.
    //
    // Exists to prove cross-map teleport works for a client-less player, which is the prerequisite
    // for battlegrounds and for bots following an owner through a portal. Until the worldport ack
    // was handled server-side, a bot sent to another map was simply dismissed.
    static bool HandleWorldTeleCommand(ChatHandler* handler, uint32 mapId, float x, float y, float z)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();
        if (bots.empty())
        {
            handler->PSendSysMessage("No world bots live — spawn some first.");
            return true;
        }

        // Reports TeleportTo's own verdict per bot. The first attempt at this silently did nothing
        // and left no way to tell whether the call was refused, the port never started, or the ack
        // never landed — so each of those three states is now printed separately.
        uint32 accepted = 0;
        uint32 refused = 0;
        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            bool const ok = bot->TeleportTo(mapId, x, y, z, bot->GetOrientation());
            bool const porting = bot->IsBeingTeleportedFar();

            if (ok)
                ++accepted;
            else
                ++refused;

            handler->PSendSysMessage("  %-12s from map %-4u TeleportTo=%s teleportingFar=%s",
                bot->GetName().c_str(), bot->GetMapId(),
                ok ? "true" : "FALSE", porting ? "yes" : "no");
        }

        handler->PSendSysMessage("Teleport -> map %u (%.1f %.1f %.1f): %u accepted, %u refused.",
                                 mapId, x, y, z, accepted, refused);
        return true;
    }

    // .pbot world bg <battlemasterListId>  (2 = Warsong Gulch)
    //
    // Queues every live world bot. Entry happens on its own once the queue forms a match: each
    // bot's AI tick notices the invite and ports itself in. Whether a match forms at all depends on
    // the battleground's minimum player count, so this needs enough bots on BOTH factions —
    // "pbot world spawnat" alternates factions, so an even count gives a balanced queue.
    static bool HandleWorldBgCommand(ChatHandler* handler, uint16 battlemasterListId)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();
        if (bots.empty())
        {
            handler->PSendSysMessage("No world bots live — spawn some first.");
            return true;
        }

        uint32 queued = 0;
        uint32 alliance = 0;
        uint32 horde = 0;
        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            // A battleground only forms when both sides reach the minimum, so the faction split is
            // the first thing to check when 20 queued bots produce no match.
            if (bot->GetTeam() == ALLIANCE)
                ++alliance;
            else
                ++horde;

            std::string err;
            if (PbotBG::Queue(bot, battlemasterListId, err))
                ++queued;
            else
                handler->PSendSysMessage("  %-12s could not queue: %s", bot->GetName().c_str(), err.c_str());
        }

        handler->PSendSysMessage("Queued %u/%u world bots for battleground %u (alliance %u / horde %u). "
                                 "They enter by themselves once the queue forms a match.",
                                 queued, uint32(bots.size()), uint32(battlemasterListId), alliance, horde);
        return true;
    }

    // Per-bot battleground state: match status, position, flag visibility. Prints only the first
    // few bots — 20 identical lines would just overflow the console pane, and the interesting
    // question (does ANY bot see a flag, and has the match started) is answered by a handful.
    static bool HandleWorldBgDiagCommand(ChatHandler* handler)
    {
        std::vector<ObjectGuid> const bots = PbotMgr::GetWorldBots();
        uint32 shown = 0;
        for (ObjectGuid const& guid : bots)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!bot)
                continue;

            handler->PSendSysMessage("%-10s %s", bot->GetName().c_str(),
                PbotBG::DescribeBattlegroundState(bot).c_str());

            if (++shown >= 4)
                break;
        }

        if (!shown)
            handler->PSendSysMessage("No resolvable world bots.");
        return true;
    }

    // Which battlemaster list id leads to which map?
    //
    // Needed because queueing for battlemaster 2 put bots on map 489 — a legacy Warsong Gulch with
    // no script bound and therefore no flags, no gates, no objects at all. The modern WSG script
    // registers itself for map 2106. Rather than guess which id is playable, print the mapping the
    // engine itself holds and pick a battleground whose map actually has a script.
    static bool HandleWorldBgListCommand(ChatHandler* handler, Optional<uint16> maxId)
    {
        uint16 const limit = maxId ? *maxId : 40;
        uint32 found = 0;

        for (uint16 id = 1; id <= limit; ++id)
        {
            BattlegroundTemplate const* tmpl =
                sBattlegroundMgr->GetBattlegroundTemplateByTypeId(BattlegroundTypeId(id));
            if (!tmpl || tmpl->MapIDs.empty())
                continue;

            std::string maps;
            for (int32 mapId : tmpl->MapIDs)
            {
                if (!maps.empty())
                    maps += ",";
                maps += std::to_string(mapId);
            }

            handler->PSendSysMessage("  bm %-3u -> map(s) %s%s", uint32(id), maps.c_str(),
                tmpl->IsArena() ? " [arena]" : "");
            ++found;
        }

        handler->PSendSysMessage("%u battleground templates found (checked ids 1..%u).", found, uint32(limit));
        return true;
    }

    static bool HandleWorldClearCommand(ChatHandler* handler)
    {
        uint32 const removed = PbotMgr::RemoveAllWorldBots();
        handler->PSendSysMessage("Removed %u world bots.", removed);
        return true;
    }
};

void AddSC_pbot_world_commandscript()
{
    new pbot_world_commandscript();
}
