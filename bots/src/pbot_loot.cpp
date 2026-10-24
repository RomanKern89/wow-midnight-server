/*
 * Companion Bots — corpse looting implementation. See pbot_loot.h for why this exists.
 */

#include "pbot_loot.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Loot.h"
#include "MotionMaster.h"
#include "MovementDefines.h"  // IDLE_MOTION_TYPE
#include "ObjectAccessor.h"
#include "ObjectDefines.h"    // INTERACTION_DISTANCE
#include "Player.h"

#include <cmath>
#include <algorithm>
#include <deque>
#include <unordered_map>

namespace
{
    constexpr uint32 LOOT_POINT_ID = 0xC03;   // distinct from the gather/quest/wander MovePoint ids

    // Corpses last about a minute; searching every few seconds is enough to catch our own kills
    // without walking the grid on every tick.
    constexpr uint32 SEARCH_INTERVAL_MS = 3000;

    // Applied after a corpse that could not be emptied, so a bot with full bags does not spend the
    // corpse's whole lifetime walking back to it.
    constexpr uint32 BLOCKED_LOOT_COOLDOWN_MS = 60000;

    // Longest a bot will spend trying to reach a body before deciding it is not worth it.
    constexpr uint32 WALK_BUDGET_MS = 30000;

    // Slightly inside interaction range, same idea as the gathering approach offset.
    constexpr float APPROACH_OFFSET = 2.0f;

    void MoveToCorpse(Player* bot, Creature* corpse)
    {
        float const angle = corpse->GetAbsoluteAngle(bot);
        float x = corpse->GetPositionX() + std::cos(angle) * APPROACH_OFFSET;
        float y = corpse->GetPositionY() + std::sin(angle) * APPROACH_OFFSET;
        float z = corpse->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        bot->GetMotionMaster()->MovePoint(LOOT_POINT_ID, x, y, z);
    }

    // Is there anything on this corpse that this bot is allowed to take?
    class LootableCorpseCheck
    {
    public:
        LootableCorpseCheck(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* c) const
        {
            if (!c || c->IsAlive() || !c->hasLootRecipient())
                return false;

            if (!_bot->IsWithinDist(c, _range))
                return false;

            // isAllowedToLoot carries the whole tap/group/round-robin rule set. Re-deriving who
            // owns a corpse is exactly the kind of thing that goes subtly wrong, so it is asked
            // rather than reimplemented.
            //
            // But for a client-less bot it answers NO far too often: measured, only ~20% of the bots'
            // own kills were ever looted, and "the bot is fighting" was ruled out as the cause (a
            // dedicated counter found ZERO corpses waiting beside a fighting bot). The tap list is the
            // other half of the same credit — a corpse tapped by this bot or its group is its kill,
            // and on a world of bots there is no player whose loot rights could be stolen.
            if (!_bot->isAllowedToLoot(c) && !c->isTappedBy(_bot))
                return false;

            Loot* loot = c->GetLootForPlayer(_bot);
            return loot && !loot->isLooted();
        }

    private:
        Player const* _bot;
        float _range;
    };

    Creature* FindCorpse(Player* bot)
    {
        Creature* found = nullptr;
        LootableCorpseCheck check(bot, PbotLoot::SEARCH_RANGE);
        Trinity::CreatureLastSearcher<LootableCorpseCheck> searcher(bot, found, check);
        Cell::VisitAllObjects(bot, searcher, PbotLoot::SEARCH_RANGE);
        return found;
    }

    // Corpses this bot earned and has not opened yet, oldest first.
    //
    // This exists because one guid at a time loses bodies: kill, get hit, kill again, and the second
    // corpse overwrote the first guid before the bot ever reached it. Measured, only 15-36% of kills
    // were looted with a single slot.
    std::unordered_map<ObjectGuid, std::deque<ObjectGuid>>& PendingCorpses()
    {
        static std::unordered_map<ObjectGuid, std::deque<ObjectGuid>> pending;
        return pending;
    }

    // A body older than this many kills is not worth walking back for.
    constexpr size_t MAX_PENDING_CORPSES = 8;

    // Empties the corpse: money first, then every item slot. Mirrors what a client does with an
    // open loot window, minus the packets nobody is there to receive.
    void TakeEverything(Player* bot, Creature* corpse)
    {
        Loot* loot = corpse->GetLootForPlayer(bot);
        if (!loot)
            return;

        if (loot->gold)
        {
            bot->ModifyMoney(loot->gold);
            loot->gold = 0;
            loot->NotifyMoneyRemoved(bot->GetMap());
        }

        // Slot index == position in loot->items (LootListId is assigned as items.size() when the
        // item is added), and StoreLootItem only marks a slot looted rather than erasing it, so
        // walking the vector by index is stable in either direction.
        for (uint32 slot = 0; slot < uint32(loot->items.size()); ++slot)
            bot->StoreLootItem(corpse->GetGUID(), uint8(slot), loot);
    }
}

void PbotLoot::NoteCorpse(Player* bot, ObjectGuid corpseGuid)
{
    if (!bot || corpseGuid.IsEmpty())
        return;

    std::deque<ObjectGuid>& queue = PendingCorpses()[bot->GetGUID()];

    if (std::find(queue.begin(), queue.end(), corpseGuid) != queue.end())
        return;

    if (queue.size() >= MAX_PENDING_CORPSES)
        queue.pop_front();

    queue.push_back(corpseGuid);
}

bool PbotLoot::Tick(Player* bot, ObjectGuid& corpseGuid, uint32& cooldownMs, uint32& walkMs, uint32 diff)
{
    if (!bot || !bot->IsAlive())
    {
        corpseGuid.Clear();
        walkMs = 0;
        return false;
    }

    if (bot->IsInCombat())
    {
        // A FIGHT DOES NOT CANCEL THE LOOT — it only postpones it.
        //
        // This branch used to clear corpseGuid, which made the bot abandon its own kill for good the
        // moment anything else attacked it: the corpse is the one thing already earned, the body lasts
        // ~60 s, and the walk budget does not accumulate while fighting, so there is nothing to gain by
        // forgetting it. Measured before this: 42 loots against 118 kills.
        //
        // Counted anyway (once per bot per 30 s), because "waited for the fight to end" and "walked
        // away" are different losses and this tells them apart.
        if (FindCorpse(bot))
        {
            static std::unordered_map<ObjectGuid, uint32> lastReport;
            uint32 const now = GameTime::GetGameTimeMS();
            uint32& last = lastReport[bot->GetGUID()];
            if (!last || now - last >= 30000)
            {
                last = now;
                TC_LOG_INFO("scripts.bots", "pbot loot: {} is in a fight and waiting to loot",
                    bot->GetName());
            }
        }

        return false;
    }

    if (cooldownMs > diff)
        cooldownMs -= diff;
    else
        cooldownMs = 0;

    if (!corpseGuid.IsEmpty())
    {
        Creature* corpse = ObjectAccessor::GetCreature(*bot, corpseGuid);
        if (!corpse || corpse->IsAlive())
        {
            // The body is gone (decayed, respawned, or removed with its grid) — nothing to learn here,
            // the corpse simply has a ~60 s lifetime and the bot was busy.
            corpseGuid.Clear();
            walkMs = 0;
            return false;
        }

        if (!bot->isAllowedToLoot(corpse) && !corpse->isTappedBy(bot))
        {
            // This is the other loss: the body is still there and is still ours in every sense the
            // game shows a player, but the client-less bot is told it may not loot it. Counted
            // separately from decay, because only one of the two is fixable in bot code.
            TC_LOG_INFO("scripts.bots", "pbot loot: {} lost loot rights on entry {} (allowed {}, "
                "tapped {})", bot->GetName(), corpse->GetEntry(),
                bot->isAllowedToLoot(corpse) ? "yes" : "no",
                corpse->isTappedBy(bot) ? "yes" : "no");

            corpseGuid.Clear();
            walkMs = 0;
            return false;
        }

        walkMs += diff;
        if (walkMs > WALK_BUDGET_MS)
        {
            corpseGuid.Clear();
            walkMs = 0;
            cooldownMs = BLOCKED_LOOT_COOLDOWN_MS;
            return false;
        }

        if (!bot->IsWithinDistInMap(corpse, INTERACTION_DISTANCE))
        {
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
                MoveToCorpse(bot, corpse);
            return true;
        }

        bot->GetMotionMaster()->Clear();

        // Looting was never logged, so "do the bots pick corpses up" had no number at all — and it
        // matters for every "collect N of X" quest and for the whole cash flow (vendor trips, repairs,
        // the auction). Measure it at the only place it happens.
        Loot const* before = corpse->GetLootForPlayer(bot);
        uint32 const slotsBefore = before ? uint32(before->items.size()) : 0;
        uint64 const moneyBefore = bot->GetMoney();

        TakeEverything(bot, corpse);

        Loot const* after = corpse->GetLootForPlayer(bot);
        bool const emptied = !after || after->isLooted();

        TC_LOG_INFO("scripts.bots", "pbot loot: {} looted entry {} — {} copper from {} slots, "
            "corpse {}", bot->GetName(), corpse->GetEntry(), bot->GetMoney() - moneyBefore,
            slotsBefore, emptied ? "emptied" : "left something");

        corpseGuid.Clear();
        walkMs = 0;

        // If anything is still on the corpse the bot could not take it — full bags, a unique item
        // it already owns. Backing off hard matters: the corpse stays "lootable", so a short
        // cooldown would make the bot re-select the same body every few seconds until it decays.
        Loot const* remaining = corpse->GetLootForPlayer(bot);
        cooldownMs = (remaining && !remaining->isLooted()) ? BLOCKED_LOOT_COOLDOWN_MS
                                                          : SEARCH_INTERVAL_MS;
        return true;
    }

    if (cooldownMs)
        return false;

    cooldownMs = SEARCH_INTERVAL_MS;

    // Our own kills first — but the NEAREST of them, not the oldest.
    //
    // Oldest-first looked right (bodies decay) and is wrong in practice: a bot that has just killed
    // three mobs around it would walk to the farthest body first, and by the time it got back the near
    // ones had rotted. Picking the closest keeps the walking time per body minimal, which is the only
    // thing that matters inside a 60-second corpse lifetime.
    std::deque<ObjectGuid>& queue = PendingCorpses()[bot->GetGUID()];

    Creature* best = nullptr;
    std::deque<ObjectGuid>::iterator bestItr = queue.end();

    for (auto itr = queue.begin(); itr != queue.end(); ++itr)
    {
        Creature* known = ObjectAccessor::GetCreature(*bot, *itr);
        if (!known || known->IsAlive())
            continue;   // decayed or replaced

        if (!bot->isAllowedToLoot(known) && !known->isTappedBy(bot))
            continue;   // not ours any more

        Loot* knownLoot = known->GetLootForPlayer(bot);
        if (!knownLoot || knownLoot->isLooted())
            continue;   // already emptied

        if (!best || bot->GetDistance(known) < bot->GetDistance(best))
        {
            best = known;
            bestItr = itr;
        }
    }

    if (best)
    {
        corpseGuid = *bestItr;
        queue.erase(bestItr);
        MoveToCorpse(bot, best);
        return true;
    }

    // Nothing usable left in the queue — drop whatever is stale so it does not grow.
    queue.clear();

    Creature* corpse = FindCorpse(bot);
    if (!corpse)
        return false;

    corpseGuid = corpse->GetGUID();
    MoveToCorpse(bot, corpse);
    return true;
}
