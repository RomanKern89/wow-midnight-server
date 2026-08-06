/*
 * Companion Bots — selling on the auction house (TrinityCore master, retail 12.0.7).
 *
 * Bots gather ore and herbs, loot gear they will never wear, and carry it until their bags jam. The
 * vendor takes the grey trash; everything of actual value has nowhere to go. Meanwhile a bot that
 * cannot earn cannot repair, and an economy with sixty producers and no market is not an economy.
 *
 * So this posts what a bot does not need onto the real auction house, through the engine's own
 * posting path — deposit taken, item removed from the bags, AuctionPosting added to the house — so
 * a player browsing the auction house sees bot goods next to their own and can buy them.
 *
 * A bot lists anything of value it is not using: gathered ore and herbs, cloth and leather, gems,
 * recipes, potions, bags, glyphs, pets and mounts, and looted weapons and armour it will not wear.
 * It refuses only what it must — anything equipped, soulbound, conjured, expiring, a bag with things
 * still inside, an item a live quest of its own needs, and anything with no vendor price to reckon
 * from. Grey trash still goes to the vendor (pbot_upkeep) — the auction house is not a bin.
 */

#ifndef TRINITYCORE_PBOT_AUCTION_H
#define TRINITYCORE_PBOT_AUCTION_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

class Player;

namespace PbotAuction
{
    // City-scale, not shop-scale.
    //
    // Auctioneers exist ONLY in capitals and a handful of large towns — far rarer than repairers,
    // and rarely standing next to one. Measured with the 120y used elsewhere: 49 of 60 bots had
    // goods worth selling and exactly ONE could reach an auctioneer, so the whole feature fired for
    // a single bot. A bot that walked to the blacksmith is already in the right city; this is the
    // range that lets it see the auction house from there.
    //
    // The wide search is affordable because it is asked rarely: SellAtAuction checks the bags first
    // and only walks grid cells when there is actually something to sell.
    constexpr float SEARCH_RANGE = 400.0f;

    // How long a bot's listings run. Long, because nobody is watching the market for them and a
    // short listing that expires simply mails the goods back and wastes the deposit.
    constexpr uint32 LISTING_HOURS = 48;

    // Most a bot will post in one visit. It is standing in a city doing this, and a bot that posts
    // forty things in one tick is a bot that stalls the world thread.
    constexpr uint32 MAX_POSTINGS_PER_VISIT = 5;

    // Does this bot have anything worth taking to market?
    bool HasSellableGoods(Player* bot);

    // Posts up to MAX_POSTINGS_PER_VISIT items if an auctioneer is in reach. Returns how many were
    // actually listed.
    uint32 SellAtAuction(Player* bot);

    // Most a bot will buy in one visit, for the same reason it does not post forty listings at once.
    constexpr uint32 MAX_PURCHASES_PER_VISIT = 2;

    // Never spend the last coin: repairs come out of the same purse, and a bot that shops itself
    // broke cannot fix its armour and starts dying again.
    //
    // Ten silver, not the five gold this started as. That first figure was picked by instinct and
    // was wrong by orders of magnitude in BOTH directions at once: measured repair bills came to
    // NINE COPPER, while the bots doing the selling carried 1,874 to 30,220 copper — so a five-gold
    // floor protected against a cost that does not exist and, in doing so, blocked every purchase
    // any bot ever tried to make. Sixty bots reached an auctioneer and not one could buy.
    //
    // Ten silver still covers a measured repair a hundred times over.
    constexpr uint64 KEEP_IN_RESERVE = 1000;

    // Buys up to MAX_PURCHASES_PER_VISIT listings the bot actually wants, at buyout. Returns how
    // many were bought.
    //
    // Buying is what turns listings into income: nothing a bot posts ever sells unless somebody
    // buys it, and until this existed the whole auction was one-way — sixty sellers, no market.
    uint32 BuyAtAuction(Player* bot);

    // Empties the mailbox: money from sales AND the goods the bot has won, then wears anything that
    // is an upgrade.
    //
    // Both halves are load-bearing. Without the money the profit exists only as unread mail and the
    // bot stays as poor as it was. Without the ITEMS a bot pays for armour and never receives it —
    // the auction house delivers purchases by post, so gear bought and gear worn are separated by a
    // step that has to be taken deliberately.
    uint64 CollectMail(Player* bot);

    // One line for the diagnostic command: what this bot could sell and whether it can reach a market.
    std::string Describe(Player* bot);
}

#endif // TRINITYCORE_PBOT_AUCTION_H
