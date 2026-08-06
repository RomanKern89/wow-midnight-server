/*
 * Companion Bots — corpse looting implementation. See pbot_loot.h for why this exists.
 */

#include "pbot_loot.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
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
            if (!_bot->isAllowedToLoot(c))
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

bool PbotLoot::Tick(Player* bot, ObjectGuid& corpseGuid, uint32& cooldownMs, uint32& walkMs, uint32 diff)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat())
    {
        corpseGuid.Clear();
        walkMs = 0;
        return false;
    }

    if (cooldownMs > diff)
        cooldownMs -= diff;
    else
        cooldownMs = 0;

    if (!corpseGuid.IsEmpty())
    {
        Creature* corpse = ObjectAccessor::GetCreature(*bot, corpseGuid);
        if (!corpse || corpse->IsAlive() || !bot->isAllowedToLoot(corpse))
        {
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
        TakeEverything(bot, corpse);

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

    Creature* corpse = FindCorpse(bot);
    if (!corpse)
        return false;

    corpseGuid = corpse->GetGUID();
    MoveToCorpse(bot, corpse);
    return true;
}
