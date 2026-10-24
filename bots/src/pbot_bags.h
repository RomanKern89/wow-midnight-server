/*
 * Companion Bots — getting bags the way a player gets them (TrinityCore master, retail 12.0.7).
 *
 * Bags were CONJURED. PbotEquip::FillBags creates one out of the item store and puts it straight
 * into an empty slot — which was the right first move (bots that predate bag support owned none,
 * and a bot with a 16-slot backpack cannot be paid a quest reward) but it is not how anyone gets a
 * bag. It also stops the moment all four slots hold something, however small: a bot wearing four
 * six-slot sacks is "done" forever, and it is still short of room.
 *
 * A player has three ways to solve this and a bot should have the same three:
 *
 *   Buy one. General goods vendors sell bags, the bot is already standing at a vendor on its town
 *   trip, and it has money in its purse it currently does nothing with.
 *
 *   Make one, if it is a tailor. That is what the trade is FOR, and a bot that gathers cloth all
 *   day and then buys a bag with coin is a strange kind of tailor.
 *
 *   Buy one from another player. Bots list their surplus at auction already; a tailor's spare bags
 *   belong on that same market, and the bot that needs room should be able to pick one up there.
 *
 * All three end in the same place, so the "should I" and the "where does it go" live here together
 * rather than being re-derived in the vendor, auction and crafting modules.
 */

#ifndef TRINITYCORE_PBOT_BAGS_H
#define TRINITYCORE_PBOT_BAGS_H

#include "Define.h"

class Creature;
class Item;
class ItemTemplate;
class Player;

namespace PbotBags
{
    // Never spend more than this fraction of the purse on a bag. A bot that cannot afford to repair
    // is a bot that dies, and the upkeep work measured that the hard way.
    constexpr float MAX_PURSE_FRACTION = 0.4f;

    // Nothing below this is worth a slot, a walk or a copper: the backpack already holds 16.
    constexpr uint32 MIN_USEFUL_CAPACITY = 6;

    // Does this bot want another (or a bigger) bag? True when a bag slot is empty, or when the
    // smallest bag it wears is small enough to be worth replacing.
    bool WantsABag(Player* bot);

    // The capacity worth buying right now — bigger than the worst bag it wears, or MIN_USEFUL when
    // it has an empty slot. Zero when the bot wants nothing.
    uint32 CapacityWanted(Player* bot);

    // Could this bot use a bag of this size, and can it pay this much for it?
    bool WouldTake(Player* bot, ItemTemplate const* proto, uint64 price);

    // Put a bag the bot now owns into a slot: an empty one, or over the smallest bag it wears if
    // this one is bigger. Returns false when it is not an improvement anywhere.
    //
    // Replacing is NOT a swap of two empty containers — the old bag is usually full. Its contents
    // are moved out first, and if they will not fit anywhere the replacement is abandoned rather
    // than destroying what the bot was carrying.
    bool Install(Player* bot, Item* bag);

    // Buy the best bag this vendor sells that the bot wants and can afford. Returns its capacity,
    // or 0. Called on the town trip the bot is already making.
    uint32 BuyFromVendor(Player* bot, Creature* vendor);
}

#endif // TRINITYCORE_PBOT_BAGS_H
