/*
 * Companion Bots — going to town (TrinityCore master, retail 12.0.7).
 *
 * Bots now fight, loot and quest, and never once deal with the consequences: bags fill with grey
 * trash until nothing more can be picked up, armour wears down until it breaks, and the copper they
 * earn is never spent. A character that never visits a vendor is not playing the game, and worse,
 * a bot with full bags silently stops completing every "collect N of X" quest.
 *
 * The engine already owns the hard parts — CanSellItemToVendor/SellItemToVendor apply the real
 * rules, DurabilityRepairAll charges the real cost — so this module is only about noticing the
 * need, finding somebody who trades, and walking there.
 */

#ifndef TRINITYCORE_PBOT_UPKEEP_H
#define TRINITYCORE_PBOT_UPKEEP_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <string>
#include <vector>

class Player;

namespace PbotUpkeep
{
    // Vendors cluster in settlements, and a bot questing nearby should notice one without crossing
    // the zone for it.
    constexpr float SEARCH_RANGE = 120.0f;

    // Free bag slots at or below which the bot considers a trip worthwhile.
    constexpr uint32 BAGS_FULL_AT = 3;

    // How much room a vendor trip tries to leave the bot with.
    //
    // Not a comfort setting — quest rewards are the reason. CanRewardQuest refuses a hand-in when
    // the prize has nowhere to go, and a bot at zero free slots walks to the right questgiver,
    // holding finished work, and comes away with nothing: measured 699 times in one soak, every one
    // logged "bags 0 free". A few slots is the difference between a quest pipeline that moves and
    // one that does not.
    constexpr uint32 ROOM_TO_LEAVE = 6;

    // Equipment durability fraction below which it is worth paying to repair.
    //
    // Back at 0.5 after measuring 0.8. Sending bots to town at the first sign of wear tripled the
    // attempts (9 trips -> 31) and produced FEWER repairs, not more: zero, against two. Of twenty
    // concluded trips, thirteen ended standing at the destination with nobody visible and seven ran
    // out of time. The limit is arrival, not eagerness — raising this again before trips reliably
    // finish just multiplies the failures, and each one costs a hearth and a walk.
    constexpr float REPAIR_BELOW = 0.5f;

    // Loads where repair-capable NPCs stand, once at boot. Without it a bot in the field has no way
    // to know which direction a repairer is in, and simply never repairs.
    void PreloadRepairSpots();

    // How many recorded positions are currently struck off because a bot went there and found no
    // trader, out of how many are known. The table cleans itself from what the population learns,
    // so this pair is the only way to see that happening.
    void CountSpots(uint32& total, uint32& blocked);

    // Does this bot have a reason to visit a vendor right now? Cheap; safe to call often.
    bool NeedsTown(Player* bot);

    // Is there anyone in reach who would trade with this bot? Walks grid cells — diagnostics only,
    // not the tick path, which already has the vendor it found.
    bool HasVendorInReach(Player* bot);

    // Everything one bot remembers about its errand.
    //
    // townGoal is the reason this is a struct rather than three loose references. A trip to town is
    // interrupted constantly — by a fight, by the bot being pulled into other behaviour — and while
    // the destination was recomputed on every attempt, the "nearest" repairer kept changing as the
    // bot drifted. Measured: a bot with gear at 0% set out for a repairer 1312, then 1368, then
    // 1393 yards away and never arrived. Committing to one destination is what makes a long walk
    // finish.
    // What a bot is walking to town FOR. A trip is committed to one of these, and the errand has to
    // travel with the destination: on arrival the bot needs to know whether it came to meet someone
    // or to stand at a forge, and re-deriving it would answer differently now that it has moved.
    enum class Errand : uint8 { Repair, Auction, Workbench, Tool };

    // How many spawns one bot remembers as hidden from it by phase. Fixed and small: the list only
    // ever holds the handful of destinations a bot actually walked to and could not see.
    constexpr uint32 PHASED_SLOTS = 8;

    struct State
    {
        // A spawn this bot has walked to and found in another phase.
        //
        // Per bot, not per position, because phase is a property of ONE character's quest state:
        // the same NPC stands in the world for a bot that has done the quest and is hidden from one
        // that has not. Kept on the shared row instead, one bot's quest log wrote the NPC off for
        // every bot on the map (measured 2026-10-23).
        struct PhasedSpawn
        {
            uint64 spawnId = 0;
            uint32 msLeft = 0;
        };

        ObjectGuid vendorGuid;      // vendor being approached right now
        uint32 cooldownMs = 0;
        uint32 walkMs = 0;
        Position townGoal;          // committed destination of the current town trip
        uint32 townGoalEntry = 0;   // the NPC that destination was read from (0 for a workbench object)
    uint64 townGoalSpawnId = 0; // its spawn GUID, to ask the map whether the creature exists at all
        uint32 townGoalMap = 0;     // the map it is on; a destination is meaningless off its map
        Errand townGoalErrand = Errand::Repair;   // what that destination is for
        uint32 townGoalFocus = 0;   // and which workbench, when it is one
        uint8 emptyLooks = 0;       // consecutive arrivals at townGoal that found nobody
        uint8 noVendorRuns = 0;     // consecutive trips that ended with nobody to trade with
        bool hasTownGoal = false;

        // Spawns this bot will not set out for again until their timer runs out. See PhasedSpawn.
        PhasedSpawn phasedSpawns[PHASED_SLOTS];
    };

    // Walks to a vendor and does business. Returns true while it owns the tick.
    //
    // Takes the bot's home anchor because the fallback for "no vendor anywhere near" is to hearth,
    // and hearthing moves the anchor with it.
    bool Tick(Player* bot, State& state, Position& home, uint32& homeMapId, uint32 diff);

    // One line explaining why this bot is or is not going to town: the two inputs to NeedsTown, how
    // much grey it is carrying, and whether anyone nearby would trade with it.
    //
    // Without this, a soak that logs no vendor visits is ambiguous between "no bot needed one" —
    // the expected result early on, since a fresh bot has empty bags and unworn armour — and "the
    // module is broken". Those are opposite conclusions, so the inputs have to be readable directly.
    std::string Describe(Player* bot);

    // True when the bot's gear has fallen below the point where it should go and repair it — the same
    // threshold the town errand itself uses (REPAIR_BELOW), not "everything is already destroyed".
    //
    // The distinction cost real deaths: the travel-ladder promotion was gated on GearIsDestroyed, so a
    // bot walked around at 40% armour (below the repair threshold, above zero) and never left the hunt
    // to fix it, until it reached 0%. Measured: 9 of 60 bots fully broken, and the 0% ones produce most
    // of the deaths. Promotion has to follow the threshold, not the catastrophe.
    bool GearNeedsRepair(Player const* bot);
}

#endif // TRINITYCORE_PBOT_UPKEEP_H
