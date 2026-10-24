/*
 * Companion Bots — what a bot wants to own. See pbot_belongings.h.
 */

#include "pbot_belongings.h"

#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "SpellMgr.h"

#include <algorithm>

namespace
{
    // Trade skills a bot may practise, paired with the trade-goods subclass whose materials it eats.
    // Used only to answer "is this stack of linen mine or someone else's problem".
    struct TradeMaterial
    {
        uint32 SkillId;
        uint32 TradeGoodsSubclass;
    };

    constexpr TradeMaterial TRADE_MATERIALS[] =
    {
        { SKILL_TAILORING,     ITEM_SUBCLASS_CLOTH        },
        { SKILL_LEATHERWORKING,ITEM_SUBCLASS_LEATHER      },
        { SKILL_BLACKSMITHING, ITEM_SUBCLASS_METAL_STONE  },
        { SKILL_ENGINEERING,   ITEM_SUBCLASS_METAL_STONE  },
        { SKILL_JEWELCRAFTING, ITEM_SUBCLASS_METAL_STONE  },
        { SKILL_JEWELCRAFTING, ITEM_SUBCLASS_JEWELCRAFTING},
        { SKILL_ALCHEMY,       ITEM_SUBCLASS_HERB         },
        { SKILL_INSCRIPTION,   ITEM_SUBCLASS_HERB         },
        { SKILL_INSCRIPTION,   ITEM_SUBCLASS_INSCRIPTION  },
        { SKILL_ENCHANTING,    ITEM_SUBCLASS_ENCHANTING   },
        { SKILL_COOKING,       ITEM_SUBCLASS_MEAT         },
    };

    bool FeedsThisBotsTrade(Player* bot, ItemTemplate const* proto)
    {
        if (proto->GetClass() != ITEM_CLASS_TRADE_GOODS)
            return false;

        for (TradeMaterial const& material : TRADE_MATERIALS)
            if (proto->GetSubClass() == material.TradeGoodsSubclass
                && bot->HasSkill(material.SkillId))
                return true;

        return false;
    }

    // The best item level the bot wears in the slot this item would occupy.
    //
    // Same comparison the auction's IsUpgradeForBot makes, and deliberately so: a bot that judged
    // "is this an upgrade" one way when buying and another way when selling would buy a breastplate
    // and list it straight back — which is exactly what it used to do before the two were aligned.
    uint32 BestWornFor(Player* bot, ItemTemplate const* proto)
    {
        uint8 const invType = proto->GetInventoryType();
        uint32 best = 0;

        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        {
            Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (!worn)
                continue;

            ItemTemplate const* wornProto = worn->GetTemplate();
            if (wornProto && wornProto->GetInventoryType() == invType)
                best = std::max(best, wornProto->GetBaseItemLevel());
        }

        return best;
    }
}

PbotBelongings::Worth PbotBelongings::Judge(Player* bot, Item* item)
{
    if (!bot || !item)
        return Worth::Never;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return Worth::Never;   // unknown is not the same as worthless — leave it alone

    // --- things that must never be sold, at any price, for any amount of room ---------------

    // Quest items. Selling one silently breaks a quest chain the bot is in the middle of, and the
    // bot has no way to notice or recover.
    //
    // Except when it is carrying a pile of them, which is not a quest in progress but a repeatable
    // quest's reward accumulating. Measured, and it is the exact thing that jammed this realm: two
    // repeatable quests ("Could I get a Fishing Flier?", 8228 and 8229) both reward item 19978, one
    // bot handed them in over and over, and 107 fliers filled all four bags. Every one of them was
    // protected as a quest item, so no vendor trip could ever shift them, and the bot then stood in
    // front of a questgiver unable to accept anything at all.
    //
    // No real objective needs a hundred of anything. Keep a working number, treat the rest as the
    // stock it is.
    if (proto->GetClass() == ITEM_CLASS_QUEST || proto->GetBonding() == BIND_QUEST
        || proto->GetStartQuest())
    {
        return bot->GetItemCount(proto->GetId()) > KEEP_OF_ONE_KIND ? Worth::Junk : Worth::Never;
    }

    // Bags. A bot short of room selling a bag is the exact opposite of what it needs, and this
    // walk includes the ones it is wearing.
    if (proto->GetClass() == ITEM_CLASS_CONTAINER || proto->GetClass() == ITEM_CLASS_QUIVER)
        return Worth::Never;

    if (item->IsRefundable() || item->IsNotEmptyBag())
        return Worth::Never;

    // A hoard of something no vendor will ever buy is junk, whatever class the item claims to be.
    //
    // This rule exists because the class-by-class reasoning below was not enough, and I had already
    // guessed wrong once: 101 copies of one item filled a bot's bags for a whole day, I classified
    // it as a quest item from the quest that rewards it, and the sweep that should have binned it
    // found nothing — because whatever the item actually is, it was not the class I assumed.
    //
    // A zero sell price is the engine's own statement that no vendor will take it (see
    // Player::CanSellItemToVendor, which refuses on exactly that). Past a working supply, an item
    // that cannot be sold and is not needed can only ever be dead weight — and dead weight in the
    // bags is what stops a bot being paid for its work.
    if (!proto->GetSellPrice() && bot->GetItemCount(proto->GetId()) > KEEP_OF_ONE_KIND)
        return Worth::Junk;

    // --- everything else is negotiable, in order ----------------------------------------------

    if (proto->GetQuality() == ITEM_QUALITY_POOR)
        return Worth::Junk;

    // Materials for the trade this bot actually practises. To anyone else the same stack is stock.
    // Same ceiling as consumables, for the same reason: a gatherer brings home more cloth than it
    // will ever sew, and "it is my trade's material" must not become a licence to fill every slot.
    if (FeedsThisBotsTrade(bot, proto))
        return bot->GetItemCount(proto->GetId()) > KEEP_OF_ONE_KIND ? Worth::Surplus : Worth::Useful;

    if (proto->GetClass() == ITEM_CLASS_TRADE_GOODS)
        return Worth::Surplus;   // someone else's craft: worth money, not worth carrying

    // Food, drink, bandages, potions. Cheap, small, and the difference between a bot that recovers
    // between fights and one that dies. Conjured goods vanish on logout and are never worth selling.
    //
    // But only up to a point, and that point is the whole lesson here. Rating consumables Useful
    // outright protected them from every selling pass — and then an alchemist bot brewed its own
    // supply until 107 copies of one potion filled all four bags, at which point it could not be
    // paid a quest reward and stood in front of the questgiver logging "bags 0 free". A bot that
    // makes potions all day is not carrying a useful reserve, it is carrying STOCK.
    //
    // So: a working supply is Useful, and the surplus above it is exactly that — surplus, to be
    // sold like any other goods. That also feeds the market rather than the bin.
    if (proto->GetClass() == ITEM_CLASS_CONSUMABLE)
    {
        if (proto->IsConjuredConsumable())
            return Worth::Junk;

        return bot->GetItemCount(proto->GetId()) > KEEP_OF_ONE_KIND ? Worth::Surplus : Worth::Useful;
    }

    if (proto->GetClass() == ITEM_CLASS_RECIPE)
        return Worth::Spare;

    if (proto->GetClass() == ITEM_CLASS_WEAPON || proto->GetClass() == ITEM_CLASS_ARMOR)
    {
        // Cannot ever wear it — wrong class, wrong proficiency, too high a level. Pure stock.
        if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return Worth::Surplus;

        uint32 const offered = proto->GetBaseItemLevel();
        uint32 const worn = BestWornFor(bot, proto);

        if (offered > worn)
            return Worth::Precious;   // it should be wearing this, not selling it

        // Roughly equal to what it has on: a spare, worth keeping while there is room and worth
        // parting with before anything it uses.
        if (worn && offered * 10 >= worn * 9)
            return Worth::Spare;

        return Worth::Surplus;
    }

    // Anything unclassified — keys, tokens, oddities. Cheap to keep, and a bot that sells things it
    // does not understand is a bot that will eventually sell something that mattered.
    return Worth::Spare;
}

std::vector<Item*> PbotBelongings::Disposable(Player* bot, Worth ceiling)
{
    std::vector<Item*> out;
    if (!bot)
        return out;

    std::vector<std::pair<Worth, Item*>> judged;

    bot->ForEachItem(ItemSearchLocation::Inventory, [bot, ceiling, &judged](Item* item)
    {
        // Equipment is a separate search location, so nothing worn is ever a candidate here. What
        // this walk does include is the bot's own equipped BAGS — kept out by Judge, not by luck.
        Worth const worth = Judge(bot, item);
        if (worth != Worth::Never && worth <= ceiling)
            judged.emplace_back(worth, item);

        return ItemSearchCallbackResult::Continue;
    });

    // Worst first, and cheapest first within the same worth — so a bot that only needs one slot
    // gives up the least it possibly can.
    std::sort(judged.begin(), judged.end(), [](auto const& a, auto const& b)
    {
        if (a.first != b.first)
            return a.first < b.first;

        uint32 const av = a.second->GetTemplate()->GetSellPrice() * a.second->GetCount();
        uint32 const bv = b.second->GetTemplate()->GetSellPrice() * b.second->GetCount();
        return av < bv;
    });

    out.reserve(judged.size());
    for (auto const& entry : judged)
        out.push_back(entry.second);

    return out;
}

bool PbotBelongings::StillThere(Player* bot, Item* item)
{
    if (!bot || !item)
        return false;

    // Compare by identity, not by position alone: another item may now sit exactly where this one
    // used to, and destroying THAT is the corruption this guards against.
    return bot->GetItemByPos(item->GetBagSlot(), item->GetSlot()) == item;
}

bool PbotBelongings::NeedsRoom(Player* bot)
{
    return bot && bot->GetFreeInventorySlotCount() < ROOM_WANTED;
}
