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

    // Bags are equipment too, but nothing above finds them: INVTYPE_BAG is not in that list, and
    // the quality filter below would reject them anyway because ordinary bags are Common.
    //
    // Without one a bot lives in the 16-slot backpack it was born with, and a full backpack is the
    // end of its questing: the engine refuses a turn-in it cannot pay the reward for. Measured on
    // the live realm — 22 of 64 bots owned no bag at all, 35 had a completely full backpack, and
    // the bots that did reach a quest taker were told "bags 0 free" 1,861 times in one run, walking
    // back to the same NPC after each refusal. Their vendor trips sold nothing, because nothing
    // they carried was grey.
    //
    // The four bag slots are filled the same way as any other slot, through the engine's own
    // CanEquipItem, so an item that is not really a bag simply gets refused.
    constexpr uint32 BAG_SLOT_KEY = uint32(INVTYPE_BAG);
    constexpr uint8 BAG_SLOTS_TO_FILL = 4;   // INVENTORY_SLOT_BAG_START .. BAG_END

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
            uint32 const quality = tmpl.GetQuality();

            // Bags: judged on capacity, not on level or rarity. A plain white 16-slot bag is exactly
            // what a levelling character carries, and requiring a level would exclude every one of
            // them. Store the capacity in the level field so the existing "walk down from the best
            // the bot may have" search picks the roomiest bag it can hold.
            if (invType == INVTYPE_BAG)
            {
                uint32 const slots = tmpl.GetContainerSlots();
                if (slots < 6 || quality > ITEM_QUALITY_RARE)
                    continue;

                table[BAG_SLOT_KEY].push_back({ entry, int32(slots) });
                continue;
            }

            if (std::find(WANTED_SLOTS.begin(), WANTED_SLOTS.end(), invType) == WANTED_SLOTS.end())
                continue;

            // Uncommon/rare only: greys and whites are worse than nothing worth the effort, and
            // epics/legendaries would hand a bot raid gear for walking into a starting zone.
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
    // Puts a bag straight into one bag slot, bypassing the backpack entirely.
    //
    // FillSlot cannot do this. It uses StoreNewItemInBestSlots, which finds room for the item in
    // the inventory first — so a bot whose backpack is full is refused, and a bot whose backpack is
    // full is precisely the one that needs a bag. Measured: of the 11 bots holding no bag at all,
    // 6 had zero free slots, and not one of them could be given anything.
    //
    // CanEquipNewItem targets the bag slot directly and asks the engine's own rules; a candidate it
    // will not accept simply loses to the next one down the list.
    bool EquipBagInSlot(Player* bot, uint8 bagSlot, uint8 capacityBudget)
    {
        auto const& table = CandidateTable();
        auto const it = table.find(BAG_SLOT_KEY);
        if (it == table.end() || it->second.empty())
            return false;

        std::vector<Candidate> const& candidates = it->second;

        // Roomiest bag this bot is entitled to, then downwards.
        auto upper = std::upper_bound(candidates.begin(), candidates.end(), int32(capacityBudget),
            [](int32 budget, Candidate const& c) { return budget < c.RequiredLevel; });

        size_t tried = 0;
        while (upper != candidates.begin() && tried < MAX_CANDIDATES_PER_SLOT)
        {
            --upper;
            ++tried;

            uint16 dest = 0;
            if (bot->CanEquipNewItem(bagSlot, dest, upper->Entry, false) != EQUIP_ERR_OK)
                continue;

            if (bot->EquipNewItem(dest, upper->Entry, ItemContext::NONE, true))
                return true;
        }

        return false;
    }

    // Which worn slot an inventory type occupies. NULL_SLOT for types that are not worn directly
    // (a robe and a chest piece both go to CHEST; a two-hander to MAINHAND).
    uint8 EquipmentSlotFor(InventoryType invType)
    {
        switch (invType)
        {
            case INVTYPE_HEAD:      return EQUIPMENT_SLOT_HEAD;
            case INVTYPE_NECK:      return EQUIPMENT_SLOT_NECK;
            case INVTYPE_SHOULDERS: return EQUIPMENT_SLOT_SHOULDERS;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:      return EQUIPMENT_SLOT_CHEST;
            case INVTYPE_WAIST:     return EQUIPMENT_SLOT_WAIST;
            case INVTYPE_LEGS:      return EQUIPMENT_SLOT_LEGS;
            case INVTYPE_FEET:      return EQUIPMENT_SLOT_FEET;
            case INVTYPE_WRISTS:    return EQUIPMENT_SLOT_WRISTS;
            case INVTYPE_HANDS:     return EQUIPMENT_SLOT_HANDS;
            case INVTYPE_FINGER:    return EQUIPMENT_SLOT_FINGER1;
            case INVTYPE_TRINKET:   return EQUIPMENT_SLOT_TRINKET1;
            case INVTYPE_CLOAK:     return EQUIPMENT_SLOT_BACK;
            case INVTYPE_WEAPON:
            case INVTYPE_2HWEAPON:  return EQUIPMENT_SLOT_MAINHAND;
            case INVTYPE_SHIELD:    return EQUIPMENT_SLOT_OFFHAND;
            case INVTYPE_RANGED:    return EQUIPMENT_SLOT_RANGED;
            default:                return NULL_SLOT;
        }
    }

    bool FillSlot(Player* bot, uint32 invType, uint8 botLevel, int32 floorRequiredLevel)
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

            // Nothing the bot already wears, and nothing worse.
            if (upper->RequiredLevel <= floorRequiredLevel)
                continue;

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

        // Replace worse gear, do not merely fill holes.
        //
        // GearUp used to leave an occupied slot alone, so a bot that existed before a gear rule wore
        // whatever it was handed forever: measured 2026-10-22, level-30 bots hitting for 20-100
        // against creatures with 3,000-1,400,000 health. The bots that already exist are exactly the
        // ones that need the upgrade, so the replacement has to happen here, not only at creation.
        uint8 const equipSlot = EquipmentSlotFor(slot);
        Item* current = equipSlot != NULL_SLOT
            ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot) : nullptr;
        int32 const floorLevel = current ? current->GetTemplate()->GetBaseRequiredLevel() : 0;

        if (current && floorLevel >= int32(level))
            continue;   // already the best tier this level allows

        // Replacing a worn piece stores the new one in the bags first, so a bot with no free slot
        // cannot upgrade and the engine logs "can't equip or store initial item" for every slot and
        // every candidate it tries. Measured 2026-10-23: hundreds of those lines at each boot.
        // Nothing is lost by skipping: the bot upgrades on a later load once it has room.
        if (current && bot->GetFreeInventorySlotCount() < 1)
            continue;

        if (FillSlot(bot, uint32(slot), level, floorLevel))
        {
            if (current)
            {
                // The new piece is in the bags; only now is the old one retired, and the existing
                // swap path wears the new one. The other order leaves the bot naked whenever the
                // engine refuses the replacement.
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, equipSlot, true);
                EquipFromBags(bot);

                TC_LOG_INFO("scripts.bots", "PbotEquip: {} replaced slot {} (was required level {}) "
                    "for level {}", bot->GetName(), uint32(equipSlot), floorLevel, uint32(level));
            }
            ++equipped;
        }
    }

    uint32 const bags = FillBags(bot);

    TC_LOG_INFO("scripts.bots", "PbotEquip: bot {} (level {}) equipped {} items and {} bags "
        "({} free slots).", bot->GetName(), uint32(level), equipped, bags,
        bot->GetFreeInventorySlotCount());

    return equipped + bags;
}

// Separate from GearUp on purpose. GearUp runs when a bot is CREATED, and bots outlive restarts —
// so a fix that only lives inside it reaches nobody who already exists. Measured after shipping
// exactly that: 60 bots came back from the database, none of them ran GearUp, and the 22 with no
// bag still had none. Bag slots are also the one kind of gear that can be topped up safely at any
// time: filling an EMPTY slot cannot displace anything.
uint32 PbotEquip::FillBags(Player* bot)
{
    if (!bot)
        return 0;

    uint8 const level = bot->GetLevel();
    uint32 bags = 0;

    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_START + BAG_SLOTS_TO_FILL
         && slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        // Never replace a bag that is already there — its contents would have nowhere to go.
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            continue;

        // FillSlot searches the list by the value stored in the level field, and for bags that
        // field holds CAPACITY. Passing the bot's level would mean a level-8 character could only
        // ever be offered an 8-slot bag and a level-5 one nothing at all, since the smallest bag
        // kept is 6. Give it a capacity budget that grows with level instead — roughly what a
        // player of that level actually carries, and never more than the game's largest ordinary
        // bag.
        uint8 const capacityBudget = uint8(std::min<uint32>(6u + level / 4u, 22u));

        if (EquipBagInSlot(bot, slot, capacityBudget))
            ++bags;
    }

    return bags;
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
