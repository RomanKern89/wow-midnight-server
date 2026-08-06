/*
 * Companion Bots — vendor and repair implementation. See pbot_upkeep.h.
 */

#include "pbot_upkeep.h"

#include "pbot_hearth.h"   // last resort when there is nowhere to walk to
#include "pbot_travel.h"   // repairers live in towns; the bot has to go to one
#include "pbot_auction.h"  // and so do auctioneers — the market is a town errand like any other
#include "pbot_profession.h" // and forges: between a vein and a bar there is a walk into town
#include "pbot_profession.h" // and forges: between a vein and a bar there is a walk into town

#include "Cell.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "DB2Structure.h"
#include "GameTime.h"
#include "Map.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "Player.h"
#include "StringFormat.h"

#include <cmath>
#include <vector>

namespace
{
    constexpr uint32 UPKEEP_POINT_ID = 0xC05;   // distinct from gather/quest/loot/travel ids

    // How often the "do I need town" question is asked. Bags fill and armour wears slowly.
    constexpr uint32 CHECK_INTERVAL_MS = 60000;

    // After a successful visit. Long, because a bot that just sold everything has nothing to sell.
    constexpr uint32 AFTER_VISIT_MS = 600000;   // 10 minutes

    // Same give-up budget every other walk-to-something behaviour has: an unreachable vendor must
    // not freeze the bot forever (that defect once left most of the population standing still).
    constexpr uint32 WALK_BUDGET_MS = 90000;

    // After hearthing home to find a vendor: enough time to arrive, look around and do business.
    constexpr uint32 AFTER_HEARTH_MS = 120000;

    // Home already, or no create position for this race — nothing more to try for a while.
    constexpr uint32 STRANDED_RETRY_MS = 300000;

    // Walking to the nearest town is a real journey, not a stroll to the next hut, so this budget is
    // far larger than the local walk. It still exists: an unreachable town must not become a bot's
    // whole life.
    constexpr uint32 TOWN_TRIP_BUDGET_MS = 600000;   // 10 minutes

    // How close the bot must be to a recorded position before concluding nobody is there.
    //
    // Must be at least PbotTravel::ARRIVED_RANGE, because that is where travel stops pushing. Set
    // tighter (20y) it created a dead band: StepToward reported "arrived" at 25y, the spot check
    // still said "not arrived", and the gap fell through to the no-path branch — so a bot that had
    // walked all the way to the repairer was logged as unable to reach it and hearthed away.
    // Measured, 9 of 10 "could not path" entries read exactly 25y, the arrival radius itself.
    constexpr float SPOT_ARRIVED_RANGE = PbotTravel::ARRIVED_RANGE + 5.0f;

    // How many times a bot standing at the destination re-checks before writing the place off, and
    // how long it waits between looks. Seconds, not minutes: this is waiting for the world around
    // the bot to finish loading, not for an NPC to walk back from a patrol.
    constexpr uint8 EMPTY_LOOKS_BEFORE_GIVING_UP = 4;
    constexpr uint32 LOOK_AGAIN_MS = 3000;

    // Two recorded positions this close are treated as the same place. Several repairer rows often
    // sit within a few yards inside one building, and the neighbour of an empty row is empty too.
    constexpr float SAME_SPOT_RANGE = 15.0f;

    // How long a position stays out of the running once a bot has found it deserted.
    constexpr uint32 SPOT_BLOCK_SECONDS = 3600;

    constexpr float APPROACH_OFFSET = 3.0f;

    void MoveToVendor(Player* bot, Creature* vendor)
    {
        float const angle = vendor->GetAbsoluteAngle(bot);
        float x = vendor->GetPositionX() + std::cos(angle) * APPROACH_OFFSET;
        float y = vendor->GetPositionY() + std::sin(angle) * APPROACH_OFFSET;
        float z = vendor->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        bot->GetMotionMaster()->MovePoint(UPKEEP_POINT_ID, x, y, z);
    }

    // Worst durability fraction across equipped items, or 1.0 when nothing can wear out.
    float WorstDurability(Player* bot)
    {
        float worst = 1.0f;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 const maxDurability = *item->m_itemData->MaxDurability;
            if (!maxDurability)
                continue;

            worst = std::min(worst, float(*item->m_itemData->Durability) / float(maxDurability));
        }

        return worst;
    }

    // Anything grey in the bags is money the bot is carrying around as dead weight.
    uint32 CountJunk(Player* bot)
    {
        uint32 count = 0;
        bot->ForEachItem(ItemSearchLocation::Inventory, [&count](Item* item)
        {
            if (item->GetQuality() == ITEM_QUALITY_POOR && !item->IsRefundable())
                ++count;
            return ItemSearchCallbackResult::Continue;
        });
        return count;
    }

    // Someone who will do the specific business this bot came for, alive and friendly. Hostile town
    // NPCs of the other faction would refuse anyway, and walking to one is a good way to die.
    //
    // The distinction between selling and repairing is not pedantry, it was the measured failure:
    // only 1519 of 6784 vendors can repair, and every single NPC the bots reached in the first run
    // was a seller. They walked over, sold nothing, repaired nothing and left — while carrying the
    // money to pay for it. Asking for "vendor OR repairer" is asking for the wrong NPC three times
    // out of four.
    class VendorCheck
    {
    public:
        VendorCheck(Player const* bot, float range, bool requireRepair)
            : _bot(bot), _range(range), _requireRepair(requireRepair) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive() || !_bot->IsWithinDist(creature, _range))
                return false;

            if (!creature->HasNpcFlag(_requireRepair ? UNIT_NPC_FLAG_REPAIR : UNIT_NPC_FLAG_VENDOR))
                return false;

            return creature->IsFriendlyTo(_bot);
        }

    private:
        Player const* _bot;
        float _range;
        bool _requireRepair;
    };

    // Counts every creature in range regardless of what it does or whose side it is on.
    //
    // Diagnostic only, and it exists to settle one question that every measurement so far has left
    // open: when a bot stands on a recorded repairer position and reports nobody, is the world there
    // empty to it, or is the record simply wrong? "No seller either" cannot answer that — sellers
    // are found opportunistically wherever a bot happens to be, never by walking to a coordinate,
    // so the two searches are not comparable. A raw creature count is.
    class CountEverything
    {
    public:
        CountEverything(Player const* bot, float range, uint32& total, uint32& friendly)
            : _bot(bot), _range(range), _total(total), _friendly(friendly) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !_bot->IsWithinDist(creature, _range))
                return false;

            ++_total;
            if (creature->IsFriendlyTo(_bot))
                ++_friendly;

            return false;   // never a match — the tally is the whole point
        }

    private:
        Player const* _bot;
        float _range;
        uint32& _total;
        uint32& _friendly;
    };

    void CountVisibleCreatures(Player* bot, uint32& total, uint32& friendly)
    {
        total = 0;
        friendly = 0;

        Creature* unused = nullptr;
        CountEverything counter(bot, PbotUpkeep::SEARCH_RANGE, total, friendly);
        Trinity::CreatureLastSearcher<CountEverything> searcher(bot, unused, counter);
        Cell::VisitAllObjects(bot, searcher, PbotUpkeep::SEARCH_RANGE);
    }

    Creature* FindVendor(Player* bot, bool requireRepair)
    {
        Creature* found = nullptr;
        VendorCheck check(bot, PbotUpkeep::SEARCH_RANGE, requireRepair);
        Trinity::CreatureLastSearcher<VendorCheck> searcher(bot, found, check);
        Cell::VisitAllObjects(bot, searcher, PbotUpkeep::SEARCH_RANGE);
        return found;
    }

    // Where a bot can actually get fixed, by map. Loaded once at boot.
    //
    // This table exists because waiting for a repairer to wander past does not work: measured over a
    // live run, exactly one bot in sixty had a repairer inside the 120y search, while bots with gear
    // at 0% stood in the countryside indefinitely. There are 1513 repair-capable spawns in the world
    // and they are all in towns — so the bot has to travel, and to travel it needs to know where.
    // A position alone is not enough: the NPC's faction has to travel with it. Measured, bots were
    // walking to repairers of the OPPOSING faction — Alliance bots sent into Orgrimmar to a Horde
    // blacksmith who will never trade with them. They arrived, the friendly check correctly refused
    // him, and the honest report "no repairer visible" hid a destination that was wrong from the
    // start. 5 of the 7 bots standing at that spot were Alliance.
    struct RepairSpot
    {
        Position pos;
        uint32 faction = 0;

        // When this position becomes worth trying again. Set whenever a bot walks here and finds no
        // trader: measured, such a bot sees 20-68 creatures around it, 7-18 of them friendly, and
        // not one that trades — so the row does not describe the live world and every bot that
        // picks it repeats the same wasted journey.
        //
        // Timed rather than permanent, because "nobody here" can also mean an NPC that is briefly
        // dead. An hour is long enough that the population stops wasting trips on genuinely wrong
        // rows, short enough that a real repairer is not written off for the life of the server.
        time_t blockedUntil = 0;
    };

    std::unordered_map<uint32, std::vector<RepairSpot>> _repairSpots;

    // The same table for auctioneers. A bot with goods to sell needs to reach a market exactly the
    // way a bot with worn armour needs to reach a smith, and measurement showed the market is the
    // harder errand: auctioneers live only in capitals, and 49 of 60 bots had things worth selling
    // while ONE could see an auctioneer. Everything that made the repairer trip work — faction and
    // phase filters, stepped walking, remembering deserted spots, a budget — applies unchanged.
    std::unordered_map<uint32, std::vector<RepairSpot>> _auctionSpots;

    // And the same again for workbenches, which are game objects rather than people — a forge does
    // not walk off, so a position here is a stronger promise than a spawn row for an NPC.
    //
    // Keyed by spell-focus id first, because "a workbench" is not one thing: a smelter needs a
    // forge, a cook needs a fire, and sending a bot to the wrong one is a wasted journey that ends
    // in the same refusal it set out to fix.
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<RepairSpot>>> _workbenchSpots;

    bool _repairSpotsLoaded = false;

    using Errand = PbotUpkeep::Errand;

    char const* ErrandName(Errand errand)
    {
        switch (errand)
        {
            case Errand::Auction:   return "an auctioneer";
            case Errand::Workbench: return "a workbench";
            default:                return "a repairer";
        }
    }

    bool NeedsSpace(Player* bot)  { return bot->GetFreeInventorySlotCount() <= PbotUpkeep::BAGS_FULL_AT; }
    bool NeedsRepair(Player* bot) { return WorstDurability(bot) < PbotUpkeep::REPAIR_BELOW; }

    // Who should this bot walk to? A repairer when the gear is what hurts — those nearly always sell
    // too, so it covers both errands in one trip — and otherwise anyone who buys.
    Creature* FindForNeed(Player* bot)
    {
        if (NeedsRepair(bot))
            if (Creature* repairer = FindVendor(bot, /*requireRepair*/ true))
                return repairer;

        if (NeedsSpace(bot))
            return FindVendor(bot, /*requireRepair*/ false);

        return nullptr;
    }

    // Nearest known repairer on the bot's own map. Linear over that map's spots — a few hundred
    // entries, walked at most once a minute per bot, which is cheaper than any index would repay.
    // Would this NPC's faction actually deal with this bot? Cheap enough to run per candidate, and
    // it is the difference between a destination and a wasted journey.
    bool WouldTrade(Player const* bot, uint32 npcFaction)
    {
        FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(npcFaction);
        FactionTemplateEntry const* ours = bot->GetFactionTemplateEntry();
        if (!theirs || !ours)
            return true;   // unknown either side: let the live check at the destination decide

        return !theirs->IsHostileTo(ours);
    }

    // One bot's wasted journey is every bot's lesson. Blocking the position in the shared table —
    // rather than in the walker's own memory — is what stops fifty-nine others repeating it.
    // The per-map table for one errand. Workbenches are looked up by focus first — a forge is not a
    // cooking fire — and an unknown focus simply has no table, which reads as "nowhere to go".
    std::vector<RepairSpot>* SpotsFor(uint32 mapId, Errand errand, uint32 focus)
    {
        if (errand == Errand::Workbench)
        {
            auto const byFocus = _workbenchSpots.find(focus);
            if (byFocus == _workbenchSpots.end())
                return nullptr;

            auto const byMap = byFocus->second.find(mapId);
            return byMap == byFocus->second.end() ? nullptr : &byMap->second;
        }

        auto& table = errand == Errand::Auction ? _auctionSpots : _repairSpots;
        auto const itr = table.find(mapId);
        return itr == table.end() ? nullptr : &itr->second;
    }

    void MarkSpotEmpty(Player* bot, Position const& where, Errand errand, uint32 focus)
    {
        std::vector<RepairSpot>* spots = SpotsFor(bot->GetMapId(), errand, focus);
        if (!spots)
            return;

        time_t const until = GameTime::GetGameTime() + SPOT_BLOCK_SECONDS;
        uint32 blocked = 0;

        for (RepairSpot& spot : *spots)
        {
            if (spot.pos.GetExactDist2d(where.GetPositionX(), where.GetPositionY()) < SAME_SPOT_RANGE
                && spot.blockedUntil < until)
            {
                spot.blockedUntil = until;
                ++blocked;
            }
        }

        if (blocked)
            TC_LOG_INFO("scripts.bots", "pbot upkeep: {} rows around that position are now out of "
                "the running for {} minutes ({} found it deserted)", blocked,
                SPOT_BLOCK_SECONDS / 60, bot->GetName());
    }

    bool FindNearestTownSpot(Player* bot, Errand errand, uint32 focus, Position& out)
    {
        std::vector<RepairSpot> const* spots = SpotsFor(bot->GetMapId(), errand, focus);
        if (!spots || spots->empty())
            return false;

        time_t const now = GameTime::GetGameTime();
        float best = std::numeric_limits<float>::max();

        for (RepairSpot const& spot : *spots)
        {
            if (!WouldTrade(bot, spot.faction))
                continue;

            if (spot.blockedUntil > now)
                continue;

            float const dist = bot->GetExactDistSq(spot.pos.GetPositionX(), spot.pos.GetPositionY(),
                spot.pos.GetPositionZ());
            if (dist < best)
            {
                best = dist;
                out = spot.pos;
            }
        }

        return best < std::numeric_limits<float>::max();
    }

    // Repair one equipped slot if it is worn and the bot can pay for it. Returns true only when the
    // item actually came back up, so the caller can report repairs done rather than repairs tried.
    bool RepairSlot(Player* bot, uint8 slot)
    {
        uint16 const pos = (INVENTORY_SLOT_BAG_0 << 8) | slot;

        Item* item = bot->GetItemByPos(pos);
        if (!item)
            return false;

        uint32 const maxDurability = *item->m_itemData->MaxDurability;
        if (!maxDurability || *item->m_itemData->Durability >= maxDurability)
            return false;

        bot->DurabilityRepair(pos, /*takeCost*/ true, /*discountMod*/ 0.0f);
        return *item->m_itemData->Durability >= maxDurability;
    }

    // Sell the junk, then repair. In that order on purpose: the sale is what pays for the repair,
    // exactly as it does for a player emptying their bags at the end of a run.
    void DoBusiness(Player* bot, Creature* vendor)
    {
        uint32 sold = 0;

        if (vendor->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        {
            // Collect first: selling mutates the inventory, and mutating a container while
            // iterating it is how this kind of loop goes wrong.
            std::vector<Item*> junk;
            bot->ForEachItem(ItemSearchLocation::Inventory, [&junk](Item* item)
            {
                if (item->GetQuality() == ITEM_QUALITY_POOR && !item->IsRefundable())
                    junk.push_back(item);
                return ItemSearchCallbackResult::Continue;
            });

            for (Item* item : junk)
            {
                uint32 const count = item->GetCount();
                if (bot->CanSellItemToVendor(item, count))
                    continue;   // the engine refused — a returned SellResult is an error

                if (!bot->SellItemToVendor(item, count))
                    ++sold;
            }
        }

        uint64 const moneyBefore = bot->GetMoney();
        uint32 repaired = 0;

        if (vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
        {
            // Item by item, weapons first — NOT DurabilityRepairAll, which is all-or-nothing and
            // therefore does exactly nothing for a bot too poor to fix its whole set at once. That
            // was the measured failure: bots walked to a vendor, repaired zero, and left with their
            // gear still at 0%. Per-slot, the engine simply skips what the purse will not cover, so
            // a thin purse still buys back the thing that matters most.
            //
            // Weapons come first because a broken weapon costs the bot every fight it takes, while
            // a scuffed belt costs it almost nothing — and a bot that cannot win fights cannot earn
            // the money to repair anything else.
            for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND })
                if (RepairSlot(bot, slot))
                    ++repaired;

            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (slot != EQUIPMENT_SLOT_MAINHAND && slot != EQUIPMENT_SLOT_OFFHAND)
                    if (RepairSlot(bot, slot))
                        ++repaired;
        }

        TC_LOG_INFO("scripts.bots", "pbot upkeep: {} sold {} junk at '{}', repaired {} items for {} "
            "(purse {}, gear now {:.0f}%)", bot->GetName(), sold, vendor->GetName(), repaired,
            moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0, bot->GetMoney(),
            WorstDurability(bot) * 100.0f);
    }
}

namespace
{
    // Loads every unconditional spawn carrying one npc flag into one of the town-spot tables.
    //
    // Unconditional matters: a phased row is a position where a bot will usually find bare ground —
    // 176 of the 1513 repairer spawns are phased, and a bot sent to one walks the distance for
    // nothing. (Event-gated and pooled rows measured at zero here, so they need no clause.)
    //
    // COALESCE because the spawn row may override the template's flags, and the engine prefers the
    // spawn's value (ObjectMgr::ChooseCreatureFlags). Read only the template and the table records
    // NPCs that are not what it thinks, and misses ones that are.
    uint32 LoadTownSpots(uint32 npcFlag, std::unordered_map<uint32, std::vector<RepairSpot>>& into,
        char const* what)
    {
        QueryResult result = WorldDatabase.Query(Trinity::StringFormat(
            "SELECT c.map, c.position_x, c.position_y, c.position_z, ct.faction FROM creature c "
            "JOIN creature_template ct ON ct.entry = c.id "
            "WHERE (COALESCE(c.npcflag, ct.npcflag) & {}) != 0 "
            "AND c.PhaseId = 0 AND c.PhaseGroup = 0", npcFlag).c_str());

        if (!result)
        {
            TC_LOG_ERROR("scripts.bots", "pbot: {} spot query returned nothing — bots will not be "
                "able to use them", what);
            return 0;
        }

        uint32 skipped = 0;
        do
        {
            Field* f = result->Fetch();
            uint32 const mapId = f[0].GetUInt32();

            // Continents only, for the same reason the population spots are: an NPC inside a raid
            // or a scripted instance is not somewhere a bot can simply walk to.
            MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
            if (!mapEntry || !mapEntry->IsContinent())
            {
                ++skipped;
                continue;
            }

            RepairSpot spot;
            spot.pos.Relocate(f[1].GetFloat(), f[2].GetFloat(), f[3].GetFloat());
            spot.faction = f[4].GetUInt32();
            into[mapId].push_back(spot);
        }
        while (result->NextRow());

        uint32 total = 0;
        for (auto const& [mapId, spots] : into)
            total += uint32(spots.size());

        TC_LOG_INFO("scripts.bots", "pbot: loaded {} {} spots across {} continent maps "
            "({} skipped as non-continent)", total, what, uint32(into.size()), skipped);
        return total;
    }

    // Where the forges, anvils and cooking fires are.
    //
    // Objects rather than people, so there is no faction and no npc flag to read — what identifies a
    // workbench is the focus it provides, and one object can provide up to four of them. Every focus
    // it offers gets its own entry, because a bot searches for the focus its recipe named.
    uint32 LoadWorkbenchSpots()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT gt.Data0, gt.Data11, gt.Data12, gt.Data13, g.map, "
            "g.position_x, g.position_y, g.position_z "
            "FROM gameobject g JOIN gameobject_template gt ON gt.entry = g.id "
            "WHERE gt.type = 8 AND g.PhaseId = 0 AND g.PhaseGroup = 0");

        if (!result)
        {
            TC_LOG_ERROR("scripts.bots", "pbot: workbench query returned nothing — bots will never "
                "smelt, and everything downstream of smelting stops with it");
            return 0;
        }

        uint32 total = 0;
        do
        {
            Field* f = result->Fetch();
            uint32 const mapId = f[4].GetUInt32();

            MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
            if (!mapEntry || !mapEntry->IsContinent())
                continue;

            RepairSpot spot;
            spot.pos.Relocate(f[5].GetFloat(), f[6].GetFloat(), f[7].GetFloat());

            for (uint8 i = 0; i < 4; ++i)
            {
                uint32 const focus = f[i].GetUInt32();
                if (!focus)
                    continue;

                _workbenchSpots[focus][mapId].push_back(spot);
                ++total;
            }
        }
        while (result->NextRow());

        TC_LOG_INFO("scripts.bots", "pbot: loaded {} workbench positions covering {} distinct focus "
            "types", total, uint32(_workbenchSpots.size()));
        return total;
    }
}

void PbotUpkeep::PreloadRepairSpots()
{
    if (_repairSpotsLoaded)
        return;
    _repairSpotsLoaded = true;   // set first: a failed query must not retry forever

    LoadTownSpots(4096, _repairSpots, "repairer");        // UNIT_NPC_FLAG_REPAIR
    LoadTownSpots(0x200000, _auctionSpots, "auctioneer"); // UNIT_NPC_FLAG_AUCTIONEER
    LoadWorkbenchSpots();
}

void PbotUpkeep::CountSpots(uint32& total, uint32& blocked)
{
    total = 0;
    blocked = 0;

    time_t const now = GameTime::GetGameTime();
    for (auto const& [mapId, spots] : _repairSpots)
    {
        total += uint32(spots.size());
        for (RepairSpot const& spot : spots)
            if (spot.blockedUntil > now)
                ++blocked;
    }
}

bool PbotUpkeep::HasVendorInReach(Player* bot)
{
    return bot && bot->IsInWorld() && FindForNeed(bot) != nullptr;
}

std::string PbotUpkeep::Describe(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return "not in world";

    uint32 const freeSlots = bot->GetFreeInventorySlotCount();
    float const durability = WorstDurability(bot);
    uint32 const junk = CountJunk(bot);

    // Both are reported because they fail independently: a bot can be standing next to somebody who
    // buys its junk and still have no one within a zone who will fix its weapon.
    Creature* seller = FindVendor(bot, /*requireRepair*/ false);
    Creature* repairer = FindVendor(bot, /*requireRepair*/ true);

    return Trinity::StringFormat("bags {} free, gear {:.0f}%, {} junk, purse {}, needs town: {}, "
        "seller {}, repairer {}",
        freeSlots, durability * 100.0f, junk, bot->GetMoney(), NeedsTown(bot) ? "YES" : "no",
        seller ? Trinity::StringFormat("{:.0f}y", bot->GetDistance(seller)) : "none",
        repairer ? Trinity::StringFormat("{:.0f}y", bot->GetDistance(repairer)) : "none");
}

bool PbotUpkeep::NeedsTown(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return false;

    // Goods in the bags count as a reason to go to town too — that is what makes the bot walk to a
    // market at all. Without it the auction house is only ever used by whoever happens to already be
    // standing in a capital.
    return NeedsSpace(bot) || NeedsRepair(bot) || PbotAuction::HasSellableGoods(bot)
        || PbotProfession::NeededFocus(bot) != 0;
}

bool PbotUpkeep::Tick(Player* bot, State& state, Position& home, uint32& homeMapId, uint32 diff)
{
    ObjectGuid& vendorGuid = state.vendorGuid;
    uint32& cooldownMs = state.cooldownMs;
    uint32& walkMs = state.walkMs;

    if (!bot || !bot->IsAlive() || bot->IsInCombat() || bot->InBattleground())
    {
        // A fight interrupts the errand but must NOT cancel it. Clearing the trip here is what
        // made long walks impossible: every skirmish on the way sent the bot back to choosing a
        // destination from wherever it had drifted to.
        vendorGuid.Clear();
        return false;
    }

    if (cooldownMs > diff)
        cooldownMs -= diff;
    else
        cooldownMs = 0;

    if (!vendorGuid.IsEmpty())
    {
        Creature* vendor = ObjectAccessor::GetCreature(*bot, vendorGuid);
        if (!vendor || !vendor->IsAlive())
        {
            vendorGuid.Clear();
            walkMs = 0;
            return false;
        }

        walkMs += diff;
        if (walkMs > WALK_BUDGET_MS)
        {
            vendorGuid.Clear();
            walkMs = 0;
            cooldownMs = CHECK_INTERVAL_MS;
            return false;
        }

        if (!bot->IsWithinDistInMap(vendor, INTERACTION_DISTANCE))
        {
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
                MoveToVendor(bot, vendor);
            return true;
        }

        bot->GetMotionMaster()->Clear();
        DoBusiness(bot, vendor);

        vendorGuid.Clear();
        walkMs = 0;
        state.hasTownGoal = false;   // errand done
        cooldownMs = AFTER_VISIT_MS;
        return true;
    }

    if (cooldownMs)
        return false;

    cooldownMs = CHECK_INTERVAL_MS;

    if (!NeedsTown(bot))
        return false;

    Creature* vendor = FindForNeed(bot);

    // Nobody in reach, but the gear is what hurts: walk to the nearest town that can fix it. This is
    // the whole difference between a bot that repairs and one that does not — repairers are in
    // towns, bots are in the field, and standing still waiting for the two to meet never works.
    // Which errand needs a journey? Gear first — a bot that cannot fight cannot earn — and the
    // market second, for a bot whose bags hold something worth selling.
    //
    // The market trip exists because reach, not willingness, was the whole bottleneck: measured,
    // 49 of 60 bots had goods worth listing and exactly one could see an auctioneer. Widening the
    // search helped a little; actually walking to a capital is what closes the gap.
    // Which errands does this bot have, and which destination is closest?
    //
    // A fixed order of precedence starved the last one on the list. Measured over thirty minutes:
    // 51 town errands began and exactly ONE was for a forge, because a bot almost always has
    // something worth selling and the market always won. The forge was never reached, so nothing
    // was ever smelted, so nothing was crafted — a queue whose third item never comes up is not a
    // priority list, it is a way of never doing the third thing.
    //
    // Nearest-first instead. A player who rides to a capital does everything there; the errands do
    // not compete for the trip, they share it, and whichever destination is closest is the one the
    // trip aims at. Repair still jumps the queue when the gear is what hurts — a bot that cannot
    // fight earns nothing to sell or smelt.
    Errand errand = Errand::Repair;

    // Asked BEFORE the test below, not inside it. Written as the last operand of an || chain, this
    // never ran: a bot almost always has something to sell, the chain short-circuited on that, and
    // focus stayed 0 — so the workbench was never even a candidate, which is precisely the failure
    // the nearest-errand change was meant to fix. Measured: still zero forge trips in thirty
    // minutes, with copper ore sitting in four bots' bags the whole time.
    uint32 focus = PbotProfession::NeededFocus(bot);

    if (!vendor && (NeedsRepair(bot) || NeedsSpace(bot) || PbotAuction::HasSellableGoods(bot)
        || focus))
    {
        // Stick with the destination already chosen. Picking the nearest spot afresh every attempt
        // is what stopped long walks finishing: each interruption left the bot somewhere new, so
        // "nearest" answered differently and the journey restarted instead of resuming.
        Position spot;
        bool haveSpot = state.hasTownGoal && state.townGoalMap == bot->GetMapId();
        if (haveSpot)
        {
            spot = state.townGoal;
            errand = state.townGoalErrand;
            focus = state.townGoalFocus;
        }
        else
        {
            // Ask every errand the bot actually has where it would send it, and take the shortest
            // walk. Repair overrides that choice — worn-out gear is the one errand whose value
            // decays while the bot walks somewhere else.
            struct Candidate { Errand errand; uint32 focus; Position pos; float distance; };
            std::vector<Candidate> candidates;

            auto consider = [&](Errand which, uint32 wantFocus)
            {
                Position found;
                if (!FindNearestTownSpot(bot, which, wantFocus, found))
                    return;

                candidates.push_back({ which, wantFocus, found,
                    bot->GetExactDist2d(found.GetPositionX(), found.GetPositionY()) });
            };

            if (NeedsRepair(bot) || NeedsSpace(bot))
                consider(Errand::Repair, 0);
            if (PbotAuction::HasSellableGoods(bot))
                consider(Errand::Auction, 0);
            if (focus)
                consider(Errand::Workbench, focus);

            Candidate const* pick = nullptr;
            for (Candidate const& candidate : candidates)
            {
                if (candidate.errand == Errand::Repair && NeedsRepair(bot))
                {
                    pick = &candidate;   // gear first, distance second
                    break;
                }

                if (!pick || candidate.distance < pick->distance)
                    pick = &candidate;
            }

            if (pick)
            {
                spot = pick->pos;
                errand = pick->errand;
                focus = pick->focus;
                haveSpot = true;
            }
        }

        if (haveSpot && !state.hasTownGoal)
        {
            state.townGoal = spot;
            state.townGoalMap = bot->GetMapId();
            state.townGoalErrand = errand;
            state.townGoalFocus = focus;
            state.emptyLooks = 0;
            state.hasTownGoal = true;
        }

        // 2D, to match how travel itself measures arrival. Height differences in a town — a
        // blacksmith one floor up — must not read as "still far away".
        float const toSpot = haveSpot
            ? bot->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) : 0.0f;

        // Standing where the table says a repairer is, and the live search above found nobody.
        // A row is a spawn position, not a promise: the NPC can be phased out of this bot's view,
        // despawned, or pooled. Measured, a bot sat 6 yards from a blacksmith it could not see and
        // re-chose that same destination every tick — 8238 times in twenty minutes.
        //
        // So the spot is written off for a while and the stone is used instead, which also moves
        // the bot elsewhere so the next attempt picks a different nearest spot.
        if (haveSpot && toSpot <= SPOT_ARRIVED_RANGE)
        {
            // A workbench errand ends on arrival. There is nobody to talk to — the forge is simply
            // there, and the crafting tick walks the last few yards to it and works. Running the
            // deserted-spot machinery here would write off perfectly good anvils for the crime of
            // not being people.
            if (errand == Errand::Workbench)
            {
                TC_LOG_INFO("scripts.bots", "pbot upkeep: {} reached a workbench for focus {}",
                    bot->GetName(), focus);
                state.emptyLooks = 0;
                state.hasTownGoal = false;
                walkMs = 0;
                cooldownMs = AFTER_VISIT_MS;
                return true;
            }

            // Arrived, and the search above saw nobody. Do not conclude anything yet: measured,
            // bots reaching a town spot reported "nearest seller none either" — not even an
            // ordinary merchant — which is not what a town looks like. The likeliest reading is
            // that the surrounding grid had not finished populating at the instant of the check.
            //
            // So look again a few times before writing the place off. Giving up on one glance cost
            // four of nine trips.
            if (++state.emptyLooks < EMPTY_LOOKS_BEFORE_GIVING_UP)
            {
                cooldownMs = LOOK_AGAIN_MS;
                return true;             // hold position and re-check shortly
            }

            // Height is reported because the search measures distance in three dimensions while
            // arrival is judged in two. A bot standing 25 yards away on the map but far below the
            // spot — under the terrain, or on the wrong floor of a tiered city — is a different
            // fault entirely from an NPC that simply is not there, and the two are indistinguishable
            // without this number.
            uint32 visible = 0;
            uint32 friendlyVisible = 0;
            CountVisibleCreatures(bot, visible, friendlyVisible);

            Creature* anySeller = FindVendor(bot, /*requireRepair*/ false);
            TC_LOG_INFO("scripts.bots", "pbot upkeep: {} reached the recorded {} spot ({:.0f}y flat, "
                "{:+.0f}y height) and after {} looks nobody is there; {} creatures in range "
                "({} friendly); nearest seller {}",
                bot->GetName(), ErrandName(errand), toSpot,
                spot.GetPositionZ() - bot->GetPositionZ(),
                uint32(state.emptyLooks), visible, friendlyVisible,
                anySeller ? Trinity::StringFormat("'{}' at {:.0f}y", anySeller->GetName(),
                    bot->GetDistance(anySeller)) : "none either");

            // Strike it from the shared table and carry on to the next nearest, rather than
            // hearthing home and losing the ground already covered. The walk budget is deliberately
            // NOT reset: the whole errand still has to finish inside it, so a bot cannot tour a
            // town's empty corners forever.
            MarkSpotEmpty(bot, state.townGoal, errand, focus);

            state.emptyLooks = 0;
            state.hasTownGoal = false;
            cooldownMs = 0;              // choose the next candidate immediately
            return true;                 // still on the errand
        }
        else if (haveSpot)
        {
            // Only when a journey actually begins.
            if (!walkMs)
                TC_LOG_INFO("scripts.bots", "pbot upkeep: {} sets out for {} {:.0f}y away "
                    "(gear {:.0f}%)", bot->GetName(), ErrandName(errand), toSpot,
                    WorstDurability(bot) * 100.0f);

            if (PbotTravel::StepToward(bot, spot))
            {
                walkMs += diff;
                if (walkMs > TOWN_TRIP_BUDGET_MS)
                {
                    TC_LOG_INFO("scripts.bots", "pbot upkeep: {} gave up walking to {} after {} "
                        "minutes, still {:.0f}y away", bot->GetName(), ErrandName(errand),
                        TOWN_TRIP_BUDGET_MS / 60000, toSpot);

                    // A workbench that could not be reached in ten minutes is written off, and the
                    // bot picks a different one. Measured: a bot set out for an anvil 528 yards
                    // away, spent the whole budget, ended 549 yards away — no progress at all — and
                    // then started the identical journey again, four times. The reason is in the
                    // engine's own complaint alongside it, "MoveSplineInitArgs::Validate failed":
                    // a game object's coordinates are its ORIGIN, which for a forge is inside the
                    // building, so the path leads into stone. The gather module hit exactly this
                    // and solved it by approaching nodes from beside them; here there is no single
                    // offset that works for ten thousand objects of every shape, so the honest fix
                    // is to let the population learn which ones are unreachable.
                    //
                    // Only workbenches. An NPC spot that is merely far away is still a good spot,
                    // and forges are abundant enough — 10031 positions — that discarding one costs
                    // nothing.
                    if (errand == Errand::Workbench)
                        MarkSpotEmpty(bot, state.townGoal, errand, focus);

                    walkMs = 0;
                    state.hasTownGoal = false;
                    cooldownMs = CHECK_INTERVAL_MS;
                    return false;
                }

                // Keep walking on the NEXT tick, not in a minute's time. The check interval was
                // already armed above, and leaving it armed made the journey one 50-yard hop per
                // minute — measured, bots with gear at 0% were technically travelling and still
                // standing in the same field a quarter of an hour later.
                cooldownMs = 0;
                return true;
            }

            // StepToward stopped short of the destination: no path. Fall through to the stone.
            TC_LOG_INFO("scripts.bots", "pbot upkeep: {} could not path to {} still {:.0f}y "
                "away; using the stone instead", bot->GetName(), ErrandName(errand), toSpot);

            if (errand == Errand::Workbench)
                MarkSpotEmpty(bot, state.townGoal, errand, focus);

            walkMs = 0;
            state.hasTownGoal = false;
            // fall through to the hearth below
        }
    }

    if (!vendor)
    {
        // Stranded: the bot needs a vendor and there is none in reach. Measured, this was the
        // common case — 17 of the 24 bots that needed town had nobody to trade with nearby — so
        // returning here and retrying forever means the whole feature never fires for most of them.
        //
        // So it hearths home, which is what the stone is for and what a player would do. Walking
        // instead was the alternative and a worse one: the bots that need this most are the ones
        // whose gear is at zero, and they would not survive the trip across hostile country.
        if (PbotHearth::GoHome(bot, home, homeMapId))
        {
            TC_LOG_INFO("scripts.bots", "pbot upkeep: {} found no vendor and hearthed home to look "
                "for one", bot->GetName());
            cooldownMs = AFTER_HEARTH_MS;
        }
        else
            cooldownMs = STRANDED_RETRY_MS;   // already home, or nowhere to go — do not spin on it

        return false;
    }

    vendorGuid = vendor->GetGUID();
    walkMs = 0;
    MoveToVendor(bot, vendor);
    return true;
}
