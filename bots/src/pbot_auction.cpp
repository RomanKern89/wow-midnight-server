/*
 * Companion Bots — auction house selling implementation. See pbot_auction.h.
 */

#include "pbot_auction.h"

#include "pbot_equip.h"        // wear what just arrived in the post
#include "pbot_profession.h"    // do not sell the raw material this bot smelts and crafts with

#include "AuctionHouseMgr.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "World.h"

#include <set>
#include <unordered_set>
#include <vector>

namespace
{
    // What the auction house is worth over what a vendor pays. A player lists above vendor price or
    // they would simply have vendored it; four times is the rough shape of that instinct without
    // pretending to model a real market, which nothing here could honestly do.
    constexpr uint32 BUYOUT_OVER_VENDOR = 4;

    // Opening bid as a fraction of buyout. Leaving room below the buyout is what makes a listing an
    // auction rather than a shop shelf.
    constexpr uint32 MIN_BID_PERCENT = 70;

    // Cheap early-out before the deposit is worked out — below a silver nothing clears it anyway.
    // The real test is WorthListing, which asks the auction house what it will actually charge.
    constexpr uint64 MIN_WORTH_LISTING = 100;   // one silver

    class AuctioneerCheck
    {
    public:
        AuctioneerCheck(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive() || !_bot->IsWithinDist(creature, _range))
                return false;

            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
                return false;

            // A hostile auctioneer will not deal with this bot, exactly as it would not with a
            // player — the same lesson the repairer table had to learn the expensive way.
            return creature->IsFriendlyTo(_bot);
        }

    private:
        Player const* _bot;
        float _range;
    };

    Creature* FindAuctioneer(Player* bot)
    {
        Creature* found = nullptr;
        AuctioneerCheck check(bot, PbotAuction::SEARCH_RANGE);
        Trinity::CreatureLastSearcher<AuctioneerCheck> searcher(bot, found, check);
        Cell::VisitAllObjects(bot, searcher, PbotAuction::SEARCH_RANGE);
        return found;
    }

    // Would a player list this, or is it either worthless, precious, or not theirs to sell?
    bool IsSellable(Player* bot, Item* item)
    {
        if (!item || item->IsEquipped())
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return false;

        // The engine's own refusals, mirrored from the sell handler: a bound item cannot change
        // hands, a conjured one evaporates, a limited-duration one would rot in the listing.
        if (item->IsSoulBound() || item->IsNotEmptyBag() || item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED)
            || *item->m_itemData->Expiration)
            return false;

        // An item needed by a quest this bot is CURRENTLY on. Most quest items are soulbound and
        // were already refused above — this catches the tradeable ones, which do belong on the
        // auction house, just not while their owner still needs them.
        if (bot->HasQuestForItem(proto->GetId()))
            return false;

        // Grey goes to the vendor (pbot_upkeep). Listing it would pay the deposit for nothing.
        if (item->GetQuality() <= ITEM_QUALITY_POOR)
            return false;

        // No vendor price means no honest way to price it, and a guessed price is worse than none.
        // Ask the engine what the item is actually worth, not what the template field says — see
        // BuyoutFor below for why those two are not the same number.
        if (!item->GetSellPrice(bot))
            return false;

        // Everything else goes to market: gathered ore and herbs, cloth, gems, recipes, potions,
        // bags, glyphs, pets, mounts, weapons and armour it is not wearing.
        //
        // This is a deny-list on purpose. An allow-list of item classes was the first version and
        // it silently skipped whole categories a player would obviously sell — bags, mounts, pets —
        // and would have kept skipping each new category the game adds. The refusals above are the
        // ones that matter: equipped, soulbound, conjured, expiring, a bag with things still in it,
        // needed for a live quest, worthless, or unpriceable.
        return true;
    }

    // Price from what the item is REALLY worth to a vendor, which is Item::GetSellPrice(owner) and
    // not the raw SellPrice field on the template.
    //
    // The first version used the template field and produced a market no bot could shop in: one bot
    // listed a green bow for 344 gold and a pair of bracers for 93 gold, on a server whose richest
    // character had 46. The engine itself never reads that field directly — for anything without
    // ITEM_FLAG2_OVERRIDE_GOLD_COST it derives the price from quality and item level instead, which
    // is why the deposit the auction house charged for that same 344-gold bow came out at the
    // minimum: the engine priced it at under seven silver while we priced it at three hundred gold.
    //
    // Same source as AuctionHouseMgr::GetItemAuctionDeposit, so the listing price and the deposit
    // the bot pays for it can no longer disagree by four orders of magnitude.
    uint64 BuyoutFor(Player const* bot, Item* item)
    {
        return uint64(item->GetSellPrice(bot)) * BUYOUT_OVER_VENDOR * item->GetCount();
    }

    // Is this worth putting on the shelf, or would a player have walked it to the vendor instead?
    //
    // A listing costs a deposit, and the auction house floors that deposit at a silver rounded up
    // and multiplies it by the listing's length — four silver for the 48 hours these run. So the
    // cheap end of the market is a trap: the first bot to reach an auctioneer after the pricing was
    // fixed listed Black Mushroom x10 for 200 copper and paid 400 to do it. It would have to sell
    // at full buyout to lose only half its money.
    //
    // A flat MIN_WORTH_LISTING cannot catch that, because the floor that matters is not a constant —
    // it is whatever the house charges for THIS item over THIS duration, and only the engine knows
    // it. So ask the engine, and compare against the alternative the bot actually has: the vendor,
    // who pays BUYOUT_OVER_VENDOR times less but charges nothing and pays immediately. Listing is
    // only the better errand when it clears the deposit AND beats the vendor price it gives up.
    bool WorthListing(Player* bot, Item* item)
    {
        uint64 const buyout = BuyoutFor(bot, item);
        if (buyout < MIN_WORTH_LISTING)
            return false;

        Minutes const runTime(PbotAuction::LISTING_HOURS * 60);
        uint64 const deposit = AuctionHouseMgr::GetItemAuctionDeposit(bot, item, runTime);
        uint64 const vendorValue = buyout / BUYOUT_OVER_VENDOR;

        return buyout > deposit + vendorValue;
    }

    std::vector<Item*> CollectSellables(Player* bot, uint32 limit)
    {
        // Keep back anything the bot's own trade consumes. Gathering, processing and crafting are
        // one chain: a blacksmith that lists the ore it just mined stands at the anvil afterwards
        // with a hundred recipes and nothing to smelt. Worked out once per call rather than per item
        // — it walks the whole spellbook.
        std::unordered_set<uint32> const reagents = PbotProfession::KnownReagents(bot);

        std::vector<Item*> goods;
        bot->ForEachItem(ItemSearchLocation::Inventory, [bot, &goods, limit, &reagents](Item* item)
        {
            if (goods.size() >= limit)
                return ItemSearchCallbackResult::Stop;

            if (reagents.count(item->GetEntry()))
                return ItemSearchCallbackResult::Continue;   // raw material for its own craft

            if (IsSellable(bot, item) && WorthListing(bot, item))
                goods.push_back(item);

            return ItemSearchCallbackResult::Continue;
        });
        return goods;
    }

    // Posts one item, following the engine's own sequence from WorldSession::HandleAuctionSellItem:
    // take the deposit through PendingAuctionAdd, pull the item out of the bags, then write the
    // listing and the inventory in ONE transaction. Doing these in any other order can leave an item
    // that exists in both places or neither.
    bool PostOne(Player* bot, Creature* auctioneer, Item* item)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
        if (!house)
            return false;

        Minutes const runTime(PbotAuction::LISTING_HOURS * 60);
        uint64 const deposit = AuctionHouseMgr::GetItemAuctionDeposit(bot, item, runTime);
        if (!bot->HasEnoughMoney(deposit))
            return false;   // too poor to list — it will try again once it has sold something

        uint64 const buyout = BuyoutFor(bot, item);
        uint64 const minBid = std::max<uint64>(1, buyout * MIN_BID_PERCENT / 100);

        uint32 const auctionId = sObjectMgr->GenerateAuctionID();

        AuctionPosting auction;
        auction.Id = auctionId;
        auction.Owner = bot->GetGUID();
        auction.OwnerAccount = bot->GetSession()->GetAccountGUID();
        auction.MinBid = minBid;
        auction.BuyoutOrUnitPrice = buyout;
        auction.Deposit = deposit;
        auction.BidAmount = minBid;
        auction.StartTime = GameTime::GetSystemTime();
        auction.EndTime = auction.StartTime + Seconds(int64(std::chrono::duration_cast<Seconds>(runTime).count()
            * double(sWorld->getRate(RATE_AUCTION_TIME))));
        auction.Items.push_back(item);

        if (!sAuctionMgr->PendingAuctionAdd(bot, house->GetAuctionHouseId(), auctionId, deposit))
            return false;

        std::string const itemName = item->GetTemplate()->GetDefaultLocaleName();
        uint32 const count = item->GetCount();

        bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->DeleteFromInventoryDB(trans);
        item->SaveToDB(trans);
        house->AddAuction(trans, std::move(auction));
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        TC_LOG_INFO("scripts.bots", "pbot auction: {} listed {}x{} for {} (deposit {}, purse {})",
            bot->GetName(), itemName, count, buyout, deposit, bot->GetMoney());
        return true;
    }
}

namespace
{
    // Is this a real step up from what the bot is wearing right now?
    //
    // Item level is the engine's own one-number summary of how good a piece is, and PbotEquip's
    // wear-what-you-carry pass compares the same number in the same direction. They have to agree:
    // when they did not, a bot bought a breastplate, could not put it on, and relisted it.
    bool IsUpgradeForBot(Player* bot, ItemTemplate const* proto)
    {
        uint8 const slot = proto->GetInventoryType();
        if (slot == INVTYPE_NON_EQUIP)
            return false;

        uint32 const offered = proto->GetBaseItemLevel();
        if (!offered)
            return false;

        // Compare against every equipped piece that could occupy this kind of slot, and take the
        // best of them — two-slot items (rings, trinkets, one-handers) would otherwise be judged
        // against whichever one happened to be looked at first.
        uint32 best = 0;
        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        {
            Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (!worn)
                continue;

            ItemTemplate const* wornProto = worn->GetTemplate();
            if (!wornProto || wornProto->GetInventoryType() != slot)
                continue;

            best = std::max(best, wornProto->GetBaseItemLevel());
        }

        // An empty slot is the clearest upgrade there is.
        return offered > best;
    }

    // Would this bot want the thing badly enough to spend real money on it?
    //
    // Deliberately narrow. A bot that buys whatever is cheap fills its bags with rubbish, goes
    // broke, and cannot repair — and the measured lesson from the upkeep work is that a broke bot
    // is a dying bot. So: equipment it could actually wear, and nothing else.
    bool WantsToBuy(Player* bot, AuctionPosting const& auction, uint64 reserve)
    {
        if (auction.Owner == bot->GetGUID())
            return false;                       // do not buy your own goods back

        if (!auction.BuyoutOrUnitPrice || auction.Items.empty())
            return false;                       // bid-only listings need patience a bot has not got

        if (auction.BuyoutOrUnitPrice + reserve > bot->GetMoney())
            return false;

        Item* item = auction.Items.front();
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return false;

        if (proto->GetClass() != ITEM_CLASS_WEAPON && proto->GetClass() != ITEM_CLASS_ARMOR)
            return false;

        if (proto->GetBaseRequiredLevel() > bot->GetLevel())
            return false;

        // CanUseItem covers class, race and proficiency — the same question the client asks before
        // it lets a player equip something, so a bot cannot buy plate it will never wear.
        if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return false;

        // And it must be BETTER than what the bot already wears in that slot. Without this a bot
        // spends its earnings on gear no better than its own, which is not shopping, it is hoarding
        // — the bags fill, the purse empties, and nothing about the bot improves.
        return IsUpgradeForBot(bot, proto);
    }

    bool BuyOne(Player* bot, AuctionHouseObject* house, uint32 auctionId)
    {
        AuctionPosting* auction = house->GetAuction(auctionId);
        if (!auction)
            return false;                       // sold or expired since we picked it

        uint64 const price = auction->BuyoutOrUnitPrice;
        if (!bot->HasEnoughMoney(price))
            return false;

        std::string const itemName = auction->Items.empty() || !auction->Items.front()
            ? "something" : auction->Items.front()->GetTemplate()->GetDefaultLocaleName();

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        bot->ModifyMoney(-int64(price));
        auction->Bidder = bot->GetGUID();
        auction->BidAmount = price;

        // Follows the engine's buyout branch exactly: pull the listing out of the house FIRST, then
        // let it mail the goods to the winner and the money to the seller. SendAuctionSold is what
        // pays the seller — skip it and bots would buy from each other while the sellers earned
        // nothing, which would look like a working market and be the opposite of one.
        std::map<uint32, AuctionPosting>::node_type removed = house->RemoveAuction(trans, auction);
        house->SendAuctionSold(&removed.mapped(), nullptr, trans);
        house->SendAuctionWon(&removed.mapped(), bot, trans);

        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        TC_LOG_INFO("scripts.bots", "pbot auction: {} bought {} for {} (purse now {})",
            bot->GetName(), itemName, price, bot->GetMoney());
        return true;
    }
}

namespace
{
    // Slots this bot has already committed to but is not wearing yet: bought moments ago in this
    // same visit, or bought earlier and still sitting in the post.
    //
    // The upgrade test asks what the bot is WEARING, and neither of those is worn — so without this
    // a bot buys a helm, judges the next helm against the OLD one it still has on, and buys that
    // too. Measured: "Kessa bought Steel Plate Helm" twice in one visit, six gold for a spare.
    std::set<uint8> SlotsAlreadyClaimed(Player* bot)
    {
        std::set<uint8> claimed;

        for (Mail* mail : bot->GetMails())
            for (MailItemInfo const& info : mail->items)
                if (Item* item = bot->GetMItem(info.item_guid))
                    if (ItemTemplate const* proto = item->GetTemplate())
                        claimed.insert(uint8(proto->GetInventoryType()));

        return claimed;
    }
}

uint32 PbotAuction::BuyAtAuction(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return 0;

    if (bot->GetMoney() <= KEEP_IN_RESERVE)
        return 0;

    Creature* auctioneer = FindAuctioneer(bot);
    if (!auctioneer)
        return 0;

    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!house)
        return 0;

    // Collect the ids first, then buy. Buying mutates the house's map, and walking a container while
    // erasing from it is the classic way this kind of loop corrupts itself.
    //
    // One slot, one purchase. Claiming the slot as each candidate is chosen — not after it is
    // bought — is what stops a bot picking two helms in the same pass, because nothing it buys here
    // will be worn before this loop ends.
    std::set<uint8> claimed = SlotsAlreadyClaimed(bot);

    std::vector<uint32> wanted;
    for (auto itr = house->GetAuctionsBegin(); itr != house->GetAuctionsEnd(); ++itr)
    {
        if (wanted.size() >= MAX_PURCHASES_PER_VISIT)
            break;

        if (!WantsToBuy(bot, itr->second, KEEP_IN_RESERVE))
            continue;

        uint8 const slot = uint8(itr->second.Items.front()->GetTemplate()->GetInventoryType());
        if (!claimed.insert(slot).second)
            continue;   // already buying something for that slot this trip

        wanted.push_back(itr->first);
    }

    uint32 bought = 0;
    for (uint32 id : wanted)
        if (BuyOne(bot, house, id))
            ++bought;

    return bought;
}

bool PbotAuction::HasSellableGoods(Player* bot)
{
    return bot && bot->IsInWorld() && !CollectSellables(bot, 1).empty();
}

uint32 PbotAuction::SellAtAuction(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return 0;

    // Wear before you sell. Anything in the bags that beats what is on the bot belongs on the bot,
    // and whatever is left over after that is genuine surplus — the definition of what a merchant
    // puts on the counter. Without this pass the bot sells its own upgrades, including ones it just
    // bought or made.
    PbotEquip::EquipFromBags(bot);

    // Bags first, auctioneer second. Checking the inventory is a cheap walk over items the bot
    // already holds; finding an auctioneer walks grid cells out to city range. Doing the cheap test
    // first means the expensive one only runs for a bot that actually has something to sell.
    std::vector<Item*> const goods = CollectSellables(bot, MAX_POSTINGS_PER_VISIT);
    if (goods.empty())
        return 0;

    Creature* auctioneer = FindAuctioneer(bot);
    if (!auctioneer)
        return 0;

    uint32 listed = 0;
    for (Item* item : goods)
        if (PostOne(bot, auctioneer, item))
            ++listed;

    return listed;
}

uint64 PbotAuction::CollectMail(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return 0;

    uint64 taken = 0;
    uint32 itemsTaken = 0;
    time_t const now = GameTime::GetGameTime();

    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED || mail->deliver_time > now)
            continue;

        if (mail->COD)   // cash on delivery must be paid for, not quietly pocketed
            continue;

        // Money: what the bot's own sales earned.
        if (mail->money && bot->ModifyMoney(mail->money, false))
        {
            taken += mail->money;
            mail->money = 0;
            mail->state = MAIL_STATE_CHANGED;
            bot->m_mailsUpdated = true;
        }

        // Attachments: what the bot BOUGHT. Copy the guid list first — taking an item mutates the
        // mail's own vector, and erasing from a container while walking it is how this goes wrong.
        std::vector<ObjectGuid::LowType> attachments;
        for (MailItemInfo const& info : mail->items)
            attachments.push_back(info.item_guid);

        for (ObjectGuid::LowType attachment : attachments)
        {
            Item* item = bot->GetMItem(attachment);
            if (!item)
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                continue;   // bags full — it stays in the mailbox rather than being destroyed

            mail->RemoveItem(attachment);
            mail->removedItems.push_back(attachment);
            mail->state = MAIL_STATE_CHANGED;
            bot->m_mailsUpdated = true;
            bot->RemoveMItem(attachment);

            item->SetState(ITEM_UNCHANGED);   // or it cannot be removed again later
            bot->MoveItemToInventory(dest, item, true);
            ++itemsTaken;
        }
    }

    if (taken || itemsTaken)
    {
        // A FULL save, not just inventory and gold.
        //
        // The mail rows have to go down in the same breath as the bags. Writing only the inventory
        // would leave the mail table still claiming an attachment the bot is now carrying, and a
        // restart would hand it the same item twice. Player::_SaveMail is protected, so the honest
        // way to get both written is the ordinary character save the engine already does on a timer.
        bot->SaveToDB(false);

        TC_LOG_INFO("scripts.bots", "pbot auction: {} emptied the mailbox — {} copper and {} items "
            "(purse {})", bot->GetName(), taken, itemsTaken, bot->GetMoney());
    }

    // Wear what just arrived. Buying armour and leaving it in the bags is the same as not buying it
    // — worse, actually: the next selling pass sees an unworn item as surplus and lists it straight
    // back, so the bot pays a deposit to give away what it just paid for.
    if (itemsTaken)
        PbotEquip::EquipFromBags(bot);

    return taken;
}

std::string PbotAuction::Describe(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return "not in world";

    std::vector<Item*> const goods = CollectSellables(bot, MAX_POSTINGS_PER_VISIT);
    uint64 worth = 0;
    for (Item* item : goods)
        worth += BuyoutFor(bot, item);

    Creature* auctioneer = FindAuctioneer(bot);

    return Trinity::StringFormat("{} sellable ({} copper), {} mails, auctioneer {}",
        uint32(goods.size()), worth, bot->GetMailSize(),
        auctioneer ? Trinity::StringFormat("'{}' at {:.0f}y", auctioneer->GetName(),
            bot->GetDistance(auctioneer)) : "none in reach");
}
