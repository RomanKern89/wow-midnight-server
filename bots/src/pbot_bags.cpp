/*
 * Companion Bots — bags. See pbot_bags.h for why a bot should buy and make them rather than have
 * them conjured.
 */

#include "pbot_bags.h"

#include "pbot_belongings.h"  // "short of room" is one definition, shared

#include "Bag.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"

#include <algorithm>

namespace
{
    // The smallest bag the bot is wearing, and where. Capacity 0 with a valid slot means the slot
    // is EMPTY, which is the best possible place to put a new bag: nothing has to move.
    struct WorstBag
    {
        uint8 Slot = 0;
        uint32 Capacity = 0;
        bool Found = false;
        bool SlotEmpty = false;
    };

    WorstBag FindWorstBag(Player* bot)
    {
        WorstBag worst;

        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                return { slot, 0, true, true };   // an empty slot beats every occupied one

            Bag* bag = item->ToBag();
            uint32 const capacity = bag ? bag->GetBagSize() : 0;

            if (!worst.Found || capacity < worst.Capacity)
                worst = { slot, capacity, true, false };
        }

        return worst;
    }

    // A PLAIN bag, and only that. The specialised containers — herb, mining, enchanting, gem —
    // are the same item class and often far roomier for the price, but they accept only their own
    // kind of goods. A bot that bought a 24-slot herb bag would read as having plenty of room and
    // still have nowhere to put a quest reward, which is the exact failure this is meant to end.
    bool IsBag(ItemTemplate const* proto)
    {
        return proto && proto->GetClass() == ITEM_CLASS_CONTAINER
            && proto->GetSubClass() == ITEM_SUBCLASS_CONTAINER;
    }
}

uint32 PbotBags::CapacityWanted(Player* bot)
{
    if (!bot)
        return 0;

    WorstBag const worst = FindWorstBag(bot);
    if (!worst.Found)
        return 0;

    if (worst.SlotEmpty)
        return MIN_USEFUL_CAPACITY;

    // Every slot is taken, so a new bag has to beat the smallest one already worn — and it is only
    // worth WANTING while the bot is genuinely short of room.
    //
    // Both halves matter. Without the first, a bot with four six-slot sacks buys six-slot sacks
    // forever. Without the second, "wants a bag" is true for every bot at every moment (there is
    // always a bigger bag in the world), NeedsTown never goes false, and the population spends its
    // life walking to vendors — the errand would eat the day it was meant to save.
    if (!PbotBelongings::NeedsRoom(bot))
        return 0;

    return worst.Capacity + 1;
}

bool PbotBags::WantsABag(Player* bot)
{
    return CapacityWanted(bot) != 0;
}

bool PbotBags::WouldTake(Player* bot, ItemTemplate const* proto, uint64 price)
{
    if (!bot || !IsBag(proto))
        return false;

    uint32 const capacity = proto->GetContainerSlots();
    uint32 const wanted = CapacityWanted(bot);

    if (!wanted || capacity < wanted || capacity < MIN_USEFUL_CAPACITY)
        return false;

    if (proto->GetBaseRequiredLevel() > bot->GetLevel())
        return false;

    if (!price)
        return true;    // crafted or looted: nothing to weigh against the purse

    // Never let a bag leave the bot unable to repair. A broke bot dies, and a dead bot hands in
    // nothing — which is the very thing the extra room was for.
    return price <= uint64(float(bot->GetMoney()) * MAX_PURSE_FRACTION);
}

bool PbotBags::Install(Player* bot, Item* bag)
{
    if (!bot || !bag)
        return false;

    ItemTemplate const* proto = bag->GetTemplate();
    if (!IsBag(proto))
        return false;

    WorstBag const worst = FindWorstBag(bot);
    if (!worst.Found)
        return false;

    uint32 const capacity = proto->GetContainerSlots();

    // The easy case: an empty slot. Nothing to displace.
    if (worst.SlotEmpty)
    {
        uint16 dest = 0;
        if (bot->CanEquipItem(worst.Slot, dest, bag, false) != EQUIP_ERR_OK)
            return false;

        bot->EquipItem(dest, bag, true);
        TC_LOG_INFO("scripts.bots", "pbot bags: {} put on a {}-slot {}", bot->GetName(), capacity,
            proto->GetDefaultLocaleName());
        return true;
    }

    if (capacity <= worst.Capacity)
        return false;   // not an improvement on anything it wears

    // Replacing an occupied slot. The old bag is usually FULL, and unequipping it with items still
    // inside is how a bot would lose everything it was carrying — the engine refuses, but only if
    // asked properly, so ask properly.
    Item* old = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, worst.Slot);
    Bag* oldBag = old ? old->ToBag() : nullptr;
    if (oldBag && !oldBag->IsEmpty())
    {
        // Empty it first, into anywhere else that will take the contents. If even one item has
        // nowhere to go, leave the whole thing alone: a bigger bag is not worth a lost item, and
        // the bot will try again after its next vendor trip has freed some room.
        for (uint32 i = 0; i < oldBag->GetBagSize(); ++i)
        {
            Item* inside = oldBag->GetItemByPos(uint8(i));
            if (!inside)
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, inside, /*swap*/ true) != EQUIP_ERR_OK)
                return false;
        }

        for (uint32 i = 0; i < oldBag->GetBagSize(); ++i)
        {
            Item* inside = oldBag->GetItemByPos(uint8(i));
            if (!inside)
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, inside, true) != EQUIP_ERR_OK)
                continue;

            bot->RemoveItem(worst.Slot, inside->GetSlot(), true);
            bot->StoreItem(dest, inside, true);
        }
    }

    uint16 dest = 0;
    if (bot->CanEquipItem(worst.Slot, dest, bag, /*swap*/ true) != EQUIP_ERR_OK)
        return false;

    bot->RemoveItem(INVENTORY_SLOT_BAG_0, worst.Slot, true);
    bot->EquipItem(dest, bag, true);

    TC_LOG_INFO("scripts.bots", "pbot bags: {} replaced a {}-slot bag with a {}-slot {}",
        bot->GetName(), worst.Capacity, capacity, proto->GetDefaultLocaleName());
    return true;
}

uint32 PbotBags::BuyFromVendor(Player* bot, Creature* vendor)
{
    if (!bot || !vendor || !WantsABag(bot))
        return 0;

    VendorItemData const* items = vendor->GetVendorItems();
    if (!items || items->Empty())
        return 0;

    // Best bag on the shelf that the bot wants and can pay for. Best, not first: a general goods
    // vendor stocks several sizes and the small ones are listed first.
    uint32 bestSlot = 0;
    ItemTemplate const* bestProto = nullptr;
    uint64 bestPrice = 0;

    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* entry = items->GetItem(slot);
        if (!entry || entry->ExtendedCost)
            continue;   // bought with tokens or currency, not coin — not a bot's business

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry->item);
        if (!IsBag(proto))
            continue;

        uint64 const price = proto->GetBuyPrice();
        if (!WouldTake(bot, proto, price))
            continue;

        if (!bestProto || proto->GetContainerSlots() > bestProto->GetContainerSlots())
        {
            bestSlot = slot;
            bestProto = proto;
            bestPrice = price;
        }
    }

    if (!bestProto)
        return 0;

    // The purchase needs somewhere to land before it can be worn, and a bot that wants a bag is by
    // definition short of room. NULL_BAG/NULL_SLOT lets the engine pick.
    if (!bot->BuyItemFromVendorSlot(vendor->GetGUID(), bestSlot, bestProto->GetId(), 1,
                                    NULL_BAG, NULL_SLOT))
        return 0;

    uint32 const capacity = bestProto->GetContainerSlots();
    TC_LOG_INFO("scripts.bots", "pbot bags: {} bought a {}-slot {} from '{}' for {}",
        bot->GetName(), capacity, bestProto->GetDefaultLocaleName(), vendor->GetName(), bestPrice);

    // Wear it now. Left in the bags it is one more thing taking up the room it was meant to add,
    // and the surplus-selling pass would eventually put it back on the market.
    if (Item* bought = bot->GetItemByEntry(bestProto->GetId()))
        Install(bot, bought);

    return capacity;
}
