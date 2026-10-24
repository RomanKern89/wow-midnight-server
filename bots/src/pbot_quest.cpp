/*
 * Companion Bots — questing implementation. See pbot_quest.h for scope and non-goals.
 */

#include "pbot_quest.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
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
#include "pbot_belongings.h" // what this bot considers worth keeping
#include "pbot_social.h"   // a quest taken or handed in is when a player actually speaks

#include <cmath>
#include <vector>

namespace
{

    // Questgivers this bot could not reach, and until when to stop trying. Measured 2026-10-21:
    // 163 give-ups in one window, and they were not spread out - one bot walked at 'Aysa Cloudsinger'
    // 29 times, another at 'Lorewalker Cho' 14, a third at 'Captain Grondel' 10. Each attempt costs a
    // minute of walking plus a minute of cooldown, and the bot never gets anywhere.
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint32>> blockedGivers;
    constexpr uint32 GIVER_BLOCK_MS = 900000;   // fifteen minutes, then the world may have changed
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

    // Stop taking a repeatable quest once the bot holds this many of its reward. Small on purpose:
    // the point of a repeatable quest is the reward, and past a handful the bot is just filling
    // bags it needs for everything else.
    constexpr uint32 REPEAT_REWARD_CAP = 5;

    // ---------------------------------------------------------------------------------------------
    // Repeatable quests: a rate, not a loop.
    //
    // Promoting "deliver what is already finished" above the errands fixed the starvation and
    // immediately exposed this: **one bot handed the same repeatable in 1,638 times in three and a
    // half hours at one NPC** (Farrow at Ogunaro Wolfrunner — 1,638 of the 1,642 hand-ins in the
    // window, from 11 of the 60 bots). Nothing stopped a HAND-IN from looping; REPEAT_REWARD_CAP
    // above only guards ACCEPTING, and only when the reward happens to be an item.
    //
    // A rate rather than a ban: a player does a repeatable now and then, not sixty times a minute,
    // and a bot refused outright would carry the finished copy as ballast in its log forever. Twenty
    // minutes per quest per bot is about "when I feel like it" and cannot loop.
    constexpr uint32 REPEAT_HANDIN_COOLDOWN_MS = 1200000;

    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint32>> _lastRepeatDone;

    bool RepeatableOnCooldown(Player const* bot, Quest const* quest)
    {
        if (!bot || !quest || !quest->IsRepeatable())
            return false;

        auto botItr = _lastRepeatDone.find(bot->GetGUID());
        if (botItr == _lastRepeatDone.end())
            return false;

        auto questItr = botItr->second.find(quest->GetQuestId());
        return questItr != botItr->second.end()
            && GameTime::GetGameTimeMS() - questItr->second < REPEAT_HANDIN_COOLDOWN_MS;
    }

    void NoteRepeatableDone(Player const* bot, Quest const* quest)
    {
        if (bot && quest && quest->IsRepeatable())
            _lastRepeatDone[bot->GetGUID()][quest->GetQuestId()] = GameTime::GetGameTimeMS();
    }

    // How often the quest log is tidied. Housekeeping, not a reflex.
    constexpr uint32 PRUNE_INTERVAL_MS = 60000;

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
        {
            // An objective the engine does not track has no progress to read, and asking anyway is
            // not free: Player::GetQuestObjectiveData logs an error for every call with a negative
            // StorageIndex. Measured over one eight-hour soak, ONE such objective on ONE quest
            // (78984) produced 633,073 error lines and four gigabytes of console log — while the
            // population handed in three quests.
            //
            // The engine's own guard is `StorageIndex < 0`; this is the same test, asked before the
            // call instead of after it.
            if (obj.StorageIndex < 0)
                continue;

            if (bot->GetQuestObjectiveData(obj) > 0)
                return true;
        }

        return false;
    }

    // Throws away finished work that has nowhere to be handed in, anywhere in the world. Unlike the
    // pruning below this is not about making room — it is about not pretending to hold work.
    uint32 DropImpossibleQuests(Player* bot)
    {
        uint32 dropped = 0;

        for (uint32 questId : ActiveQuests(bot))
        {
            if (PbotQuestGoal::HasReachableEnderAnywhere(questId))
                continue;

            if (!bot->TakeQuestSourceItem(questId, true))
                continue;

            bot->RemoveActiveQuest(questId);
            bot->AbandonQuest(questId);
            ++dropped;

            TC_LOG_INFO("scripts.bots", "pbot quest: {} let go of quest {} — its taker exists only "
                "inside a phase, so it can never be handed in", bot->GetName(), questId);
        }

        return dropped;
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

            // "No progress" is also true of a quest taken ten seconds ago, and dropping those is
            // how the bot ate its own tail: at the cap it accepted a quest, pruned it on the next
            // tick because it had not started it yet, walked back and accepted it again. Measured
            // on the live realm — three bots sat at exactly 19 of 20 quests and logged 825 accepts
            // of the same two quests from the same two NPCs, none of which reached the database.
            //
            // Work that can be handed in HERE is work the bot is in a position to do, so it stays.
            // What gets dropped is what it carried in from somewhere else and cannot finish.
            if (PbotQuestGoal::CanFinishOnMap(questId, bot->GetMapId()))
                continue;

            // Same order the client-driven path uses (WorldSession::HandleQuestLogRemoveQuest):
            // free the source item, drop it out of the log, THEN destroy the quest items. Calling
            // AbandonQuest alone only destroys items and leaves the quest sitting in the log.
            if (!bot->TakeQuestSourceItem(questId, true))
                continue;   // an un-removable equipped source item — same refusal the client gets

            bot->RemoveActiveQuest(questId);
            bot->AbandonQuest(questId);

            // This loop used to drop quests in silence, which is why the churn above went unseen
            // for a whole soak. Anything that removes a quest says so.
            TC_LOG_INFO("scripts.bots", "pbot quest: {} dropped unstarted quest {} — nothing to do "
                "for it on this map", bot->GetName(), questId);
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

            if (PbotQuestGoal::CanFinishOnMap(questId, bot->GetMapId()))
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

    // Throws away the least valuable grey item to free one bag slot. Returns false when there is
    // nothing worth sacrificing — a bot full of things it actually wants keeps them and simply does
    // not hand that quest in yet.
    //
    // Deliberately only POOR quality: a player clearing space for a reward bins vendor trash, not
    // their gear or their crafting materials.
    bool MakeRoomForReward(Player* bot)
    {
        // The bot's own judgement decides what goes — see PbotBelongings. All this does is take the
        // least valuable thing it is willing to part with.
        //
        // The rule here used to be "grey only", and that is what stalled the whole quest pipeline:
        // measured over one soak, 699 refused hand-ins, every one of them logged "bags 0 free", and
        // not one of those bots had a grey item left to throw. A bot that has been adventuring all
        // day is not carrying vendor trash — it is carrying white drops it will never wear.
        //
        // Ceiling deliberately at Surplus, not Spare: this is a destructive throw-away with no
        // vendor involved, so it may only reach things the bot has no use for at all. A spare of
        // roughly its own quality is worth a walk to a vendor, not a bin.
        std::vector<Item*> const spare = PbotBelongings::Disposable(bot, PbotBelongings::Worth::Surplus);
        if (spare.empty())
            return false;

        Item* worst = spare.front();   // already sorted worst-and-cheapest first
        if (!PbotBelongings::StillThere(bot, worst))
            return false;

        TC_LOG_INFO("scripts.bots", "pbot quest: {} threw away {} to make room for a reward",
            bot->GetName(), worst->GetTemplate()->GetDefaultLocaleName());

        bot->DestroyItem(worst->GetBagSlot(), worst->GetSlot(), true);
        return true;
    }

    // Turns in every quest this creature can take back. Returns how many were handed in.
    //
    // `blocked` collects, for the diagnostic, every quest this NPC COULD take back but did not, and
    // the reason. Walking to the right person and coming away empty is the failure that has been
    // hardest to see: from outside it is indistinguishable from having nothing to hand in, and the
    // population has spent a fifth of its day doing exactly that.
    uint32 TurnInAt(Player* bot, Creature* giver, std::string* blocked = nullptr)
    {
        uint32 handed = 0;
        QuestRelationResult const enders = sObjectMgr->GetCreatureQuestInvolvedRelations(giver->GetEntry());

        auto note = [blocked](uint32 questId, std::string_view why)
        {
            if (blocked)
                *blocked += Trinity::StringFormat("{}{}:{}", blocked->empty() ? "" : " ", questId, why);
        };

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
                continue;   // simply not finished yet — the ordinary state of a quest in progress,
                            // and NOT a blocked turn-in. Recording it as one buried the real
                            // signal: 5947 of these against 742 genuine refusals.

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
            {
                note(questId, "no-template");
                continue;
            }

            // A repeatable just done is not done again immediately — see RepeatableOnCooldown.
            if (RepeatableOnCooldown(bot, quest))
            {
                note(questId, "repeatable-recently-done");
                continue;
            }

            if (!bot->CanRewardQuest(quest, false))
            {
                note(questId, "cannot-reward");
                continue;
            }

            // Choice rewards: OFFER EACH ONE until the engine accepts it, rather than picking the
            // first and walking away when it says no.
            //
            // Taking index 0 blindly is what the earlier version did, and it cost 935 refused
            // turn-ins in half an hour. The reason was not bag space — measured, most of those bots
            // had plenty of room and not one had grey junk to throw out. A quest's choice list can
            // hold currencies, gaps, or entries this character cannot receive, and CanRewardQuest
            // refuses the whole hand-in over the single one it was handed.
            LootItemType rewardType = LootItemType::Item;
            uint32 rewardId = 0;

            auto tryEveryChoice = [bot, quest, &rewardType, &rewardId]()
            {
                rewardType = LootItemType::Item;
                rewardId = 0;
                if (bot->CanRewardQuest(quest, rewardType, rewardId, false))
                    return true;

                for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
                {
                    if (!quest->RewardChoiceItemId[i])
                        continue;

                    rewardType = quest->RewardChoiceItemType[i];
                    rewardId   = quest->RewardChoiceItemId[i];
                    if (bot->CanRewardQuest(quest, rewardType, rewardId, false))
                        return true;
                }

                return false;
            };

            bool acceptable = tryEveryChoice();

            // Only now is it worth making room: a player clearing space for a reward bins something
            // they do not need, and this asks again afterwards.
            //
            // The retry runs the WHOLE choice list again, not just the last option tried. The last
            // option is simply where the first pass stopped failing — very often the one this
            // character can never receive — so re-asking about that one alone would throw an item
            // away and still refuse the quest.
            if (!acceptable && MakeRoomForReward(bot))
                acceptable = tryEveryChoice();

            if (!acceptable)
            {
                note(questId, Trinity::StringFormat("refused-after-{}-choices",
                    quest->GetRewChoiceItemsCount()));
                continue;
            }

            bot->RewardQuest(quest, rewardType, rewardId, giver, /*announce*/ false);
            NoteRepeatableDone(bot, quest);
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
            // At the cap, stop taking. Do NOT prune to make room: evicting something to accept
            // something else is the trade that produced the loop described in PruneQuestLog.
            // Housekeeping runs at the top of Tick, where it is not competing with an acquisition.
            if (ActiveQuestCount(bot) >= SOFT_QUEST_CAP)
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

            // A repeatable quest is worth doing ONCE in a while, not on a loop.
            //
            // Nothing stops a bot re-taking one the moment it hands it in, and nothing had to: two
            // repeatable quests on this realm ("Could I get a Fishing Flier?") each reward the same
            // item, one bot rode that loop until 107 fliers filled all four of its bags, and it
            // then could not accept any reward from anyone. The engine was doing exactly as asked.
            //
            // A player does a repeatable quest when they want what it gives. A bot that already has
            // a pile of the reward plainly does not.
            if (quest->IsRepeatable() && quest->RewardItemId[0]
                && bot->GetItemCount(quest->RewardItemId[0]) >= REPEAT_REWARD_CAP)
            {
                refused.insert(questId);
                continue;
            }

            // And a repeatable just done is not TAKEN again straight away either: without this the
            // bot re-accepts the moment it hands in, holds a finished copy, and the loop simply
            // resumes one step later.
            if (RepeatableOnCooldown(bot, quest))
                continue;

            // The same question housekeeping asks. It has to be the same one, asked here, or the two
            // fight: this side sees a giver standing in front of it and says yes, that side sees
            // nowhere to hand the thing in and says no, and the bot loops between them forever.
            //
            // Measured before this line existed: 3,803 accept-and-drop pairs from three bots. All of
            // it was legitimate on both sides — quest 14037 really is offered by Miles Standish in
            // the open and really is only taken back by a William Mullins who exists solely inside
            // phase 43472. Nobody was wrong. They were answering different questions.
            //
            // Note this is a MAP test, not a distance test. Far is a job for the flight paths; the
            // wrong continent, or a taker who only exists in a phase, is not far but impossible.
            if (!PbotQuestGoal::CanFinishOnMap(questId, bot->GetMapId()))
            {
                refused.insert(questId);   // and do not come back to ask again on this map
                continue;
            }

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
        QuestGiverCheck(Player const* bot, float range,
            std::unordered_map<uint32, uint32> const* blocked = nullptr)
            : _bot(bot), _range(range), _blocked(blocked) { }

        bool operator()(Creature* c) const
        {
            if (!c || !c->IsAlive() || !c->IsQuestGiver())
                return false;

            if (_blocked)
            {
                auto const itr = _blocked->find(c->GetEntry());
                if (itr != _blocked->end() && itr->second > GameTime::GetGameTimeMS())
                    return false;
            }

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

                // Eligible is not the same as worth the walk. AcceptAt will decline anything it
                // cannot finish on this map, so an NPC offering only such quests has nothing for
                // this bot — and without this line the bot walks over anyway, takes nothing, and
                // comes back tomorrow. That trip is most of what the day was going to.
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && _bot->CanTakeQuest(quest, false)
                    && PbotQuestGoal::CanFinishOnMap(questId, _bot->GetMapId()))
                    return true;
            }

            return false;
        }

    private:
        Player const* _bot;
        float _range;
        std::unordered_map<uint32, uint32> const* _blocked = nullptr;
    };

    // A questgiver that takes back a quest this bot has FINISHED. Narrower than QuestGiverCheck on
    // purpose: that one also accepts a giver of a brand-new quest, so FindGiver used to walk the bot
    // to whichever NPC happened to be nearest - typical line in the log was "handed in 0 and accepted
    // 1 at ...". Measured 2026-10-20: 1489 approaches in fifteen minutes, movement fine (14 yards in
    // two seconds, height delta 1-3 yards), and almost every interaction handed in nothing because it
    // was not the taker. A bot carrying finished work goes to the taker first.
    class CompleteTakerCheck
    {
    public:
        CompleteTakerCheck(Player const* bot, float range,
            std::unordered_map<uint32, uint32> const* blocked = nullptr)
            : _bot(bot), _range(range), _blocked(blocked) { }

        bool operator()(Creature* c) const
        {
            if (!c || !c->IsAlive() || !c->IsQuestGiver() || c->IsHostileTo(_bot)
                || !_bot->IsWithinDist(c, _range))
                return false;

            if (_blocked)
            {
                auto const itr = _blocked->find(c->GetEntry());
                if (itr != _blocked->end() && itr->second > GameTime::GetGameTimeMS())
                    return false;
            }

            QuestRelationResult const enders =
                sObjectMgr->GetCreatureQuestInvolvedRelations(c->GetEntry());
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = _bot->GetQuestSlotQuestId(slot);
                if (questId && _bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE
                    && enders.HasQuest(questId))
                    return true;
            }

            return false;
        }

    private:
        Player const* _bot;
        float _range;
        std::unordered_map<uint32, uint32> const* _blocked = nullptr;
    };

    Creature* FindGiver(Player* bot)
    {
        std::unordered_map<uint32, uint32>& blocked = blockedGivers[bot->GetGUID()];

        // Drop expired entries while we are here, so the map cannot grow without bound.
        uint32 const nowMs = GameTime::GetGameTimeMS();
        for (auto itr = blocked.begin(); itr != blocked.end(); )
        {
            if (itr->second <= nowMs)
                itr = blocked.erase(itr);
            else
                ++itr;
        }

        // The taker of finished work first; anything else only when there is none in reach.
        {
            Creature* taker = nullptr;
            CompleteTakerCheck takerCheck(bot, PbotQuest::SEARCH_RANGE, &blocked);
            Trinity::CreatureLastSearcher<CompleteTakerCheck> takerSearcher(bot, taker, takerCheck);
            Cell::VisitAllObjects(bot, takerSearcher, PbotQuest::SEARCH_RANGE);
            if (taker)
                return taker;
        }

        Creature* found = nullptr;
        QuestGiverCheck check(bot, PbotQuest::SEARCH_RANGE, &blocked);
        Trinity::CreatureLastSearcher<QuestGiverCheck> searcher(bot, found, check);
        Cell::VisitAllObjects(bot, searcher, PbotQuest::SEARCH_RANGE);
        return found;
    }

    // ---------------------------------------------------------------------------------------------
    // Why is a bot carrying finished work doing nothing with it?
    //
    // This module only speaks when a turn-in is ATTEMPTED (reached the taker, engine refused) or when
    // a walk is abandoned. Both are silent for the case that matters: FindGiver never returns the NPC
    // at all — the bot holds a completed quest whose taker is spawned, alive, flagged and standing
    // twenty yards away, and not one line says so. A silent refusal is how the last four defects in
    // this module stayed hidden for a soak apiece, so this probe exists to name the gate.
    //
    // It changes nothing. It counts, once a minute per bot, and only for a bot that is actually
    // carrying finished work.
    struct GiverProbe
    {
        uint32 questGiversInRange = 0;   // alive + flagged + not hostile: the pool FindGiver searches
        uint32 hostileInRange = 0;       // flagged, but hostile to this bot — rejected before the
                                         // relation test, so a taker could hide here
        uint32 enderInRange = 0;         // of the pool: take a quest THIS bot is holding
        uint32 takesComplete = 0;        // ...and take one it has actually FINISHED
        float nearestDist = 0.0f;
        std::string nearestName;         // nearest non-hostile flagged one, identified for the report
        uint32 nearestEntry = 0;         // its creature entry, so the SQL can be compared against it
        uint32 nearestRelations = 0;     // quest relations the ENGINE holds for it at runtime
    };

    class QuestProbe
    {
    public:
        QuestProbe(Player const* bot, GiverProbe& out) : _bot(bot), _out(out) { }

        // Always false: this probe fills the struct, it does not collect creatures.
        bool operator()(Creature* c) const
        {
            if (!c || !c->IsAlive() || !c->IsQuestGiver()
                || !_bot->IsWithinDist(c, PbotQuest::SEARCH_RANGE))
                return false;

            if (c->IsHostileTo(_bot))
            {
                ++_out.hostileInRange;
                return false;
            }

            ++_out.questGiversInRange;

            float const dist = _bot->GetExactDist(c);
            if (_out.nearestName.empty() || dist < _out.nearestDist)
            {
                _out.nearestName = c->GetName();
                _out.nearestDist = dist;
                _out.nearestEntry = c->GetEntry();

                // How many quest relations the ENGINE holds for this NPC at runtime. The SQL has them
                // (creature_questender), the logger reports no dropped rows — so this number is the
                // only way to see whether the runtime table really carries them. Zero here means the
                // relation table is empty in memory, not that the bot looked in the wrong place.
                uint32 n = 0;
                for (auto const& relation : sObjectMgr->GetCreatureQuestInvolvedRelations(c->GetEntry()))
                {
                    (void)relation;
                    ++n;
                }
                _out.nearestRelations = n;
            }

            QuestRelationResult const enders =
                sObjectMgr->GetCreatureQuestInvolvedRelations(c->GetEntry());
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = _bot->GetQuestSlotQuestId(slot);
                if (!questId || !enders.HasQuest(questId))
                    continue;

                ++_out.enderInRange;
                if (_bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                    ++_out.takesComplete;
                break;   // one shared relation is enough to place this NPC in the pool
            }

            return false;
        }

    private:
        Player const* _bot;
        GiverProbe& _out;
    };

    void ReportGiverSearch(Player* bot, bool force = false)
    {
        uint32 complete = 0;
        uint32 goalHasLocalSpot = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            ++complete;
            if (PbotQuestGoal::HasEnderOnMap(questId, bot->GetMapId()))
                ++goalHasLocalSpot;
        }

        if (!complete)
            return;   // nothing finished: this bot has nothing to say about turn-ins

        // Once a minute, not on every search (the search itself runs every twenty seconds).
        static std::unordered_map<ObjectGuid, uint32> lastReport;
        uint32 const now = GameTime::GetGameTimeMS();
        uint32& last = lastReport[bot->GetGUID()];
        if (!force && last && now - last < 60000)
            return;

        last = now;

        GiverProbe probe;
        QuestProbe check(bot, probe);
        std::vector<Creature*> none;   // the probe reports through `probe`, not through a list
        Trinity::CreatureListSearcher<QuestProbe> searcher(bot, none, check);
        Cell::VisitAllObjects(bot, searcher, PbotQuest::SEARCH_RANGE);

        TC_LOG_INFO("scripts.bots", "pbot quest probe: {} map {} — {} finished (the goal module has a "
            "local spot for {} of them); quest givers in range {} (hostile {}), taking a quest I hold "
            "{} and a FINISHED one {}; nearest non-hostile giver '{}' at {:.0f}y (entry {}, engine "
            "relations {})",
            bot->GetName(), bot->GetMapId(), complete, goalHasLocalSpot,
            probe.questGiversInRange, probe.hostileInRange, probe.enderInRange, probe.takesComplete,
            probe.nearestName.empty() ? "none" : probe.nearestName, probe.nearestDist,
            probe.nearestEntry, probe.nearestRelations);

        // Per-quest ender check: for each finished quest, name the NPC that is supposed to take it,
        // and say separately (a) whether a creature with that entry exists within 120 yards of the
        // bot, (b) whether the engine's relation table knows that NPC takes this quest, and (c) what
        // the template says its npc flags are. Those three answer different questions:
        //   (a) no  -> the NPC is not in the world here at all (spawn/phase problem, world data);
        //   (b) no  -> the relation is missing at runtime (load problem);
        //   (c) no  -> the NPC exists but is not flagged as a quest giver.
        // Without this split the last three diagnoses were guesses between them.
        uint32 reported = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && reported < 4; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            auto bounds = sObjectMgr->GetCreatureQuestInvolvedRelationReverseBounds(questId);
            auto row = bounds.begin();
            if (row == bounds.end())
            {
                TC_LOG_INFO("scripts.bots", "pbot ender check: {} quest {} — no creature ender row at "
                    "all (gameobject ender or engine-completed)", bot->GetName(), questId);
                ++reported;
                continue;
            }

            uint32 const enderEntry = row->second;
            Creature* near = bot->FindNearestCreature(enderEntry, PbotQuest::SEARCH_RANGE, true);
            bool const known = sObjectMgr->GetCreatureQuestInvolvedRelations(enderEntry).HasQuest(questId);
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(enderEntry);

            TC_LOG_INFO("scripts.bots", "pbot ender check: {} quest {} — ender {} '{}': npc in "
                "{}y {} (dist {}), engine knows the relation {}, template npcflag {} (questgiver {}), "
                "runtime npcflag {} (in world {})",
                bot->GetName(), questId, enderEntry,
                tmpl ? tmpl->Name : "?", uint32(PbotQuest::SEARCH_RANGE),
                near ? "YES" : "no", near ? bot->GetExactDist(near) : -1.0f,
                known ? "YES" : "no", tmpl ? tmpl->npcflag : 0,
                (tmpl && (tmpl->npcflag & UNIT_NPC_FLAG_QUESTGIVER)) ? "yes" : "no",
                near ? near->GetNpcFlags() : 0,
                near ? (near->IsInWorld() ? "yes" : "no") : "-");

            ++reported;
        }
    }
}

namespace
{
    // One record per bot per approach to a questgiver. It exists to separate two failures that look
    // identical from outside and need different repairs: the order was never issued, or it was issued
    // and the bot did not move. Measured 2026-10-20: bots stopped 24-25 yards from a taker that the
    // probe confirmed was in range and holding a finished quest for them, stood for the whole goal
    // cooldown, and left. Nothing in the log could say which half of that was true.
    struct ApproachTrace
    {
        bool started = false;
        uint32 startedMs = 0;
        float startX = 0.0f;
        float startY = 0.0f;
        uint32 giverEntry = 0;
        bool checked = false;
    };

    std::unordered_map<ObjectGuid, ApproachTrace> approachTraces;

}

bool PbotQuest::Tick(Player* bot, ObjectGuid& giverGuid, uint32& cooldownMs, uint32& walkMs,
                     uint32& speakCooldownMs, std::unordered_set<uint32>& refused,
                     uint32& pruneCooldownMs, uint32 diff)
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
    // Quests that can never be handed in ANYWHERE go regardless of how full the log is.
    //
    // The tidy-up below only runs at the soft cap, on the reasoning that a bot with room has no
    // need to throw anything away. That reasoning fails for work which is not merely inconvenient
    // but impossible: 44 of 277 finished quests on this realm had an ender that exists only inside
    // a phase, so no walk, flight or lift could ever deliver them — and the bots holding them sat
    // below the cap, so nothing ever cleared them. They are not a backlog, they are ballast.

    // Once a minute, not ten times a second.
    //
    // This ran on every tick of every capped bot, and each run walks every objective of every quest
    // it holds. That was tolerable until one of those objectives turned out to be untracked, at
    // which point the engine logged an error per call and the console took 633,073 of them in a
    // single soak. The frequency was always wrong; the untracked objective merely made it visible.
    // A quest log does not become stale between one tenth of a second and the next.
    if (pruneCooldownMs > diff)
        pruneCooldownMs -= diff;
    else
    {
        // Charge the cooldown FIRST, and once, whatever the housekeeping below decides to do.
        // Setting it only inside the at-cap branch would leave a bot under the cap running this
        // every tick — the same mistake, one level down, as the ten-times-a-second prune it
        // replaced.
        pruneCooldownMs = PRUNE_INTERVAL_MS;

        DropImpossibleQuests(bot);

        if (ActiveQuestCount(bot) >= SOFT_QUEST_CAP)
            PruneQuestLog(bot);
    }

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
            approachTraces.erase(bot->GetGUID());
            return false;
        }

        // Give up on a giver we cannot reach, and do not immediately pick the same one again.
        walkMs += diff;
        if (walkMs > WALK_BUDGET_MS)
        {
            TC_LOG_INFO("scripts.bots", "pbot quest: {} gave up walking to '{}' - ignoring that giver "
                "for {} minutes", bot->GetName(), giver->GetName(), GIVER_BLOCK_MS / 60000);
            blockedGivers[bot->GetGUID()][giver->GetEntry()] = GameTime::GetGameTimeMS() + GIVER_BLOCK_MS;
            giverGuid.Clear();
            walkMs = 0;
            approachTraces.erase(bot->GetGUID());
            cooldownMs = GIVE_UP_COOLDOWN_MS;
            return false;
        }

        if (!IsWithinInteractRange(bot, giver))
        {
            // Re-issue the approach whenever the bot is NOT under way, not only when its motion
            // generator reports IDLE, and record whether that worked.
            //
            // Measured 2026-10-20: bots reached a taker (travel stops 25 yards short), the probe said
            // "taking a quest I hold 2 and a FINISHED one 1", and then the bot stood there for the
            // whole 120 s goal cooldown and walked away with the quest still in hand. The motion
            // generator was something other than IDLE (a finished point move, or a generator left by
            // the travel module), so the old type test never fired. IsMoving() covers that case, and
            // only fires when nothing is actually moving the bot.
            ApproachTrace& trace = approachTraces[bot->GetGUID()];
            uint32 const nowMs = GameTime::GetGameTimeMS();

            if (!trace.started || trace.giverEntry != giver->GetEntry())
            {
                trace = ApproachTrace();
                trace.started = true;
                trace.startedMs = nowMs;
                trace.startX = bot->GetPositionX();
                trace.startY = bot->GetPositionY();
                trace.giverEntry = giver->GetEntry();

                TC_LOG_DEBUG("scripts.bots", "pbot approach: {} starts towards '{}' at {:.0f}y, z bot "
                    "{:+.0f} vs npc {:+.0f} (delta {:+.1f}), moving {}, motion type {}",
                    bot->GetName(), giver->GetName(), bot->GetExactDist(giver),
                    bot->GetPositionZ(), giver->GetPositionZ(),
                    bot->GetPositionZ() - giver->GetPositionZ(),
                    bot->isMoving() ? "yes" : "no",
                    uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()));
            }
            else if (!trace.checked && nowMs - trace.startedMs >= 2000)
            {
                float const dx = bot->GetPositionX() - trace.startX;
                float const dy = bot->GetPositionY() - trace.startY;
                trace.checked = true;

                // Routine approaches are silent; only one that made no headway is worth a line.
                // Measured healthy movement at 14 yards in two seconds, so anything under a yard is
                // the failure this trace exists for.
                float const moved = std::sqrt(dx * dx + dy * dy);
                if (moved < 1.0f)
                    TC_LOG_INFO("scripts.bots", "pbot approach: {} did NOT move towards '{}' ({:.0f}y away, "
                        "moving {}, motion type {}), z delta {:+.1f}",
                        bot->GetName(), giver->GetName(), bot->GetExactDist(giver),
                        bot->isMoving() ? "yes" : "no",
                        uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                        bot->GetPositionZ() - giver->GetPositionZ());
            }

            if (!bot->isMoving())
                MoveToGiver(bot, giver);
            return true;
        }
        approachTraces.erase(bot->GetGUID());

        bot->GetMotionMaster()->Clear();

        std::string blocked;
        uint32 const handed = TurnInAt(bot, giver, &blocked);
        uint32 const taken  = AcceptAt(bot, giver, refused);
        if (handed || taken)
            TC_LOG_INFO("scripts.bots", "pbot quest: {} handed in {} and accepted {} at '{}'",
                bot->GetName(), handed, taken, giver->GetName());
        else if (!blocked.empty())
            // Reached the right NPC, holding work it takes, and still nothing moved. This is the
            // case that never had a voice, and the reason codes name it exactly: the objectives are
            // not finished, the reward has nowhere to go, or the engine refused outright.
            TC_LOG_INFO("scripts.bots", "pbot quest: {} reached '{}' with work it takes and handed "
                "in NOTHING — {} (bags {} free)", bot->GetName(), giver->GetName(), blocked,
                bot->GetFreeInventorySlotCount());

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
    {
        ReportGiverSearch(bot);
        return false;
    }

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


// Called from the travel module at the instant a bot reaches a hand-in destination.
//
// The regular probe fires once a minute per bot, wherever the bot happens to be, so in practice it
// almost never runs while the bot is standing at the taker - 341 checks, not one with the NPC inside
// 120 yards. That made "givers in range 0" unreadable: it could mean an empty spot, a wrong spot, or
// a filter refusing a present NPC. Forcing the same probe at the arrival moment answers exactly that.
void PbotQuest::ReportGiverSearchNow(Player* bot)
{
    ReportGiverSearch(bot, /*force*/ true);
}


bool PbotQuest::HasTakerInReach(Player* bot)
{
    if (!bot || !bot->IsAlive() || !bot->IsInWorld())
        return false;

    // 30 yards: the travel module stops this far short of a destination, and the point of the check
    // is to win the tick on the arrival, not to insist the bot is already touching the NPC.
    constexpr float TAKER_IN_REACH_RANGE = 30.0f;

    Creature* found = nullptr;
    CompleteTakerCheck check(bot, TAKER_IN_REACH_RANGE);
    Trinity::CreatureLastSearcher<CompleteTakerCheck> searcher(bot, found, check);
    Cell::VisitAllObjects(bot, searcher, TAKER_IN_REACH_RANGE);
    return found != nullptr;
}
