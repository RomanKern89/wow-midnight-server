/*
 * Companion Bots — level-appropriate gear (TrinityCore master, retail 12.0.7).
 *
 * Player::Create hands out the level-1 starter kit and nothing ever replaces it, so a bot levelled
 * to match its owner fights in cloth scraps with a training sword. Measured consequence: level-20
 * bots died repeatedly to level-20 mobs, which is why the autonomous fair-fight filter had to be
 * clamped to "same level or lower". Gear is the change that lifts that ceiling.
 *
 * Selection is derived from the item store rather than from a hand-written table of item ids: a
 * curated list would be wrong the moment the client data changes, and there is no need for one.
 * For each equipment slot we take the highest-required-level items the bot's level allows and hand
 * them to the engine one at a time — Player::StoreNewItemInBestSlots runs the real CanEquipItem,
 * which already knows about class proficiencies, armor types, unique flags and everything else.
 * The first candidate the engine accepts is the one the bot wears. That means this file never has
 * to model "can a rogue wear mail"; it asks.
 */

#ifndef TRINITYCORE_PBOT_EQUIP_H
#define TRINITYCORE_PBOT_EQUIP_H

#include "Define.h"

class Player;

namespace PbotEquip
{
    // Fills the bot's equipment slots with the best gear its level permits, CONJURED from the item
    // store. This is the starting kit, not a response to anything the bot owns.
    // Safe to call more than once; slots that already hold something are left alone.
    // Returns how many items were successfully equipped.
    uint32 GearUp(Player* bot);

    // Wears what is already in the bags, swapping out anything worse.
    //
    // GearUp cannot do this: it creates new items from templates and never looks in the bags, so a
    // piece the bot bought, was mailed, or crafted stays there forever. Measured consequence — a bot
    // bought a breastplate for 6000, collected it from the mail, and listed it straight back at
    // auction, losing the deposit: to the selling pass an unworn item is simply surplus stock.
    // Returns how many items were put on.
    uint32 EquipFromBags(Player* bot);
}

#endif // TRINITYCORE_PBOT_EQUIP_H
