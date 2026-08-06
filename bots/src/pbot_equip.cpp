/*
 * Companion Bots — level-appropriate gear implementation.
 * See pbot_equip.h for why selection is derived from the item store and validated by the engine.
 */

#include "pbot_equip.h"

#include "DBCEnums.h"        // ItemContext
#include "Item.h"
#include "ItemDefines.h"     // EQUIP_ERR_OK
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"   // ITEM_QUALITY_*, EQUIPMENT_SLOT_*

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

namespace
{
    // The slots worth filling. Deliberately excludes shirt/tabard (cosmetic) and ammo.
    // ROBE is listed alongside CHEST because cloth chest pieces use it.
    constexpr std::array<InventoryType, 17> WANTED_SLOTS =
    {
        INVTYPE_HEAD, INVTYPE_NECK, INVTYPE_SHOULDERS, INVTYPE_CHEST, INVTYPE_ROBE,
        INVTYPE_WAIST, INVTYPE_LEGS, INVTYPE_FEET, INVTYPE_WRISTS, INVTYPE_HANDS,
        INVTYPE_FINGER, INVTYPE_TRINKET, INVTYPE_CLOAK,
        INVTYPE_WEAPON, INVTYPE_2HWEAPON, INVTYPE_SHIELD, INVTYPE_RANGED
    };

    // How many candidates to offer the engine per slot before giving up. Each rejected candidate
    // costs a CanEquipItem call, so this bounds the work; in practice the first few succeed for
    // any class that can use the slot at all.
    constexpr size_t MAX_CANDIDATES_PER_SLOT = 25;

    struct Candidate
    {
        uint32 Entry;
        int32  RequiredLevel;
    };

    // Per-slot candidates, sorted by required level ascending. Built once — the item store does not
    // change at runtime, and walking ~100k templates per bot spawn would be absurd.
    std::unordered_map<uint32, std::vector<Candidate>>& CandidateTable()
    {
        static std::unordered_map<uint32, std::vector<Candidate>> table;
        static bool built = false;

        if (built)
            return table;

        built = true;

        for (auto const& [entry, tmpl] : sObjectMgr->GetItemTemplateStore())
        {
            InventoryType const invType = tmpl.GetInventoryType();
            if (std::find(WANTED_SLOTS.begin(), WANTED_SLOTS.end(), invType) == WANTED_SLOTS.end())
                continue;

            // Uncommon/rare only: greys and whites are worse than nothing worth the effort, and
            // epics/legendaries would hand a bot raid gear for walking into a starting zone.
            uint32 const quality = tmpl.GetQuality();
            if (quality != ITEM_QUALITY_UNCOMMON && quality != ITEM_QUALITY_RARE)
                continue;

            // A required level of 0 is the marker of starter/quest/vendor filler with no progression
            // meaning; those are exactly what we are replacing.
            int32 const reqLevel = tmpl.GetBaseRequiredLevel();
            if (reqLevel <= 0)
                continue;

            table[uint32(invType)].push_back({ entry, reqLevel });
        }

        for (auto& [invType, candidates] : table)
            std::sort(candidates.begin(), candidates.end(),
                [](Candidate const& a, Candidate const& b) { return a.RequiredLevel < b.RequiredLevel; });

        uint32 total = 0;
        for (auto const& [invType, candidates] : table)
            total += uint32(candidates.size());

        TC_LOG_INFO("scripts.bots", "PbotEquip: built gear table — {} slots, {} candidate items.",
            uint32(table.size()), total);

        return table;
    }

    // Tries the best-fitting candidates for one slot until the engine accepts one.
    bool FillSlot(Player* bot, uint32 invType, uint8 botLevel)
    {
        auto const& table = CandidateTable();
        auto it = table.find(invType);
        if (it == table.end() || it->second.empty())
            return false;

        std::vector<Candidate> const& candidates = it->second;

        // Highest required level that is still <= the bot's level, then walk downwards.
        auto upper = std::upper_bound(candidates.begin(), candidates.end(), int32(botLevel),
            [](int32 level, Candidate const& c) { return level < c.RequiredLevel; });

        size_t tried = 0;
        while (upper != candidates.begin() && tried < MAX_CANDIDATES_PER_SLOT)
        {
            --upper;
            ++tried;

            // The engine's own CanEquipItem runs inside here: class proficiency, armor type,
            // unique-equipped, everything. A refusal just means "try the next one".
            if (bot->StoreNewItemInBestSlots(upper->Entry, 1, ItemContext::NONE))
                return true;
        }

        return false;
    }
}

uint32 PbotEquip::GearUp(Player* bot)
{
    if (!bot)
        return 0;

    uint8 const level = bot->GetLevel();
    uint32 equipped = 0;

    for (InventoryType slot : WANTED_SLOTS)
    {
        // CHEST and ROBE compete for the same body slot; if one filled it, skip the other.
        if (slot == INVTYPE_ROBE && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST))
            continue;

        // Likewise a two-hander is pointless once a one-hander is in the main hand.
        if (slot == INVTYPE_2HWEAPON && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            continue;

        if (FillSlot(bot, uint32(slot), level))
            ++equipped;
    }

    TC_LOG_INFO("scripts.bots", "PbotEquip: bot {} (level {}) equipped {} items.",
        bot->GetName(), uint32(level), equipped);

    return equipped;
}

uint32 PbotEquip::EquipFromBags(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return 0;

    // Collect first, act second: equipping moves items between slots, and mutating the bags from
    // inside the traversal that walks them is asking for trouble.
    std::vector<Item*> carried;
    bot->ForEachItem(ItemSearchLocation::Inventory, [&carried](Item* item)
    {
        // "Inventory" starts at the BAG slots, so the bot's own equipped bags are in this walk.
        // Without this line a bot re-equips the sack it is already wearing, every pass, forever:
        // measured 590 of those in forty minutes and every one of them was a Large Red Sack.
        // What we want is only what is loose — inside a bag, or in the backpack.
        if (!item->IsInBag() && item->GetSlot() < INVENTORY_SLOT_ITEM_START)
            return ItemSearchCallbackResult::Continue;

        ItemTemplate const* proto = item->GetTemplate();
        if (proto && proto->GetInventoryType() != INVTYPE_NON_EQUIP)
            carried.push_back(item);

        return ItemSearchCallbackResult::Continue;
    });

    uint32 worn = 0;
    for (Item* item : carried)
    {
        ItemTemplate const* proto = item->GetTemplate();

        uint16 dest = NULL_SLOT;
        if (bot->CanEquipItem(NULL_SLOT, dest, item, /*swap*/ true) != EQUIP_ERR_OK)
            continue;   // wrong class, wrong armour type, too high a level — the engine decides

        // Never trade down. CanEquipItem happily hands back an occupied slot when asked to swap,
        // so without this a bot would take off a good breastplate to put on a worse one.
        if (Item* current = bot->GetItemByPos(dest))
        {
            ItemTemplate const* currentProto = current->GetTemplate();
            if (currentProto && currentProto->GetBaseItemLevel() >= proto->GetBaseItemLevel())
                continue;
        }

        // SwapItem is the client's own drag-onto-the-doll path: it unequips whatever was there into
        // the freed bag slot and puts this on, with every check the engine would run for a player.
        uint16 const from = item->GetPos();
        bot->SwapItem(from, dest);

        if (bot->GetItemByPos(dest) == item)
        {
            TC_LOG_INFO("scripts.bots", "pbot equip: {} put on {} from the bags",
                bot->GetName(), proto->GetDefaultLocaleName());
            ++worn;
        }
    }

    return worn;
}
