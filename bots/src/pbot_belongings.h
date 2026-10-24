/*
 * Companion Bots — what a bot actually wants to own (TrinityCore master, retail 12.0.7).
 *
 * Until now "is this worth keeping" was answered by ITEM COLOUR, in three different places that did
 * not agree with each other. The vendor trip sold grey. The clear-a-slot-for-a-reward path threw
 * away grey. The auction listed surplus weapons and armour. Nothing else in a bot's bags was ever
 * classified at all.
 *
 * That is not a small imprecision, it is what stopped the quest pipeline: a bot who had been
 * adventuring for a day had no grey left, so the vendor trip sold NOTHING ("sold 0 junk" in every
 * log line), the bags stayed full, and CanRewardQuest refused every hand-in for want of a free slot
 * — 699 times in one soak. Colour is a proxy for value, and by the time it matters it has stopped
 * being a good one.
 *
 * So the question moves here and gets asked properly, once, for every item: what IS this to this
 * particular bot? A reagent for the trade it practises is precious to a tailor and dead weight to a
 * blacksmith. A bag is never junk. A quest item must never be sold at any price. Rank them, and
 * both the vendor and the emergency clear-a-slot path can simply take from the bottom.
 *
 * Note on scope: the bot deciding for itself does NOT mean the bot may decide to stay full. Making
 * room is not optional — a full bot cannot be paid, cannot loot, and stops progressing. What it
 * decides is only the ORDER things go in, and where the line falls between "sell this" and "I still
 * want this".
 */

#ifndef TRINITYCORE_PBOT_BELONGINGS_H
#define TRINITYCORE_PBOT_BELONGINGS_H

#include "Define.h"

#include <vector>

class Item;
class Player;

namespace PbotBelongings
{
    // What an item is worth to this bot, low to high. Lower goes first when room is needed.
    enum class Worth : uint8
    {
        Never    = 0,   // must not leave the bags: quest items, conjured goods, anything soulbound
                        // that is still useful, the bags themselves
        Junk     = 1,   // grey, broken, or plainly beneath the bot's level — sell without a thought
        Surplus  = 2,   // real gear the bot will never wear: worse than what it has on, or for a
                        // class it is not
        Spare    = 3,   // usable but not needed right now — a second helm of the same quality
        Useful   = 4,   // food, drink, reagents for the trade this bot practises, materials
        Precious = 5    // upgrades it should be wearing, and things it cannot replace
    };

    // Judge one item, for THIS bot. The bot's own trade and equipment decide most of it, which is
    // the point: the same stack of linen is Useful to a tailor and Surplus to a warrior.
    Worth Judge(Player* bot, Item* item);

    // Everything the bot is carrying that may be parted with, worst first. Never includes anything
    // judged Worth::Never, so a caller cannot sell a quest item by accident.
    //
    // `ceiling` is the highest worth to include. Pass Worth::Junk for the ordinary "clear the trash"
    // pass, and higher only when the bot genuinely needs the room — which is the whole difference
    // between tidying up and selling your own boots.
    std::vector<Item*> Disposable(Player* bot, Worth ceiling);

    // Is the item still exactly where it was when Disposable() listed it?
    //
    // MUST be asked immediately before anything destructive. Disposable() hands back raw pointers
    // collected in one pass, and acting on that list mutates the very containers it was read from:
    // selling one item can merge or shift another, and then destroying "the item at bag 3 slot 7"
    // reaches whatever landed there instead. The engine notices at save time and aborts the whole
    // server — Player::_SaveInventory found an item claiming a position it no longer occupied and
    // killed the realm with "deleted but still in world".
    bool StillThere(Player* bot, Item* item);

    // Is this bot short of room? Answers the question every caller was asking by hand, so that the
    // definition of "full" lives in one place.
    bool NeedsRoom(Player* bot);

    // How many of one kind of thing counts as a working supply. Above this it is stock, whatever
    // it is — and that distinction is not academic: an alchemist bot brewed 107 copies of one
    // potion, every one of them protected as "useful", until all four bags were full and it could
    // not be paid a single quest reward. Twenty of anything is more than a bot ever drinks or sews.
    constexpr uint32 KEEP_OF_ONE_KIND = 20;

    // How many free slots the bot is trying to keep. Not comfort: quest rewards need somewhere to
    // land, and a bot at zero free slots walks to a questgiver holding finished work and comes away
    // with nothing.
    constexpr uint32 ROOM_WANTED = 6;
}

#endif // TRINITYCORE_PBOT_BELONGINGS_H
