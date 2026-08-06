/*
 * Companion Bots — questing implementation. See pbot_quest.h for scope and non-goals.
 */

#include "pbot_quest.h"

#include <unordered_set>

#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "LootItemType.h"
#include "MotionMaster.h"
#include "MovementDefines.h" // IDLE_MOTION_TYPE
#include "ObjectAccessor.h"
#include "ObjectDefines.h"   // INTERACTION_DISTANCE
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "StringFormat.h"
#include "pbot_questgoal.h" // is there anywhere on this map to hand this in?
#include "pbot_social.h"   // a quest taken or handed in is when a player actually speaks

#include <cmath>
#include <vector>

namespace
{
    constexpr uint32 QUEST_POINT_ID = 0xC02;      // distinct from the gather/wander MovePoint ids

    // Seconds between questgiver searches. The search walks grid cells, so it is rate limited the
    // same way the gathering and hunting searches are.
    constexpr uint32 SEARCH_INTERVAL_MS = 20000;

    // Shorter cooldown right after a successful interaction: a hub usually has several givers, and
    // a player who just took a quest looks at the next NPC immediately, not twenty seconds later.
    constexpr uint32 AFTER_INTERACT_MS = 4000;

    // Soft cap on the quest log. Below MAX_QUEST_LOG_SIZE on purpose — the last few slots are kept
    // free so a quest the bot CAN finish is never blocked by a pile of ones it cannot.
    constexpr uint32 SOFT_QUEST_CAP = 20;

    // Stop just inside interaction range rather than on top of the NPC.
    constexpr float APPROACH_OFFSET = 3.0f;

    // How long a bot may spend trying to reach a chosen questgiver before writing it off. Generous
    // enough for a genuinely long walk, short enough that an unreachable one costs a minute rather
    // than the rest of the bot's life.
    constexpr uint32 WALK_BUDGET_MS = 60000;

    // Quiet period after giving up, so the search does not hand back the same unreachable NPC on
    // the very next tick.
    constexpr uint32 GIVE_UP_COOLDOWN_MS = 60000;

    bool IsWithinInteractRange(Player const* bot, Creature const* giver)
    {
        return bot->IsWithinDistInMap(giver, INTERACTION_DISTANCE);
    }

    void MoveToGiver(Player* bot, Creature* giver)
    {
        float const angle = giver->GetAbsoluteAngle(bot);
        float x = giver->GetPositionX() + std::cos(angle) * APPROACH_OFFSET;
        float y = giver->GetPositionY() + std::sin(angle) * APPROACH_OFFSET;
        float z = giver->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        bot->GetMotionMaster()->MovePoint(QUEST_POINT_ID, x, y, z);
    }

    // Every quest currently in the bot's log.
    std::vector<uint32> ActiveQuests(Player const* bot)
    {
        std::vector<uint32> ids;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (uint32 questId = bot->GetQuestSlotQuestId(slot))
                ids.push_back(questId);
        return ids;
    }

    uint32 ActiveQuestCount(Player const* bot)
    {
        uint32 count = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (bot->GetQuestSlotQuestId(slot))
                ++count;
        return count;
    }

    // Has the bot made ANY measurable progress on this quest? Used to decide which quest to drop
    // when the log is full: dropping one the bot is actively working on would make it churn
    // forever without ever finishing anything.
    bool HasProgress(Player const* bot, Quest const* quest)
    {
        for (QuestObjective const& obj : quest->GetObjectives())
            if (bot->GetQuestObjectiveData(obj) > 0)
                return true;
        return false;
    }

    // Frees one slot by abandoning a quest with no progress. Returns true if something was dropped.
    // Deliberately conservative: if every held quest shows progress, nothing is abandoned and the
    // bot simply stops taking new ones until it finishes some.
    bool PruneQuestLog(Player* bot)
    {
        for (uint32 questId : ActiveQuests(bot))
        {
            if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || HasProgress(bot, quest))
                continue;

            // Same order the client-driven path uses (WorldSession::HandleQuestLogRemoveQuest):
            // free the source item, drop it out of the log, THEN destroy the quest items. Calling
            // AbandonQuest alone only destroys items and leaves the quest sitting in the log.
            if (!bot->TakeQuestSourceItem(questId, true))
                continue;   // an un-removable equipped source item — same refusal the client gets

            bot->RemoveActiveQuest(questId);
            bot->AbandonQuest(questId);
            return true;
        }

        // Last resort: drop something FINISHED that cannot be handed in anywhere on this map.
        //
        // Without this the whole system deadlocks, and it did. Completed quests are never pruned
        // above — correctly, since throwing away earned rewards is absurd — but a bot that wandered
        // off the continent where the taker stands can never deliver them either. Measured: 264
        // completed quests filled every log to the soft cap, no new quest could be accepted, and the
        // population spent 21% of its day walking to questgivers and coming away with nothing. Not
        // one thing was broken; the loop simply had no exit.
        //
        // A player in that position shrugs and abandons the dead weight. The reward is already lost
        // — the only question is whether the log slot goes with it.
        for (uint32 questId : ActiveQuests(bot))
        {
            if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            if (PbotQuestGoal::HasTakerOnMap(questId, bot->GetMapId()))
                continue;   // deliverable from here; it is waiting, not stuck

            if (!bot->TakeQuestSourceItem(questId, true))
                continue;

            bot->RemoveActiveQuest(questId);
            bot->AbandonQuest(questId);

            TC_LOG_INFO("scripts.bots", "pbot quest: {} let go of finished quest {} — nobody on "
                "this map will take it", bot->GetName(), questId);
            return true;
        }

        return false;
    }

    // Turns in every quest this creature can take back. Returns how many were handed in.
    uint32 TurnInAt(Player* bot, Creature* giver)
    {
        uint32 handed = 0;
        QuestRelationResult const enders = sObjectMgr->GetCreatureQuestInvolvedRelations(giver->GetEntry());

        for (uint32 questId : ActiveQuests(bot))
        {
            if (!enders.HasQuest(questId))
                continue;

            // Complete it ourselves if it is completable but not yet marked complete.
            //
            // CanCompleteQuest is a pure predicate — it reads status, it does not set it (an
            // earlier version of this code called it for its side effect and got none). The engine
            // normally pairs it with CompleteQuest inside UpdateQuestObjectiveProgress, so a quest
            // whose completability changes outside an objective-progress event would otherwise sit
            // at QUEST_STATUS_INCOMPLETE forever and never be handed in.
            if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE && bot->CanCompleteQuest(questId))
                bot->CompleteQuest(questId);

            if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !bot->CanRewardQuest(quest, false))
                continue;

            // Choice rewards: take the first one offered. A real player weighs them; a bot picking
            // deterministically is at worst a slightly odd taste in loot, and picking nothing at
            // all would make the turn-in fail outright.
            LootItemType rewardType = LootItemType::Item;
            uint32 rewardId = 0;
            if (quest->GetRewChoiceItemsCount() > 0)
            {
                rewardType = quest->RewardChoiceItemType[0];
                rewardId   = quest->RewardChoiceItemId[0];
            }

            if (!bot->CanRewardQuest(quest, rewardType, rewardId, false))
                continue;

            bot->RewardQuest(quest, rewardType, rewardId, giver, /*announce*/ false);
            ++handed;
        }
        return handed;
    }

    // Hands finished work to a nearby OBJECT — a chest, an altar, a notice board.
    //
    // 1563 quests in this world are turned in to a game object rather than to a person, and a bot
    // that only ever looks for creatures walks all the way to the right place and stands there. The
    // engine does not care which it is: RewardQuest takes an Object*, and a chest is one.
    uint32 TurnInAtObject(Player* bot)
    {
        // Nothing finished, nothing to deliver — and this runs on the ordinary tick, so the cheap
        // test comes before the grid search.
        bool anyComplete = false;
        for (uint32 questId : ActiveQuests(bot))
            if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
            {
                anyComplete = true;
                break;
            }

        if (!anyComplete)
            return 0;

        std::vector<GameObject*> nearby;
        Trinity::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(),
            bot->GetPositionZ(), INTERACTION_DISTANCE);
        Trinity::GameObjectListSearcher<Trinity::GameObjectInRangeCheck> searcher(bot, nearby, check);
        Cell::VisitAllObjects(bot, searcher, INTERACTION_DISTANCE);

        uint32 handed = 0;
        for (GameObject* go : nearby)
        {
            QuestRelationResult const enders = sObjectMgr->GetGOQuestInvolvedRelations(go->GetEntry());

            for (uint32 questId : ActiveQuests(bot))
            {
                if (!enders.HasQuest(questId) || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                    continue;

                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest || !bot->CanRewardQuest(quest, false))
                    continue;

                LootItemType rewardType = LootItemType::Item;
                uint32 rewardId = 0;
                if (quest->GetRewChoiceItemsCount() > 0)
                {
                    rewardType = quest->RewardChoiceItemType[0];
                    rewardId   = quest->RewardChoiceItemId[0];
                }

                if (!bot->CanRewardQuest(quest, rewardType, rewardId, false))
                    continue;

                bot->RewardQuest(quest, rewardType, rewardId, go, /*announce*/ false);
                ++handed;
            }
        }

        if (handed)
            TC_LOG_INFO("scripts.bots", "pbot quest: {} handed in {} at an object", bot->GetName(),
                handed);

        return handed;
    }

    // Accepts everything this creature offers that the bot is allowed to take. Returns the count.
    uint32 AcceptAt(Player* bot, Creature* giver, std::unordered_set<uint32>& refused)
    {
        uint32 taken = 0;
        for (uint32 questId : sObjectMgr->GetCreatureQuestRelations(giver->GetEntry()))
        {
            if (ActiveQuestCount(bot) >= SOFT_QUEST_CAP && !PruneQuestLog(bot))
                break;

            if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE || refused.count(questId))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // CanTakeQuest covers level, race/class, prerequisites, exclusive groups and the rest —
            // every rule the client-driven path enforces. Re-deriving any of it here would only be
            // a chance to disagree with the engine.
            if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
                continue;

            bot->AddQuestAndCheckCompletion(quest, giver);

            // Count it only if it STUCK. The engine can decline after CanTakeQuest said yes —
            // scenario and timewalking chains do exactly that — and counting the attempt instead of
            // the outcome is how a bot ends up "accepting" the same quest forever.
            //
            // Measured before this: 3498 accept lines against NINE real hand-ins, one bot taking a
            // quest from Chromie 1235 times and another from Orweyna 1012 times. The log called all
            // of it success. Identical mistake to the crafting log, which reported 62 refused casts
            // as 62 crafted items — an attempt and an outcome are not the same event, and only one
            // of them is worth recording.
            if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
            {
                refused.insert(questId);   // never ask for this one again
                continue;
            }

            ++taken;
        }
        return taken;
    }

    // Is this creature worth walking to right now?
    class QuestGiverCheck
    {
    public:
        QuestGiverCheck(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* c) const
        {
            if (!c || !c->IsAlive() || !c->IsQuestGiver())
                return false;

            if (c->IsHostileTo(_bot) || !_bot->IsWithinDist(c, _range))
                return false;

            // Something to hand in?
            QuestRelationResult const enders = sObjectMgr->GetCreatureQuestInvolvedRelations(c->GetEntry());
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = _bot->GetQuestSlotQuestId(slot);
                if (questId && enders.HasQuest(questId))
                    return true;
            }

            // Or something to take? Checked with the engine's own eligibility rules so the bot does
            // not walk across a zone to an NPC that would refuse it.
            for (uint32 questId : sObjectMgr->GetCreatureQuestRelations(c->GetEntry()))
            {
                if (_bot->GetQuestStatus(questId) != QUEST_STATUS_NONE)
                    continue;

                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && _bot->CanTakeQuest(quest, false))
                    return true;
            }

            return false;
        }

    private:
        Player const* _bot;
        float _range;
    };

    Creature* FindGiver(Player* bot)
    {
        Creature* found = nullptr;
        QuestGiverCheck check(bot, PbotQuest::SEARCH_RANGE);
        Trinity::CreatureLastSearcher<QuestGiverCheck> searcher(bot, found, check);
        Cell::VisitAllObjects(bot, searcher, PbotQuest::SEARCH_RANGE);
        return found;
    }
}

bool PbotQuest::Tick(Player* bot, ObjectGuid& giverGuid, uint32& cooldownMs, uint32& walkMs,
                     uint32& speakCooldownMs, std::unordered_set<uint32>& refused, uint32 diff)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat())
    {
        giverGuid.Clear();
        walkMs = 0;
        return false;
    }

    // Keep the log workable, before anything else in this module.
    //
    // This lived inside AcceptAt — that is, it only ever ran when a bot had already reached a
    // questgiver. But a bot with a full log has nothing to say to a questgiver, so it stops going to
    // them, so the pruning never runs: the exit was locked behind the door it was meant to open.
    // Measured, the population sat at 264 completed-undeliverable quests for a whole soak with the
    // release valve compiled in and never once opened.
    //
    // Third time today that necessary work was hidden behind a control path that stopped being
    // taken — first on bot creation after creation stopped happening, then under the decision ladder
    // where the busiest bots never reached it, now behind an NPC visit that a full log prevents.
    // Housekeeping belongs where nothing can gate it.
    if (ActiveQuestCount(bot) >= SOFT_QUEST_CAP)
        PruneQuestLog(bot);

    if (cooldownMs > diff)
        cooldownMs -= diff;
    else
        cooldownMs = 0;

    // Already walking to a giver.
    if (!giverGuid.IsEmpty())
    {
        Creature* giver = ObjectAccessor::GetCreature(*bot, giverGuid);
        if (!giver || !giver->IsAlive())
        {
            giverGuid.Clear();
            walkMs = 0;
            return false;
        }

        // Give up on a giver we cannot reach, and do not immediately pick the same one again.
        walkMs += diff;
        if (walkMs > WALK_BUDGET_MS)
        {
            TC_LOG_INFO("scripts.bots", "pbot quest: {} gave up walking to '{}'",
                bot->GetName(), giver->GetName());
            giverGuid.Clear();
            walkMs = 0;
            cooldownMs = GIVE_UP_COOLDOWN_MS;
            return false;
        }

        if (!IsWithinInteractRange(bot, giver))
        {
            // Re-issue the move if the bot has gone idle short of the target (a failed path leaves
            // it standing still, and without this it would stand there until the giver despawned).
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
                MoveToGiver(bot, giver);
            return true;
        }

        bot->GetMotionMaster()->Clear();

        uint32 const handed = TurnInAt(bot, giver);
        uint32 const taken  = AcceptAt(bot, giver, refused);
        if (handed || taken)
            TC_LOG_INFO("scripts.bots", "pbot quest: {} handed in {} and accepted {} at '{}'",
                bot->GetName(), handed, taken, giver->GetName());

        // Handing something in is the more satisfying of the two, so it gets the line if both
        // happened — and the shared quiet time means only one of them can speak anyway.
        if (handed)
            PbotSocial::OnQuestTurnedIn(bot, speakCooldownMs, std::string());
        else if (taken)
            PbotSocial::OnQuestAccepted(bot, speakCooldownMs, std::string());

        giverGuid.Clear();
        walkMs = 0;
        cooldownMs = AFTER_INTERACT_MS;
        return true;
    }

    if (cooldownMs)
        return false;

    cooldownMs = SEARCH_INTERVAL_MS;

    // Deliver to anything standing right here first. The bot may have walked to an object taker
    // under its own quest goal, in which case there is no creature to look for and the errand is
    // already finished — it just has to reach out and hand the thing over.
    if (TurnInAtObject(bot))
        return true;

    Creature* giver = FindGiver(bot);
    if (!giver)
        return false;

    giverGuid = giver->GetGUID();
    MoveToGiver(bot, giver);
    return true;
}

std::string PbotQuest::Describe(Player* bot)
{
    if (!bot)
        return "no bot";

    uint32 active = 0;
    uint32 complete = 0;
    std::string newest;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        ++active;
        if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
            ++complete;

        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            newest = quest->GetLogTitle();
    }

    if (!active)
        return "quest log empty";

    return Trinity::StringFormat("{} quests ({} complete), last: {}", active, complete, newest);
}
