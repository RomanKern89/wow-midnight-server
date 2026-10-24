/*
 * Companion Bots — PbotAI implementation (fake-player bots, TrinityCore master, retail 12.0.7).
 * See DESIGN_PHASE3 SS3 (update loop), SS8.3 (base interface), SS8.6 (forbidden list) and
 * SS9 PART B (combat rotations + positioning). Every engine call below is verified against
 * C:\WOW\tc-src.
 *
 * Ticking model (SS3): a bot's Player* is a normal WorldObject in a Map, so Map::Update ticks
 * Player::Update for it every world frame "for free" — including the melee auto-attack driver
 * DoMeleeAttackIfReady (Player.cpp:1019). We therefore do NOT hand-roll swings here; once
 * Unit::Attack(target, true) has set UNIT_STATE_MELEE_ATTACKING (Unit.cpp:5925), the engine
 * swings on its own timer. Phase 3.2 layers class spell rotations on TOP of that swing: each tick
 * we decide follow-vs-assist, set the victim, and run the shared rotation (bot_rotations.h). The
 * rotation issues casts through ctx.TryCast (our DoCastChecked — the engine's real SpellHistory
 * does all GCD/cooldown bookkeeping, B.1) and repositioning through ctx.Reposition (our class-gated,
 * self-throttled MoveChase, B.4).
 */

#include "pbot_ai.h"
#include "pbot_mgr.h"          // PbotMgr::GetBotsOf (heal-target candidate set, B.4)
#include "pbot_autonomy.h"     // world-bot behaviour primitives (Phase 6)
#include "pbot_gather.h"       // herb/ore harvesting (Phase 5)
#include "pbot_pvp.h"          // duel acceptance + hostile-player selection (Phase 7)
#include "pbot_bg.h"           // battleground queue + entry (Phase 7)
#include "pbot_quest.h"        // take/turn in quests from world questgivers
#include "pbot_loot.h"         // loot our own kills (money, drops, quest items)
#include "pbot_migrate.h"
#include "pbot_delivery.h"  // PbotDelivery::Tick — fetch rewards left on another map      // move to a new zone once this one is outgrown
#include "Item.h"
#include "ItemTemplate.h"
#include "pbot_world_spots.h" // vetted ground to move a repeatedly stuck bot to
#include "pbot_belongings.h" // what this bot considers worth keeping
#include "pbot_travel.h"       // stepped long-distance movement + roam points
#include "pbot_flight.h"       // flight paths — discovering them and using them
#include "pbot_questgoal.h"    // where an unfinished quest is actually done
#include "pbot_intent.h"       // what this bot is doing for the next few minutes
#include "pbot_hearth.h"       // go home to your own people when a zone runs dry
#include "pbot_social.h"       // bots that say things — silence is the loudest tell
#include "pbot_group.h"        // bots that party up instead of ignoring each other
#include "pbot_mount.h"        // ride the long stretches instead of jogging cross-country
#include "pbot_personality.h"  // who this particular bot is — no two run the same numbers
#include "pbot_upkeep.h"       // sell the junk and repair the armour, like a player does
#include "pbot_auction.h"      // take the good stuff to market, and read the mail it pays into
#include "pbot_profession.h"   // make things out of what it gathers, and sell those too
#include "bot_rotations.h"     // BotCombatContext + Run*Rotation free functions (B.2/B.3)
#include "Player.h"
#include "Unit.h"
#include "Creature.h"          // the kill tally asks the corpse whether we may loot it
#include "Group.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "ThreatManager.h"     // GetThreatManager().AddThreat / GetThreatListSize
#include "SpellHistory.h"      // GetSpellHistory()->HasCooldown / HasGlobalCooldown (B.1)
#include "SpellMgr.h"          // sSpellMgr
#include "SpellInfo.h"         // SpellInfo (passed by pointer into SpellHistory)
#include "DBCEnums.h"          // DIFFICULTY_NONE
#include "SharedDefines.h"     // CLASS_*, SPELL_CAST_OK

#include "StringFormat.h"      // the activity budget report
#include "GameTime.h"          // GetGameTimeMS — the throttles below

#include <algorithm>           // sorting that report by share
#include <array>
#include <atomic>              // maps tick in parallel threads; the tally is written from all of them
#include <sstream>             // DescribeState (Phase 4A status report)
#include <unordered_map>       // the travel probes below
#include <utility>
#include <vector>

namespace
{
    // ---------------------------------------------------------------------------------------------
    // Travel probes (2026-10-15). The turn-in goal is chosen inside TickTravel, and every path that
    // declines to choose it is silent: a bot can carry finished work with its taker standing a few
    // hundred yards away, on its own map, for a whole run — and not one line says whether the choice
    // was ever offered, or whether it was a goal already in flight that blocked it. Naming the gate
    // is the whole point; these lines change nothing. One a minute per bot, and only for a bot that
    // is actually carrying finished work.
    bool ShouldReportTravel(Player* bot)
    {
        uint32 complete = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (questId && bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                ++complete;
        }

        if (!complete)
            return false;

        static std::unordered_map<ObjectGuid, uint32> lastReport;
        uint32 const now = GameTime::GetGameTimeMS();
        uint32& last = lastReport[bot->GetGUID()];
        if (last && now - last < 60000)
            return false;

        last = now;
        return true;
    }

    // How often a bot bothers checking for things nobody will buy. Cheap, but not free — it walks
    // every item — and a pile of unsellable junk does not appear between one second and the next.
    constexpr uint32 BIN_INTERVAL_MS = 60000;

    // How far a bot must move to count as having moved at all. Generous: a bot fighting in place or
    // casting still shuffles a yard or two, and that is not stuck.
    constexpr float STILL_TOLERANCE = 8.0f;

    // How long it may stand perfectly still, out of combat, before being treated as stuck. Long
    // enough that resting, waiting at a gathering node and standing at a vendor all pass.
    constexpr uint32 STUCK_AFTER_MS = 300000;   // 5 minutes

    // How many rescues in a row before the bot is moved somewhere else entirely.
    constexpr uint8 STUCK_STREAK_BEFORE_REHOME = 2;

    // A quest walk that still has this share of its distance left has not really moved. Three
    // minutes at walking pace covers well over a thousand yards; keeping 80% of a 300-yard trip
    // means the ground itself is refusing to be crossed.
    constexpr float KEPT_DISTANCE_MEANS_STUCK = 0.8f;

    // How much NEARER a taker has to be before a bot re-aims mid-journey (see TickTravel), and how
    // often it may. The margin is the fix for the first version of that code: without it the bot
    // turned aside for a destination the SAME distance away, every tick, 129,196 times in one
    // window — cancelling its own motion each time, so it never actually went anywhere.
    constexpr float  TURN_ASIDE_MARGIN = 50.0f;
    constexpr uint32 TURN_ASIDE_COOLDOWN_MS = 60000;

    // Throw away what the bot has judged worthless AND no vendor will accept.
    //
    // Deliberately not part of the vendor trip, though the vendor does it too. The case this exists
    // for is precisely the one where the vendor trip never completes: quest items sell for zero, so
    // CanSellItemToVendor refuses them at any price, and a bot whose bags are full of them needs
    // town forever and is never able to fix it by going. Measured, that circuit ate a whole soak.
    void BinUnsellable(Player* bot, uint32& cooldownMs, uint32 diff)
    {
        if (cooldownMs > diff)
        {
            cooldownMs -= diff;
            return;
        }

        if (!PbotBelongings::NeedsRoom(bot))
            return;

        // Only charged when the walk actually happens: a bot with room pays nothing for this.
        cooldownMs = BIN_INTERVAL_MS;

        for (Item* item : PbotBelongings::Disposable(bot, PbotBelongings::Worth::Junk))
        {
            // Only what a vendor would refuse. Anything sellable is worth the walk — binning it
            // would be throwing away the bot's own income.
            // Destroying moves things; anything already shifted since the list was built is not
            // ours to touch. Skipping this is what aborted the whole server once, in
            // Player::_SaveInventory, with "deleted but still in world".
            if (!PbotBelongings::StillThere(bot, item))
                continue;

            if (!bot->CanSellItemToVendor(item, item->GetCount()))
                continue;

            // Re-judged as we go: destroying drops the count, and once the pile is down to a
            // working supply the rest stops being junk and this stops.
            if (PbotBelongings::Judge(bot, item) != PbotBelongings::Worth::Junk)
                continue;

            TC_LOG_INFO("scripts.bots", "pbot upkeep: {} binned {} — no vendor will take it",
                bot->GetName(), item->GetTemplate()->GetDefaultLocaleName());
            bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        }
    }

    // Follow spacing. The SS8.3 interface carries no slot index (only the bot guid), so up-to-a-
    // handful of one owner's bots fan out deterministically by guid counter instead of stacking
    // on a single point. Stable across ticks, no per-tick state. Angles avoid 0/PI (dead behind /
    // in front) exactly as Phase 1 BOT_FOLLOW_ANGLES does.
    constexpr float  PBOT_FOLLOW_ANGLES[4] = { 2.0f, 4.0f, 1.0f, 5.0f };
    constexpr float  PBOT_FOLLOW_DISTANCE  = 2.5f;    // melee formation distance behind the owner

    // Same-map leash: past this many yards from the owner, snap back next to them (SS8.3).
    constexpr float  PBOT_LEASH_DISTANCE   = 60.0f;

    // Reposition throttle (B.4). MoveChase installs a fresh generator per call, so the rotation
    // calling ctx.Reposition() every tick would thrash it; we only re-issue on a target change or
    // after this interval. Matches the creature kits' REPOSITION_INTERVAL_MS.
    constexpr uint32 PBOT_REPOSITION_INTERVAL_MS = 2000;

    // Ranged band (B.4): mage/hunter/priest hold 20-30y and never walk into melee.
    constexpr float  PBOT_RANGED_CHASE_MIN = 20.0f;
    constexpr float  PBOT_RANGED_CHASE_MAX = 30.0f;

    // AoE-mode signal (B.2): >= this many living enemies attacking the bot within the radius.
    constexpr float  PBOT_AOE_RANGE        = 10.0f;
    constexpr uint32 PBOT_AOE_MIN_ENEMIES  = 3;

    // Healer candidate scan radius (B.4), matches the creature priest kit's HEAL_RANGE.
    constexpr float  PBOT_HEAL_RANGE       = 40.0f;

    // How often an idle world bot looks around for something to fight (Phase 6/7). Grid searches
    // are the one genuinely expensive thing in the autonomous loop, and this is what keeps a
    // populated zone affordable.
    constexpr uint32 PBOT_HUNT_INTERVAL_MS = 1000;

    // How often a bot checks that it still owns its trade and knows what its skill allows. Slow:
    // this is bookkeeping, not behaviour, and it walks the whole spell book.
    constexpr uint32 PROFESSION_SETUP_INTERVAL_MS = 60000;

    float FollowAngleFor(ObjectGuid guid)
    {
        return PBOT_FOLLOW_ANGLES[guid.GetCounter() % 4];
    }

    // Gates positioning (B.4): ranged classes hold the 20-30y band, melee close to swing range.
    // Druid/Shaman/Evoker are listed as ranged because their shipped ladders are the caster ones
    // (Wrath / Lightning Bolt / Living Flame) — if a melee spec ladder is ever added, this is the
    // one place that has to learn about it.
    bool ClassIsRanged(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_MAGE:
            case CLASS_HUNTER:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
            case CLASS_DRUID:
            case CLASS_SHAMAN:
            case CLASS_EVOKER:
                return true;
            default:
                return false;
        }
    }

    // Classes whose ladder has a heal step, i.e. the ones for which the caller must select a
    // ctx.HealTarget. Picking a heal target is a 40y ally scan, so it is skipped for everyone else.
    bool ClassHeals(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_PRIEST:
            case CLASS_PALADIN:
            case CLASS_DRUID:
            case CLASS_SHAMAN:
            case CLASS_MONK:
            case CLASS_EVOKER:
                return true;
            default:
                return false;
        }
    }
}

Player* PbotAI::ResolveBot() const
{
    return ObjectAccessor::FindPlayer(_botGuid);
}

Player* PbotAI::ResolveOwner() const
{
    return ObjectAccessor::FindPlayer(_ownerGuid);
}

void PbotAI::UpdateBot(uint32 diff)
{
    // Re-resolve fresh every tick (SS8.3) — never cache a Player* across ticks.
    Player* bot = ResolveBot();
    if (!bot || !bot->IsInWorld())
        return;

    // Phase 7: answer a duel challenge one tick after it arrived. Applies to companion and world
    // bots alike — being challengeable is part of being a real character.
    if (_duelPending)
    {
        _duelPending = false;
        PbotPvP::AcceptPendingDuel(bot);
    }

    // Phase 6: an ownerless bot runs its own life instead of the companion logic below, and does
    // it BEFORE the alive check because handling its own death is part of that life.
    if (IsWorldBot())
    {
        UpdateAutonomous(bot, diff);
        return;
    }

    // Companion path. If the owner is gone this frame, do nothing and return — PbotMgr owns
    // removing our entry from the driver list on DismissBot; this class never self-terminates.
    Player* owner = ResolveOwner();
    if (!owner || !owner->IsInWorld())
        return;

    // Dead bot: nothing to drive while it's a corpse (Unit::Attack/MotionMaster/CastSpell on a dead
    // unit are rejected or meaningless). Resurrection or dismissal is the owner's / PbotMgr's job.
    if (!bot->IsAlive())
        return;

    // MVP is same-map only. If the owner has crossed to a different map (portal/teleport), we can't
    // safely NearTeleport across maps from here - that needs the full load path we deliberately
    // avoid (SS2). Leave the bot put; cross-map recovery belongs to PbotMgr's lifecycle. Distance
    // math across maps is meaningless, so guarding here is also correctness, not just scope.
    if (bot->GetMap() != owner->GetMap())
        return;

    // Phase 4A: stance decides IF we fight and WHAT we pick; Phase 3.2's unconditional
    // "owner in combat -> assist" is now one case of SelectCombatTarget. A null target means
    // "not fighting this tick" and falls through to the movement tick, which itself honours the
    // "стой" hold-position mode.
    if (Unit* target = SelectCombatTarget(bot, owner))
    {
        TickCombat(bot, owner, target, diff);
    }
    else
    {
        // The "в бой" override is a one-shot: once there is nothing left to fight it expires, so a
        // bot ordered to attack does not silently stay aggressive for the rest of the session.
        _forcedAttack = false;
        TickFollow(bot, owner, diff);
    }
}

namespace
{
    // Ticks claimed by each stage, across every bot. Atomic because maps update in parallel threads
    // and this is written from all of them; relaxed because a tally has no ordering to protect.
    std::array<std::atomic<uint64>, size_t(PbotAI::Activity::Count)> _activityTicks{};

    char const* ActivityName(PbotAI::Activity activity)
    {
        switch (activity)
        {
            case PbotAI::Activity::Settling:     return "settling";
            case PbotAI::Activity::Dead:         return "dead";
            case PbotAI::Activity::Battleground: return "battleground";
            case PbotAI::Activity::Retreat:      return "retreating";
            case PbotAI::Activity::Combat:       return "fighting";
            case PbotAI::Activity::Resting:      return "resting";
            case PbotAI::Activity::Looting:      return "looting";
            case PbotAI::Activity::Hunting:      return "hunting";
            case PbotAI::Activity::Gathering:    return "gathering";
            case PbotAI::Activity::Upkeep:       return "town errand";
            case PbotAI::Activity::Market:       return "auction house";
            case PbotAI::Activity::Workbench:    return "walking to a forge";
            case PbotAI::Activity::Crafting:     return "crafting";
            case PbotAI::Activity::Questing:     return "questing";
            case PbotAI::Activity::Travelling:   return "travelling";
            case PbotAI::Activity::Roaming:      return "roaming";
            default:                             return "idle";
        }
    }

    // Records what claimed the tick, however the function returned. Every stage in the ladder below
    // sets _activity immediately before its return; anything that falls through stays "idle".
    struct ActivityScope
    {
        PbotAI::Activity const& Slot;
        ~ActivityScope() { _activityTicks[size_t(Slot)].fetch_add(1, std::memory_order_relaxed); }
    };
}

void PbotAI::ResetActivityBudget()
{
    for (std::atomic<uint64>& counter : _activityTicks)
        counter.store(0, std::memory_order_relaxed);
}

std::string PbotAI::DescribeActivityBudget()
{
    uint64 total = 0;
    for (std::atomic<uint64> const& counter : _activityTicks)
        total += counter.load(std::memory_order_relaxed);

    if (!total)
        return "no ticks counted yet";

    // Largest share first: the answer to "where does the day go" is the top of this list, and
    // sorting it saves reading seventeen numbers to find the one that matters.
    std::vector<std::pair<uint64, Activity>> rows;
    for (size_t i = 0; i < size_t(Activity::Count); ++i)
        if (uint64 const ticks = _activityTicks[i].load(std::memory_order_relaxed))
            rows.emplace_back(ticks, Activity(i));

    std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

    std::string out = Trinity::StringFormat("{} ticks counted:", total);
    for (auto const& [ticks, activity] : rows)
        out += Trinity::StringFormat("\n  {:>5.1f}%  {}", 100.0 * double(ticks) / double(total),
            ActivityName(activity));

    return out;
}

void PbotAI::UpdateAutonomous(Player* bot, uint32 diff)
{
    _activity = Activity::Idle;
    ActivityScope const accounting{ _activity };

    // In the air. Above everything, including the map-change reset below — a flight between
    // continents changes the map mid-journey, and clearing the motion master there would drop the
    // bot out of the sky. The engine flies it; this tick is not ours to spend.
    //
    // Nothing in the ladder is safe against a taxi spline: MovePoint replaces it, MotionMaster::Clear
    // deletes it, and either leaves the bot standing wherever the flight had got to with UNIT_STATE
    // still saying it is flying.
    if (bot->IsInFlight())
    {
        _activity = Activity::Travelling;
        _lastMapId = bot->GetMapId();   // so landing on another continent is not read as a teleport
        return;
    }

    // Flight points are learned by going near them, which is how retail has worked since the
    // discovery toast replaced talking to the flight master. Bots already walk and migrate; this is
    // what turns that into a map of the world they can actually use.
    PbotFlight::Discover(bot);

    // Throw out what nobody will ever buy — ABOVE the ladder, not inside it.
    //
    // It costs no walk, no NPC and almost no time, and its position is the whole point. Placed down
    // beside the town errands it was never reached: a bot whose bags are full is also a bot that
    // keeps trying to gather, and gather claims the tick first. Measured, Perrin spent its day on
    // "saw 4 chests within 80y and could open none" while carrying 101 copies of one worthless item.
    //
    // That is the FIFTH time in this codebase that necessary work was hidden behind a control path
    // which stopped being taken — after bot creation, after the decision ladder's busiest branch,
    // after an NPC visit a full log prevented, and after a vendor trip that could never complete.
    // Housekeeping belongs where nothing can gate it.
    BinUnsellable(bot, _binCooldownMs, diff);

    // Stuck watchdog. Also above the ladder, and for the same reason: a bot that cannot move is a
    // bot that cannot reach any of the things below.
    //
    // A position the pathfinder refuses to leave — under the terrain, inside geometry, at a z of
    // zero — is invisible to every module here. Each one issues its move, the engine rejects the
    // spline as too long, and the module tries again on the next tick, forever. Measured: one
    // level-18 bot sitting at z = 0.0000000034 on map 530 produced 38,264 rejected splines, 83% of
    // every movement failure on the realm, and of course handed in nothing the whole time.
    //
    // Deliberately not a movement check but a POSITION check: asking the motion generator whether
    // it is busy gets a cheerful yes from a generator whose spline never launched. Where the bot
    // actually is, compared with where it actually was, cannot lie.
    // Whether the bot is TRYING to go anywhere right now.
    //
    // Recovery is for a bot that cannot move, not for one that is standing still on purpose: at the
    // auctioneer, between town errands, or waiting out a cooldown there is no movement intent at all,
    // and "recovering" such a bot yanks it away from its business. Measured 2026-10-21 on the first
    // four cases the new watcher captured: two were bots idling at the Booty Bay auction house with
    // no goal of any kind - not stuck, just shopping.
    bool const hasIntent = _hasGoal || !_questGiverGuid.IsEmpty() || !_gatherNodeGuid.IsEmpty()
        || !_lootCorpseGuid.IsEmpty() || _upkeep.hasTownGoal || _workbenchMs > 0;

    if (bot->GetExactDist(&_lastSeenPos) > STILL_TOLERANCE || bot->IsInCombat())
    {
        _lastSeenPos = bot->GetPosition();
        _stillMs = 0;
        _stuckStreak = 0;   // moving again: whatever it was, it is over
    }
    else if (!hasIntent)
    {
        // Idle by design, not stuck: forget the stillness and leave the bot where it is.
        _stillMs = 0;
        _stuckStreak = 0;
    }
    else if ((_stillMs += diff) >= STUCK_AFTER_MS)
    {
        _stillMs = 0;
        _lastSeenPos = bot->GetPosition();

        TC_LOG_INFO("scripts.bots", "pbot: {} has not moved for {} minutes at ({:.0f}, {:.0f}, "
            "{:.1f}) on map {} — recovering it", bot->GetName(), STUCK_AFTER_MS / 60000,
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());

        bot->GetMotionMaster()->Clear();

        // FIRST: try lifting it onto the ground where it already is.
        //
        // This is the actual defect, not a workaround for it. Both bots that ate a four-hour soak
        // were UNDER THE TERRAIN — one 21 to 43 yards below the nearest ground, the other 6 — and
        // the navmesh runs above them, so no path could be built and every module's move was
        // rejected in turn. Put the bot back on the surface and it simply carries on with what it
        // was doing, in the place it chose, with none of its plans thrown away.
        //
        // Teleporting somewhere else is the fallback for when even that is impossible.
        Position onGround = bot->GetPosition();
        if (!PbotTravel::IsStandable(bot, onGround) && PbotTravel::SnapToGround(bot, onGround))
        {
            bot->TeleportTo(bot->GetMapId(), onGround.GetPositionX(), onGround.GetPositionY(),
                onGround.GetPositionZ(), bot->GetOrientation());

            TC_LOG_INFO("scripts.bots", "pbot: {} was under the terrain on map {} — lifted it back "
                "onto the ground", bot->GetName(), bot->GetMapId());

            _lastSeenPos = onGround;
            return;
        }

        // Going home is the right answer ONCE. When it keeps happening, home is the trap.
        //
        // Measured over a four-hour soak: the watchdog fired 149 times, and 98 of those were two
        // bots — Ondrel and Delwin, 49 each, one every five minutes for the entire run. Recovery
        // put them back on their home anchor, the anchor was itself a place the pathfinder will
        // not move them from, and five minutes later they were stuck again. A rescue that returns
        // the victim to the accident is not a rescue.
        //
        // So the second time, the bot moves house: PbotWorldSpots holds the vetted ground the
        // population system uses to place bots in the first place, matched to level so the bot
        // arrives somewhere it can actually live.
        if (++_stuckStreak >= STUCK_STREAK_BEFORE_REHOME)
        {
            _stuckStreak = 0;

            PbotWorldSpots::Spot spot;
            if (PbotWorldSpots::PickForLevel(bot->GetLevel(), spot)
                && bot->TeleportTo(spot.MapId, spot.Pos.GetPositionX(), spot.Pos.GetPositionY(),
                                   spot.Pos.GetPositionZ(), spot.Pos.GetOrientation()))
            {
                _home = spot.Pos;
                _homeMapId = spot.MapId;

                TC_LOG_INFO("scripts.bots", "pbot: {} kept getting stuck at home — moved it to "
                    "zone {} on map {}", bot->GetName(), spot.ZoneId, spot.MapId);
            }
        }
        else if (!PbotHearth::GoHome(bot, _home, _homeMapId))
        {
            bot->TeleportTo(_homeMapId, _home.GetPositionX(), _home.GetPositionY(),
                _home.GetPositionZ(), _home.GetOrientation());
        }

        _hasGoal = false;
        _goalIsTurnIn = false;
        _questGiverGuid.Clear();
        _gatherNodeGuid.Clear();
        _lootCorpseGuid.Clear();
        _upkeep.vendorGuid.Clear();
        _upkeep.hasTownGoal = false;
        return;
    }

    // A movement order SURVIVES a teleport to another map: the generator keeps steering toward the
    // coordinates it was given, which now denote a completely different place. Bots teleported into
    // Warsong Gulch kept walking toward a wander point back in Elwynn Forest — both teams drifting
    // by the exact same vector, ~450 yards a minute, straight off the playable area. Clearing the
    // motion master on a map change is what stops that; the next tick issues a fresh, local order.
    if (bot->GetMapId() != _lastMapId)
    {
        _lastMapId = bot->GetMapId();
        bot->GetMotionMaster()->Clear();
        _chaseTargetGuid.Clear();
        _repositionMs = 0;
        _gatherNodeGuid.Clear();
        _gatherWaitMs = 0;
        _questGiverGuid.Clear();   // the giver it was walking to is on the map it just left
        _lootCorpseGuid.Clear();
        _bgState.HasTarget = false;
        _activity = Activity::Settling;
        return;   // spend this tick settling; act normally from the next one
    }

    // The home anchor only means anything on the map it was recorded on. Off that map — inside a
    // battleground, say — fall back to wherever the bot currently is, or it will walk toward a
    // point that does not exist here.
    bool const onHomeMap = bot->GetMapId() == _homeMapId;

    // Off its home map the anchor is meaningless, so the bot reappears where it fell. That is also
    // why the anchor passed below is a scratch copy in that case: a death in a battleground must
    // not overwrite the bot's real home with a battleground coordinate.
    Position anchor = onHomeMap ? _home : bot->GetPosition();
    uint32 anchorMapId = onHomeMap ? _homeMapId : bot->GetMapId();

    // Learning a trade is not an ACTIVITY, so it does not belong in the ladder of things a bot does
    // with a tick — it belongs above the ladder, where nothing can outrank it.
    //
    // It was below, next to the crafting attempt, and that quietly excluded the busiest bots: every
    // stage between here and there returns early, so a bot that is always fighting or always on a
    // town errand never reached its own profession. Measured: two level-35 bots with craft skill 0
    // and one recipe between them, while quieter bots of half their level knew a thousand. This is
    // the second time setup hidden behind a control path has silently not run — the first was
    // hanging it on bot CREATION, after the population became persistent and stopped being created.
    if (_professionSetupMs > diff)
        _professionSetupMs -= diff;
    else
    {
        _professionSetupMs = PROFESSION_SETUP_INTERVAL_MS;
        PbotProfession::GrantCraft(bot);    // idempotent; also raises a default skill left at 1
        PbotProfession::LearnRecipes(bot);  // the book grows as the skill does
    }

    // Dead: lie there, then get back up. Nothing else happens until that resolves — there is no
    // client to run a corpse back, so this is the whole death story for a world bot.
    if (PbotAutonomy::HandleDeath(bot, diff, _deathTimerMs, anchor, anchorMapId,
        _consecutiveDeaths, _sinceLastDeathMs))
    {
        if (onHomeMap)   // keep a relocation the death handler decided on
        {
            _home = anchor;
            _homeMapId = anchorMapId;
        }
        _activity = Activity::Dead;
        return;
    }

    // Phase 7: if this bot is sitting in a battleground queue and the queue has invited it, take
    // the invite. Checked before anything else so a bot does not wander off mid-invite; the entry
    // itself starts a teleport, after which the worldport handler takes over.
    if (PbotBG::TickQueue(bot))
    {
        _activity = Activity::Battleground;
        return;
    }

    // Inside a battleground, objectives outrank wandering. Combat is NOT short-circuited by this:
    // TickInBattleground returns false whenever there is no objective action to take, and the
    // normal target selection below then applies as usual.
    if (bot->InBattleground() && PbotBG::TickInBattleground(bot, _bgState, diff))
    {
        _activity = Activity::Battleground;
        return;
    }

    if (_repositionMs > diff)
        _repositionMs -= diff;
    else
        _repositionMs = 0;

    // Did the last fight end in a corpse of ours? Counted, because "do the bots actually kill things"
    // had no number anywhere: no log line records a kill, so the only evidence was indirect and the
    // criterion could not be closed. The engine's own answer to "who may loot this" is what tap and
    // group credit are built on, so it is the honest test rather than a guess from damage done.
    if (!_lastVictimGuid.IsEmpty())
    {
        Unit* killed = ObjectAccessor::GetUnit(*bot, _lastVictimGuid);
        if (!killed || !killed->IsAlive())
        {
            Creature const* corpse = (killed && killed->GetTypeId() == TYPEID_UNIT)
                ? killed->ToCreature() : nullptr;

            if (corpse)
            {
                // BOTH tests are logged, because this project has already been burned once by
                // trusting one engine gate on a bot with no socket (IsValidAttackTarget says no to
                // every creature in the world for a client-less player). If both are false for every
                // corpse then the bots genuinely are not killing anything; if only one is false, the
                // counter is the thing that is wrong and the log says which.
                //
                // isTappedBy asks "was this corpse tapped by me or my group" — the same credit that
                // decides loot rights, so it is the engine's own answer to "did this fight involve me".
                bool const lootOk = bot->isAllowedToLoot(corpse);
                bool const tappedOk = corpse->isTappedBy(bot);

                if (lootOk || tappedOk)
                {
                    ++_kills;

                    // Hand the body straight to the loot module, NOW.
                    //
                    // Waiting for PbotLoot's own 3-second scan is what let most kills rot: measured
                    // 132 kills against 31 loots (23%), and two suspected causes were ruled out with
                    // their own counters — "it was fighting" (0 corpses ever found beside a fighting
                    // bot) and "loot rights" (adding isTappedBy changed nothing). What is left is
                    // simply that the bot walks off before the timer notices. The corpse is here and
                    // we are still in range, so there is nothing to wait for.
                    // Queue the body, and only take the current slot if it is free — overwriting a
                    // corpse the bot is already walking to is what lost most of them.
                    PbotLoot::NoteCorpse(bot, corpse->GetGUID());
                    if (_lootCorpseGuid.IsEmpty())
                        _lootCorpseGuid = corpse->GetGUID();
                    _lootCooldownMs = 0;
                    _lootWalkMs = 0;

                    TC_LOG_INFO("scripts.bots", "pbot kill: {} killed entry {} (kills {})",
                        bot->GetName(), corpse->GetEntry(), _kills);
                }
                else
                {
                    TC_LOG_INFO("scripts.bots", "pbot fight: {} ended a fight, corpse entry {} not "
                        "ours (loot={} tapped={})", bot->GetName(), corpse->GetEntry(),
                        lootOk, tappedOk);
                }
            }

            _lastVictimGuid.Clear();
        }
    }

    // Already fighting something, or something is fighting us — that outranks every other concern.
    Unit* target = bot->GetVictim();
    if (!target || !target->IsAlive())
        target = bot->getAttackerForHelper();

    // Break off a fight that is already lost. Checked before the fight itself so the decision to
    // leave is made once and then honoured — a bot that re-decides every tick swings, steps back,
    // swings again and dies anyway.
    //
    // The ordering matters: a retreat in progress outranks picking a target, because the bot that
    // just broke off is still being attacked and getAttackerForHelper would hand the same enemy
    // straight back to it.
    //
    // Measured over two 30-minute runs of equal length: with retreat 35 kills / 126 deaths / 139
    // quests, without it 35 kills / 141 deaths / 95 quests. Same kills, fewer deaths, markedly more
    // questing — a bot that walks away from a lost fight spends that time playing instead of dying.
    if (_retreatMs || (target && target->IsAlive() && PbotAutonomy::ShouldRetreat(bot, target)))
    {
        if (PbotAutonomy::Retreat(bot, _retreatMs, diff))
        {
            _activity = Activity::Retreat;
            return;
        }
    }

    if (target && target->IsAlive() && target->IsInWorld())
    {
        // Nobody fights from the saddle. Dismounting here rather than at the start of travel means
        // a bot ambushed mid-journey lands on its feet before it swings.
        PbotMount::Dismount(bot);

        if (_resting)
        {
            PbotAutonomy::StopResting(bot);
            _resting = false;
        }
        // No owner: the rotation's owner-dependent steps (the priest's shield upkeep) null-check
        // ctx.Owner, and HealLowestAlly falls back to self-only, so passing nullptr is supported.
        TickCombat(bot, nullptr, target, diff);
        _activity = Activity::Combat;
        return;
    }

    // Out of combat from here on. Clear any leftover engagement state.
    if (bot->GetVictim())
        Disengage(bot);

    // Say something occasionally, and look for company. Both are cheap, both are throttled hard,
    // and between them they are most of the difference between "scripted actor" and "someone else
    // playing on this server" — silence and solitude are what give a bot away first.
    if (_lastKnownLevel && bot->GetLevel() > _lastKnownLevel)
        PbotSocial::OnLevelUp(bot, _speakCooldownMs);
    _lastKnownLevel = bot->GetLevel();

    // A refusal is only ever true of the place it happened, and this set never expired. Most of what
    // lands in it is a quest the bot cannot finish where it stands — a taker on another continent —
    // which stops being true the moment the bot arrives on that continent. Carrying the refusal
    // across would make the trip pointless and the bot would stand next to the taker doing nothing.
    if (_lastKnownMapId != bot->GetMapId())
    {
        _lastKnownMapId = bot->GetMapId();
        _refusedQuests.clear();
    }

    PbotSocial::TickChatter(bot, _speakCooldownMs, diff);
    PbotGroup::Tick(bot, _partyCooldownMs, diff);

    // Have I outgrown this zone? Checked here — out of combat, before any of the local errands —
    // because moving house invalidates every one of them: the node, the questgiver and the corpse
    // the bot was walking to are all back in the zone it just left.
    //
    // But NOT while there is finished work that can still be delivered from here. A player leaves a
    // zone empty: they hand in what they did, and only then move on. A bot that moves on first turns
    // every completed quest in its log into a permanent passenger — measured, the population was
    // carrying 263 finished quests and delivering two or three in forty minutes, because the takers
    // were all continents behind them. The cheapest way to stop the pile growing is to not walk away
    // from the counter.
    // ANY finished work whose taker is on this map, not just work within walking range this tick.
    //
    // This asked FindTurnIn, which stops at MAX_TURN_IN_DISTANCE. A bot holding sixteen completed
    // quests whose takers stood 1,500 yards away therefore reported "nothing owed" and moved
    // continent — converting work that was merely far into work that is unreachable forever.
    // Measured on the live realm: of 282 completed-undelivered quests, 188 had their taker on a map
    // the bot was no longer on, against 36 that were genuinely in range and should have been walked
    // to. The guard was answering a different question from the one it was asked.
    // Nothing to deliver HERE, but something waiting on another map? Go and collect it, before
    // considering a move for level reasons. This is the return half of the journey the bot already
    // made outbound: it walked away from earned rewards, and without this nothing ever brings it
    // back. A player flies home for the same reason.
    // Gated on "nothing I would actually walk to", not "nothing on this map". The unbounded check
    // belongs to the migration guard below, where the question is whether leaving would strand
    // something; here the question is whether staying achieves anything.
    // The cross-map lift is consulted ALWAYS, not only when nothing walkable exists.
    //
    // The old gate read `if (!FindTurnIn(...) && PbotDelivery::Tick(...))`, so a single taker within
    // 600 yards switched delivery off entirely. Measured: enders sit on the bot's OWN map but 2000+
    // yards away (Mountaineer Stormpike at (-4818,-2696) while Kessa stood at (-5272,-513) — 2230
    // yards), FindTurnIn refuses anything past 600, and the bot never went — it is not that the NPC is
    // missing, it is that nobody ever drove the bot to it. PbotDelivery makes its own decision
    // (FindRemoteTurnIn returns false when everything is walkable, and the trip threshold applies), so
    // consulting it unconditionally cannot send a bot away from work it can already reach.
    // Standing beside the taker with finished work, nothing else may take this tick.
    bool const takerInReach = PbotQuest::HasTakerInReach(bot);

    if (!takerInReach && PbotDelivery::Tick(bot, _deliveryCooldownMs, diff, _home, _homeMapId))
    {
        _chaseTargetGuid.Clear();
        _repositionMs = 0;
        _gatherNodeGuid.Clear();
        _gatherWaitMs = 0;
        _questGiverGuid.Clear();
        _lootCorpseGuid.Clear();
        _upkeep.vendorGuid.Clear();
        _upkeep.hasTownGoal = false;
        _bgState.HasTarget = false;
        _hasGoal = false;
        _goalIsTurnIn = false;
        return;
    }

    if (!PbotQuestGoal::HasDeliverableOnMap(bot)
        && PbotMigrate::Tick(bot, _migrateCooldownMs, diff, _home, _homeMapId))
    {
        _chaseTargetGuid.Clear();
        _repositionMs = 0;
        _gatherNodeGuid.Clear();
        _gatherWaitMs = 0;
        _questGiverGuid.Clear();
        _lootCorpseGuid.Clear();
        _upkeep.vendorGuid.Clear();
        _upkeep.hasTownGoal = false;   // a destination on the old map means nothing here
        _bgState.HasTarget = false;
        return;
    }

    // The arbiter: what is this bot doing for the next few minutes?
    //
    // Combat has had its turn above, and so has the cross-map lift. From here the rest of this
    // function is a set of OPTIONS rather than a fixed order, and the one option promoted above them
    // is delivering work that is already finished — ahead of looting, hunting, gathering and the
    // whole town errand. Measured, the fixed order is why a queue of ~266 finished quests sat flat
    // for eight hours while bots kept walking to objectives they had not finished.
    //
    // Every transition is logged: this is the measurement channel for the intent weights, the same
    // way "pbot travel:" lines are for the travel ladder.
    PbotIntent::Kind const wasIntent = _intent;
    _intent = PbotIntent::Decide(bot, _intent, _intentCooldownMs, diff);

    if (_intent != wasIntent)
        TC_LOG_INFO("scripts.bots", "pbot intent: {} is now {}", bot->GetName(),
            PbotIntent::Name(_intent));

    if (_intent == PbotIntent::Kind::DeliverFinished && TrySetOutForTurnIn(bot))
    {
        _activity = Activity::Travelling;
        return;
    }

    // Recover before looking for the next fight. Two thresholds rather than one so a bot hovering
    // at the boundary does not stand up and sit down on alternate ticks.
    //
    // Both thresholds are shifted by this bot's caution, which is what stops sixty characters
    // sitting down at the same health on the same tick. A reckless one fights on at a fifth of its
    // health; a careful one is on the ground at half and stays there until nearly full.
    PbotPersonality::Traits const& self = PbotPersonality::Of(bot->GetGUID());
    float const restBelow = PbotPersonality::Scale(self.Caution, PbotAutonomy::REST_BELOW_HEALTH_PCT, 0.6f, 1.5f);
    float const resumeAbove = PbotPersonality::Scale(self.Caution, PbotAutonomy::RESUME_ABOVE_HEALTH_PCT, 0.8f, 1.3f);

    float const healthPct = bot->GetHealthPct();
    if (_resting)
    {
        if (healthPct < resumeAbove)
        {
            PbotAutonomy::Rest(bot);
            _activity = Activity::Resting;
            return;
        }
        PbotAutonomy::StopResting(bot);
        _resting = false;
    }
    else if (healthPct < restBelow)
    {
        _resting = true;
        PbotAutonomy::Rest(bot);
        _activity = Activity::Resting;
        return;
    }

    // Loot BEFORE anything that walks away from it — including a journey already in flight.
    //
    // Measured: 101 kills produced only 15 loots (15%), because this rung sits below the travel gate
    // and a bot with a goal in flight simply walks off its own corpses. Looting is the one errand that
    // is a consequence of what the bot just did rather than a new decision, and it is measured at ~0%
    // of the day's ticks — so lifting it above the gate cannot re-create the journey oscillation the
    // gate exists to stop.
    if (PbotLoot::Tick(bot, _lootCorpseGuid, _lootCooldownMs, _lootWalkMs, diff))
    {
        _activity = Activity::Looting;
        return;
    }

    // A journey already under way owns the bot's feet until it arrives or gives up.
    //
    // This is the standing answer to the "carried it to the objective" lines — 468 of them in one
    // run, which is by far the loudest non-human thing the population does: from the outside a bot
    // that is teleported to its objective looks exactly like a player teleporting.
    //
    // What the measurement showed: a bot told to walk 42 yards to a taker ended 58 yards away after
    // 180 seconds, with no combat and no failed movement order; and a trace of its own distance read
    // 73 -> 66 -> 73. That is not a bot standing still and not a bot the navmesh refused — it is a
    // bot being pulled two ways. Everything below this line is an ERRAND, and every errand issues
    // its own MovePoint: laid over the journey's hop, it turns a walk into a wander between two
    // points a few yards apart, and at the end of the budget the goal is teleported rather than
    // reached. 21% of the day is already spent "travelling" while the queue of finished work does
    // not move, which is what competing orders look like in the activity budget.
    //
    // The workbench rung below has had precisely this protection for precisely this reason
    // ("everything below this line would otherwise send the bot somewhere else before it arrived").
    // The journey itself simply never got it.
    //
    // Combat stays ABOVE this line and still wins: a bot attacked on the way stops and fights, then
    // carries on. Resting, the cross-map reward lift and moving house are decided above it too, as
    // before. What is skipped here is only an errand that would have STARTED.
    if (_hasGoal)
    {
        if (TickTravel(bot, diff))
        {
            _activity = Activity::Travelling;
            return;
        }
    }

    // A bot with no kit left goes to the repairer BEFORE it goes hunting.
    //
    // The measured reason, and it is an ORDERING defect rather than a missing feature: the town
    // errand sits BELOW hunt and gather in this ladder, so a bot that is always fighting never
    // reaches it. Kaelen (level 24, 1 478 994 copper, 16 of 16 equipped items at 0%) had NOT ONE
    // "pbot upkeep" line in a whole window — its entire trace was retreat at 43%, retreat at 34%,
    // retreat at 27%, heal, the same fight again, and then death; 59 of them. Repair itself is proven
    // (Ithric: "repaired 9 items for 9, purse 984953, gear now 100%"); these bots simply never got
    // there, because the hunt rung above kept claiming every tick.
    //
    // Gated on having money to pay, so this cannot become a walk in a circle to a repairer it cannot
    // afford: the bots with an empty purse keep the old order and their own, different problem.
    if (!takerInReach && PbotUpkeep::GearNeedsRepair(bot) && bot->GetMoney() > 0
        && PbotUpkeep::Tick(bot, _upkeep, _home, _homeMapId, diff))
    {
        _activity = Activity::Upkeep;
        return;
    }

    // Looting now runs ABOVE the travel gate (see the note there): it is the one errand that follows
    // from what the bot just did, and leaving it here meant 85% of its own kills were walked away from.

    // Both searches below walk grid cells, so they are rate limited rather than run every tick
    // (see _huntCooldownMs). Everything after them — gathering and wandering — is cheap and still
    // runs at full rate.
    if (_huntCooldownMs > diff)
    {
        _huntCooldownMs -= diff;
    }
    else
    {
        _huntCooldownMs = PBOT_HUNT_INTERVAL_MS;

        // Phase 7: an enemy player outranks wildlife — a world bot that ignores the hostile
        // standing ten yards away to keep hitting a boar is the most obvious tell there is.
        if (Unit* enemy = PbotPvP::FindHostilePlayer(bot))
        {
            bool const ranged = ClassIsRanged(bot->GetClass());
            bot->Attack(enemy, /*meleeAttack*/ !ranged);
            DoReposition(bot, enemy, ranged);
            _activity = Activity::Hunting;
            return;
        }

        // Otherwise pick a fair fight from the local wildlife, as far off as this bot cares to look.
        // An aggressive one crosses the clearing for a kill; a timid one only fights what walks up
        // to it, which over an hour is a visibly different player.
        //
        // ...but idle hunting does NOT outrank walking to a quest objective, and getting that
        // ordering wrong is what stopped the bots questing at all. Combat sits above travel in this
        // ladder, and in a living world there is nearly always something fightable within 36 to 84
        // yards — so the tick ended here, every time, and the travel step below was simply never
        // reached. Measured: 45% of all ticks went to fighting while 352 of 362 quests in progress
        // had not advanced a single objective, and the targets were 200 yards away and further.
        //
        // A player with a quest walks to the quest. They fight what attacks them on the way — which
        // still happens, because a bot already in combat never reaches this code — but they do not
        // stop for every boar between here and the objective.
        //
        // Deliberately narrow: only when the bot HAS an unfinished kill objective it can act on.
        // With nothing to walk to, hunting is exactly the right thing to be doing.
        // The test is "is there somewhere to GO", not "is there work outstanding", and the
        // difference is the whole safety of this change. A bot whose objectives are all on another
        // continent has work outstanding and nowhere to walk: told to stop hunting it would simply
        // stand in a field forever, which is worse than the problem being fixed.
        Position objective;
        bool const somewhereToBe = PbotQuestGoal::Find(bot, objective);

        float const huntRange = PbotPersonality::Scale(self.Aggression, PbotAutonomy::SEARCH_RANGE, 0.6f, 1.4f);

        // Somewhere to be: fight only what the quest names, and let the travel step below carry the
        // bot there. Nowhere to be: fight anything fair, which is exactly right for a bot with no
        // errand to run.
        Unit* prey = somewhereToBe ? PbotAutonomy::FindQuestTarget(bot, huntRange)
                                   : PbotAutonomy::FindTarget(bot, huntRange);

        if (prey)
        {
            bool const ranged = ClassIsRanged(bot->GetClass());
            bot->Attack(prey, /*meleeAttack*/ !ranged);
            DoReposition(bot, prey, ranged);
            _activity = Activity::Hunting;
            return;
        }
    }

    // Nothing to fight — harvest anything worth harvesting nearby. Ordered after combat and before
    // wandering on purpose: a player with a gathering profession picks the herb they walked past,
    // they do not wander off and come back for it.
    //
    // Whether they bother at all is character: some players stop for every node, others walk past
    // ore their whole career. Gated on greed so both kinds exist.
    if (!takerInReach && self.Greed >= MIN_GREED_TO_GATHER
        && PbotGather::Tick(bot, _gatherNodeGuid, _gatherCooldownMs, _gatherWaitMs, diff))
    {
        _activity = Activity::Gathering;
        return;
    }

    // Town business outranks questing: a bot with full bags cannot pick up a single quest item, so
    // going shopping first is not a detour, it is what makes the next quest possible at all.
    if (!takerInReach && PbotUpkeep::Tick(bot, _upkeep, _home, _homeMapId, diff))
    {
        _activity = Activity::Upkeep;
        return;
    }

    // While in town: open the mail and visit the auction house. Both are throttled to a slow tick
    // because both walk grid cells, and neither is urgent — an unread letter and an unsold ore stack
    // cost the bot nothing for another minute.
    //
    // Deliberately AFTER upkeep, so a bot that came to town to repair does that first: the auction
    // is where its money comes from, but the armour is what keeps it alive to earn more.
    if (_marketCooldownMs > diff)
        _marketCooldownMs -= diff;
    else
    {
        _marketCooldownMs = MARKET_INTERVAL_MS;

        // Mail first — that is where the money from previous sales is, and the bot may need it to
        // afford what it is about to buy.
        PbotAuction::CollectMail(bot);

        if (PbotAuction::SellAtAuction(bot) || PbotAuction::BuyAtAuction(bot))
        {
            _activity = Activity::Market;
            return;   // spent the tick at the auctioneer
        }
    }

    // A bot on its way to a forge is left alone until it gets there. Same reasoning as the retreat
    // above: a decision worth making is worth honouring for longer than one tick, and everything
    // below this line would otherwise send the bot somewhere else before it arrived.
    if (_workbenchMs)
    {
        _workbenchMs = _workbenchMs > diff ? _workbenchMs - diff : 0;
        if (_workbenchMs)
        {
            _activity = Activity::Workbench;
            return;
        }

        _craftCooldownMs = 0;   // arrived (or gave up) — try the recipe now, not in another minute
    }

    // Practise the craft. Gated on its own slower timer and placed after the market so a bot in town
    // does its errands first — the workbench keeps, the auctioneer's queue does not.
    //
    // I first wrote that crafting where the bot stands was deliberate, on the belief that retail no
    // longer needs a forge for most recipes. That was wrong, and the measurement said so plainly:
    // every refusal was SPELL_FAILED_REQUIRES_SPELL_FOCUS. The bot goes to the workbench.
    if (_craftCooldownMs > diff)
        _craftCooldownMs -= diff;
    else
    {
        _craftCooldownMs = PbotProfession::CRAFT_INTERVAL_MS;

        switch (PbotProfession::Craft(bot))
        {
            case PbotProfession::CraftOutcome::Made:
                _activity = Activity::Crafting;
                return;
            case PbotProfession::CraftOutcome::WalkingToWorkbench:
                _workbenchMs = PbotProfession::WORKBENCH_WALK_MS;
                _activity = Activity::Workbench;
                return;
            case PbotProfession::CraftOutcome::Idle:
                break;
        }
    }

    // Then questing. After gathering because a node is right there and a questgiver is a walk away,
    // and before wandering because "walk to the exclamation mark" IS the walk a player would make —
    // it replaces aimless wandering with wandering that has a reason.
    if (PbotQuest::Tick(bot, _questGiverGuid, _questCooldownMs, _questWalkMs, _speakCooldownMs,
        _refusedQuests, _pruneCooldownMs, diff))
    {
        _activity = Activity::Questing;
        return;
    }

    // Then go somewhere. Last of the errands and before wandering, because everything above is
    // something worth stopping for on the way — a bot that walks past a herb or an enemy to reach
    // its objective is exactly as wrong as one that never leaves its spawn point.
    if (TickTravel(bot, diff))
    {
        _activity = Activity::Travelling;
        return;
    }

    PbotAutonomy::Wander(bot, anchor);
}

bool PbotAI::TrySetOutForTurnIn(Player* bot)
{
    // Nothing to decide while the bot is already on its way somewhere, or resting between journeys.
    if (_hasGoal || _goalCooldownMs)
        return false;

    Position taker;
    if (!PbotQuestGoal::FindTurnIn(bot, taker))
        return false;

    // Only takers within WALKING range are promoted here.
    //
    // FindTurnIn also offers spots further away when a flight path could reach them, which is right
    // for a goal the bot chooses for itself — it will fly most of the way and walk the rest. It is
    // wrong for this promoted path, because promoting the GOAL does not promote the LIFT: measured,
    // those goals became doomed marches (5 % of 8,000 yards in 180 seconds) that ended in the
    // give-up teleport — which is where 468 "carried it to the objective" came from, entirely without
    // combat and with the pathfinder working.
    //
    // Out-of-reach work belongs to PbotDelivery, which runs earlier in the ladder and knows how to
    // get there by air. So this declines and lets the ladder fall through to it.
    if (bot->GetExactDist(&taker) > PbotQuestGoal::MAX_TURN_IN_DISTANCE)
        return false;

    _goal = taker;
    _hasGoal = true;
    _goalIsQuest = true;
    _goalIsTurnIn = true;
    _goalStartDist = bot->GetExactDist(&_goal);
    _goalMs = 0;
    _goalCombatMs = 0;
    _noQuestMs = 0;
    PbotTravel::ResetHops(bot);   // this journey's count, not the previous one's

    // The reward leg, and for most of this project's life the only leg of the ladder with no voice at
    // all. Setting out for a reward is a rare, meaningful event, exactly like setting out for an
    // objective, so it is logged unthrottled.
    TC_LOG_INFO("scripts.bots", "pbot travel: {} SET OUT to hand in finished work — {:.0f}y away "
        "on map {}", bot->GetName(), _goalStartDist, bot->GetMapId());

    return true;
}

bool PbotAI::TickTravel(Player* bot, uint32 diff)
{
    PbotPersonality::Traits const& self = PbotPersonality::Of(bot->GetGUID());

    if (_goalCooldownMs > diff)
        _goalCooldownMs -= diff;
    else
        _goalCooldownMs = 0;

    // Delivering finished work outranks an objective the bot has merely set out for.
    //
    // This is the defect the travel probes named: while a goal was in flight the turn-in was never
    // even consulted — "a goal is ALREADY in flight" was logged 19–24 times against 13–16 times the
    // decision was reached at all — and the queue of finished work sat flat at ~266 for eight hours
    // while bots walked to quest objectives they had not finished.
    //
    // Three guards, and the first two are not caution — they are the fix for what the first version of
    // this did. It turned aside for a taker the SAME distance away that it was already walking to
    // (236y vs 236y), every tick, 129,196 times in one window, clearing its own motion master each
    // time: a bot that cancels its own order and reissues it never goes anywhere. So:
    //   - never turn aside from a hand-in trip already under way (there is nothing nearer than the
    //     thing you are already doing);
    //   - the new taker must be NEARER BY A REAL MARGIN, not by noise;
    //   - and at most once a minute, so a bot cannot re-aim itself in a loop.
    if (_hasGoal && _goalIsQuest && !_goalIsTurnIn)
    {
        if (_turnAsideMs > diff)
            _turnAsideMs -= diff;
        else
        {
            Position taker;
            if (PbotQuestGoal::FindTurnIn(bot, taker))
            {
                float const currentDist = bot->GetExactDist(&_goal);
                float const takerDist = bot->GetExactDist(&taker);

                if (takerDist + TURN_ASIDE_MARGIN < currentDist)
                {
                    _goal = taker;
                    _goalIsTurnIn = true;
                    _goalStartDist = takerDist;
                    _goalMs = 0;
        _goalCombatMs = 0;
                    PbotTravel::ResetHops(bot);
                    _turnAsideMs = TURN_ASIDE_COOLDOWN_MS;
                    PbotMount::Dismount(bot);
                    bot->GetMotionMaster()->Clear();

                    TC_LOG_INFO("scripts.bots", "pbot travel: {} turned aside from its objective — a "
                        "taker for finished work stands {:.0f}y off, against {:.0f}y it was walking",
                        bot->GetName(), takerDist, currentDist);

                    return true;
                }
            }
        }
    }

    if (_hasGoal)
    {
        if (ShouldReportTravel(bot))
            TC_LOG_INFO("scripts.bots", "pbot travel gate: {} — a goal is ALREADY in flight ({:.0f}y "
                "away, {:.0f}s of budget spent, quest goal {}) so the turn-in is never consulted",
                bot->GetName(), bot->GetExactDist(&_goal), _goalMs / 1000.0, _goalIsQuest ? "yes" : "no");

        // Same budget as everything else that walks somewhere: a destination the pathfinder cannot
        // reach must not become a life sentence. Without this the bot re-issues a failing move
        // every tick and reports "busy", so wandering, hunting and everything else never runs.
        _goalMs += diff;
        if (bot->IsInCombat())
            _goalCombatMs += diff;
        if (_goalMs > PBOT_GOAL_WALK_BUDGET_MS)
        {
            // Say how far short it stopped. Two behaviour changes in a row shifted the time budget
            // and moved objective progress not at all — which means the chain breaks somewhere
            // between choosing a destination and standing on it, and nothing in this loop has ever
            // said which. A number here settles it: still 900 yards out means the budget is simply
            // too small for the distances involved; 30 yards out means it arrives and the work is
            // not there.
            float const remaining = bot->GetExactDist(&_goal);

            // Almost no ground covered means this place cannot be walked out of, and no amount of
            // retrying will change that. Measured: two bots spent the whole run in Thunder Bluff and
            // Orgrimmar closing 15 yards per three minutes — a fiftieth of walking pace — because a
            // capital is mesas, lifts and tunnels that the navmesh does not join up. Cadoc, standing
            // in open country, reached its objectives and picked new ones normally.
            //
            // So: carry it there, exactly as the turn-in lift does for a reward it cannot walk to
            // (41 attempts, 40 deliveries). A player in a capital takes the lift down; a bot has no
            // lift, and crawling at a fiftieth of pace is not a behaviour worth preserving.
            bool const wentNowhere = _goalIsQuest && _goalStartDist > 0.0f
                && remaining > _goalStartDist * KEPT_DISTANCE_MEANS_STUCK;

            if (wentNowhere)
            {
                // No teleport to the objective.
                //
                // Carrying the bot there was the loudest non-human thing the population did: from
                // outside it is a player blinking across the map, and it still measured ~13 an hour.
                // A player who cannot walk somewhere does not teleport - they drop that errand and do
                // something else, and if they are genuinely wedged the watchdog lifts them onto the
                // ground they are standing on.
                //
                // So: write the destination off, say so, and let the goal go. The bot then chooses
                // again from where it actually is.
                TC_LOG_INFO("scripts.bots", "pbot goal: {} could not walk out of where it was "
                    "({:.0f} of {:.0f} yards in {}s; combat {}s of it, {} dead movement orders, "
                    "{} hops accepted, {}y short) - giving up that objective",
                    bot->GetName(), _goalStartDist - remaining, _goalStartDist,
                    PBOT_GOAL_WALK_BUDGET_MS / 1000, _goalCombatMs / 1000,
                    PbotTravel::FailedHops(bot), PbotTravel::HopsIssued(bot), remaining);

                PbotQuestGoal::MarkUnreachable(bot->GetMapId(), _goal);

                // The usual reason a bot cannot walk out is that it is UNDER the terrain: the
                // navmesh runs above it, so every move is rejected. Lifting it onto the ground it
                // is already standing on is the same correction the stuck watchdog makes, and it
                // keeps the bot in the place it chose - unlike carrying it across the map to the
                // objective, which was visible from outside and measured ~13 an hour.
                Position onGround = bot->GetPosition();
                if (!PbotTravel::IsStandable(bot, onGround) && PbotTravel::SnapToGround(bot, onGround))
                {
                    bot->TeleportTo(bot->GetMapId(), onGround.GetPositionX(), onGround.GetPositionY(),
                        onGround.GetPositionZ(), bot->GetOrientation());

                    TC_LOG_INFO("scripts.bots", "pbot goal: {} was under the terrain - lifted onto the "
                        "ground where it stood (map {})", bot->GetName(), bot->GetMapId());

                    _lastSeenPos = onGround;
                }
            }
            else
            {
                TC_LOG_INFO("scripts.bots", "pbot goal: {} gave up {} after {}s, still {:.0f} yards away "
                    "({} hops accepted)",
                    bot->GetName(), _goalIsQuest ? "a quest objective" : "a roam",
                    PBOT_GOAL_WALK_BUDGET_MS / 1000, remaining, PbotTravel::HopsIssued(bot));
            }

            _hasGoal = false;
        _goalIsTurnIn = false;
            _goalMs = 0;
        _goalCombatMs = 0;
            _goalCooldownMs = PBOT_GOAL_RETRY_MS;
            PbotMount::Dismount(bot);
            return false;
        }

        if (PbotTravel::StepToward(bot, _goal))
        {
            // Ride, if the remaining journey is long enough to be worth it. Checked every tick
            // rather than once at departure so a bot knocked off its mount by a fight gets back on
            // afterwards instead of finishing the trip on foot.
            if (bot->GetExactDist(&_goal) > PbotMount::WORTH_MOUNTING)
                PbotMount::MountUp(bot);
            return true;
        }

        PbotMount::Dismount(bot);   // arrived

        // Arriving at a HAND-IN is the moment the quest module must act, not "after the next
        // cooldown". It sits below gathering and town errands in the ladder, so a bot that walked
        // hundreds of yards to a taker and then found a herb or a repairer first would hand in
        // nothing — measured, 58 "SET OUT to hand in" ended in 3 hand-ins. Clearing its gate here
        // makes the module run on the very next tick; it still decides for itself whether the taker
        // is in reach (FindGiver, 120y) and walks the last few yards (MoveToGiver).
        if (_goalIsTurnIn)
        {
            // Report the state of the world at this exact spot before the quest module acts on it.
            // The regular probe runs once a minute wherever the bot is, so it almost never fires at
            // the taker; this one does, which is what makes "givers in range 0" interpretable.
            PbotQuest::ReportGiverSearchNow(bot);

            _questCooldownMs = 0;
            _questWalkMs = 0;
            _questGiverGuid.Clear();
        }

        // Arrived (or the path gave out). Make this the new home: the point of the journey was to
        // live somewhere else for a while, and leaving the anchor behind would drag the bot back.
        if (_goalIsQuest)
            TC_LOG_INFO("scripts.bots", "pbot travel: {} ARRIVED on the journey it set out on "
                "(map {}, {:.0f}y short of the recorded spot)", bot->GetName(), bot->GetMapId(),
                bot->GetExactDist(&_goal));

        _home = bot->GetPosition();
        _homeMapId = bot->GetMapId();
        _hasGoal = false;
        _goalIsTurnIn = false;
        _goalMs = 0;
        _goalCombatMs = 0;

        // Linger longer after reaching a quest objective than after a plain roam — the bot is now
        // standing among the creatures the quest wants, and the hunt logic needs time to work.
        _goalCooldownMs = _goalIsQuest ? PBOT_GOAL_QUEST_LINGER_MS : PBOT_GOAL_ROAM_LINGER_MS;
        return false;
    }

    if (_goalCooldownMs)
    {
        if (ShouldReportTravel(bot))
            TC_LOG_INFO("scripts.bots", "pbot travel gate: {} — goal cooldown still has {:.0f}s left, "
                "so the turn-in is not consulted", bot->GetName(), _goalCooldownMs / 1000.0);
        return false;
    }

    if (ShouldReportTravel(bot))
    {
        Position offered;
        bool const can = PbotQuestGoal::FindTurnIn(bot, offered);
        std::string const answer = can
            ? Trinity::StringFormat("YES, a taker {:.0f}y away", bot->GetExactDist(&offered))
            : std::string("NO");
        TC_LOG_INFO("scripts.bots", "pbot travel probe: {} map {} — the turn-in decision IS reached "
            "with a clear goal; offered {}", bot->GetName(), bot->GetMapId(), answer);
    }

    // FINISHED work first, before going looking for more. A player who has killed the eight wolves
    // walks back and collects the reward; only then do they ask what else needs doing.
    //
    // This ordering is not a nicety, it is the difference between questing and hoarding: measured,
    // the population was holding 255 COMPLETED quests and handing in roughly one every half hour.
    // The log fills, the soft cap stops new quests being taken, and the bot spends the rest of its
    // life carrying finished errands it never delivers. Nobody chases the reward harder than the
    // person who already earned it, so this one is not gated on diligence.
    if (TrySetOutForTurnIn(bot))
        return true;

    // An unfinished quest is a real reason to be somewhere; roaming is what a player does when
    // they have none. A diligent bot always goes and does the quest; a slack one often cannot be
    // bothered and wanders off to kill things instead, which is a recognisable kind of player.
    if (roll_chance(int32(PbotPersonality::Scale(self.Diligence, 100.0f, 0.35f, 1.0f)))
        && PbotQuestGoal::Find(bot, _goal))
    {
        // The other half of the measurement: how far the journey was when it began. Paired with the
        // give-up line above it says whether a bot covers ground at all — "set out 900 yards away,
        // gave up 880 yards away" and "set out 900, gave up 40" are different defects entirely.
        TC_LOG_INFO("scripts.bots", "pbot goal: {} set out for a quest objective {:.0f} yards away",
            bot->GetName(), bot->GetExactDist(&_goal));

        _hasGoal = true;
        _goalIsTurnIn = false;
        _goalIsQuest = true;
        _goalStartDist = bot->GetExactDist(&_goal);
        _noQuestMs = 0;
        PbotTravel::ResetHops(bot);
        return true;
    }

    // No quest to pursue. Keep count, and once that has been true for long enough, hearth home the
    // way a player does when a zone has run dry — an orc ends up in Durotar, among its own people
    // and its own quests, instead of circling an empty field forever.
    _noQuestMs += PBOT_GOAL_ROAM_LINGER_MS;
    if (_noQuestMs >= PBOT_HEARTH_AFTER_MS)
    {
        _noQuestMs = 0;
        if (PbotHearth::GoHome(bot, _home, _homeMapId))
        {
            _hasGoal = false;
        _goalIsTurnIn = false;
            _goalCooldownMs = PBOT_GOAL_QUEST_LINGER_MS;   // settle in before deciding anything
            return true;
        }
    }

    if (PbotTravel::PickRoamPoint(bot, _goal))
    {
        _hasGoal = true;
        _goalIsTurnIn = false;
        _goalIsQuest = false;
        PbotTravel::ResetHops(bot);
        return true;
    }

    _goalCooldownMs = PBOT_GOAL_RETRY_MS;
    return false;
}

Unit* PbotAI::SelectCombatTarget(Player* bot, Player* owner) const
{
    // Passive never engages, no matter who is swinging at whom (the disengage itself already
    // happened in ApplyCommand; this just keeps us out of combat afterwards).
    if (_stance == PbotStance::Passive)
        return nullptr;

    bool const ownerFighting = owner->IsAlive() && owner->IsInCombat();

    // "стой" means hold this spot while the owner pulls: the bot does not join the owner's fight,
    // but it absolutely still defends itself. _forcedAttack ("в бой") is the explicit override.
    if (_stay && !_forcedAttack && !bot->IsInCombat())
        return nullptr;

    if (!ownerFighting && !bot->IsInCombat() && !_forcedAttack)
        return nullptr;

    // Defend flips the priority: whatever is hitting the OWNER outranks whatever the owner chose
    // to hit. Assist keeps the Phase 3.2 order (owner's explicit victim first).
    Unit* target = nullptr;
    if (_stance == PbotStance::Defend)
    {
        target = owner->getAttackerForHelper();
        if (!target || !target->IsAlive())
            target = owner->GetVictim();
    }
    else
    {
        target = owner->GetVictim();
        if (!target || !target->IsAlive())
            target = owner->getAttackerForHelper();
    }

    // Nothing on the owner's side — fall back to whatever is attacking us. This is the path a
    // Staying or self-defending bot takes, and it is why "стой" is safe to use mid-pull.
    if (!target || !target->IsAlive())
        target = bot->getAttackerForHelper();

    if (!target || !target->IsAlive() || !target->IsInWorld())
        return nullptr;

    return target;
}

void PbotAI::Disengage(Player* bot)
{
    if (bot->GetVictim())
        bot->AttackStop();

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        bot->GetMotionMaster()->Clear();

    _chaseTargetGuid.Clear();
    _repositionMs = 0;
}

void PbotAI::ApplyCommand(PbotVerb verb)
{
    switch (verb)
    {
        case PbotVerb::Follow:
        case PbotVerb::Come:
            // Both resume formation; Come differs only in that the owner expects it to happen from
            // wherever the bot is standing, which the normal follow re-issue in TickFollow does.
            _stay = false;
            break;

        case PbotVerb::Stay:
            _stay = true;
            _forcedAttack = false;
            // Drop the follow generator now so the bot stops on the spot instead of drifting one
            // more step toward the owner before the next tick notices.
            if (Player* bot = ResolveBot())
                if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                    bot->GetMotionMaster()->Clear();
            break;

        case PbotVerb::Attack:
            _forcedAttack = true;
            if (_stance == PbotStance::Passive)
                _stance = PbotStance::Assist;   // an explicit attack order overrides a passive stance
            break;

        case PbotVerb::Assist:
            _stance = PbotStance::Assist;
            break;

        case PbotVerb::Defend:
            _stance = PbotStance::Defend;
            break;

        case PbotVerb::Passive:
            _stance = PbotStance::Passive;
            _forcedAttack = false;
            if (Player* bot = ResolveBot())
                Disengage(bot);
            break;

        case PbotVerb::Stop:
            // Leaves the stance alone: "отбой" ends this fight, it does not make the bot pacifist.
            _forcedAttack = false;
            if (Player* bot = ResolveBot())
                Disengage(bot);
            break;

        default:
            break;   // Status / Help / None are answered by the dispatcher, not by the AI
    }
}

std::string PbotAI::DescribeState() const
{
    Player* bot = ResolveBot();
    if (!bot)
        return "Меня сейчас нет в мире.";

    char const* stance =
        _stance == PbotStance::Passive ? "не вступаю в бой" :
        _stance == PbotStance::Defend  ? "прикрываю тебя"   :
                                         "помогаю в бою";

    uint32 const healthPct = bot->GetMaxHealth() ? uint32(bot->GetHealth() * 100 / bot->GetMaxHealth()) : 0;

    std::ostringstream out;
    out << "Ур. " << uint32(bot->GetLevel())
        << ", HP " << healthPct << "%"
        << ", " << stance
        << ", " << (_stay ? "стою на месте" : "иду за тобой")
        << (bot->IsInCombat() ? ", в бою." : ".");
    return out.str();
}

void PbotAI::TickFollow(Player* bot, Player* owner, uint32 /*diff*/)
{
    // Make sure we're not still latched onto a victim from a fight that just ended. AttackStop
    // clears m_attacking + UNIT_STATE_MELEE_ATTACKING (Unit.cpp:AttackStop); dropping a lingering
    // chase generator lets MoveFollow take over cleanly.
    if (bot->GetVictim())
        Disengage(bot);

    // Phase 4A "стой": hold position. No formation movement and — deliberately — no leash snap,
    // because teleporting a bot that was told to hold its ground defeats the whole order. The bot
    // stays exactly where the owner parked it until told to follow or come.
    if (_stay)
    {
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            bot->GetMotionMaster()->Clear();
        return;
    }

    // Leash: drifted too far on the same map -> snap back beside the owner. Next tick, distance is
    // small again and the follow re-issue below reforms the line.
    if (bot->GetDistance(owner) > PBOT_LEASH_DISTANCE)
    {
        bot->GetMotionMaster()->Clear();
        bot->NearTeleportTo(owner->GetPosition());
        return;
    }

    // (Re)issue MoveFollow only when we aren't already following, so idle ticks don't thrash the
    // motion generator (same guard as Phase 1 BotAI::FollowOwner). MoveFollow is a Unit-level
    // facility (MotionMaster.h:162) and works on a Player* identically to a Creature.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        bot->GetMotionMaster()->MoveFollow(owner, PBOT_FOLLOW_DISTANCE, ChaseAngle(FollowAngleFor(_botGuid)));
}

void PbotAI::TickCombat(Player* bot, Player* owner, Unit* target, uint32 diff)
{
    // Target acquisition moved to SelectCombatTarget (Phase 4A) so stance can drive the priority;
    // by contract the caller only reaches here with a live, in-world target.
    uint8 const classId = bot->GetClass();
    bool const ranged = ClassIsRanged(classId);

    // Remember what we are fighting, so the next tick can ask whether it became a corpse of ours.
    _lastVictimGuid = target->GetGUID();

    // What the bot was facing when its health FIRST fell below half - while the fight is still on.
    //
    // The death line cannot answer this: by the time HandleDeath runs the engine has already cleared
    // the attacker set, and all 45 deaths sampled on 2026-10-22 read "0 attacker(s)". Half health is
    // late enough to be in trouble and early enough for the fight to still describe itself.
    {
        static std::unordered_map<ObjectGuid, bool> lowHealthLogged;
        bool& logged = lowHealthLogged[bot->GetGUID()];

        if (bot->GetHealthPct() > 70.0f)
            logged = false;   // recovered: the next dip is a new event

        if (!logged && bot->GetHealthPct() <= 50.0f)
        {
            logged = true;

            TC_LOG_INFO("scripts.bots", "pbot danger: {} at {:.0f}% hp, {} attacker(s), target '{}' "
                "(entry {}) at {:.0f}%, dist {:.1f}, zone {}",
                bot->GetName(), bot->GetHealthPct(), uint32(bot->getAttackers().size()),
                target->GetName(), target->GetEntry(), target->GetHealthPct(),
                bot->GetDistance(target), bot->GetZoneId());
        }
    }

    // EVERY class closes in when it is not casting.
    //
    // Ranged spell damage does not land in this build (measured per class: hunter 0/3, mage 0/9,
    // warlock 1/16, druid 7/45 targets actually hurt, while the engine reports the casts as STARTED).
    // A caster left at range therefore never hurts anything, which is the whole "bots must kill"
    // problem in one line. The staff swing is the damage source that demonstrably works, so until the
    // spell path is fixed the bots close in like everyone else — and they do NOT move while a cast is
    // running, so a cast that does land is not broken by walking.
    bool const casting = bot->HasUnitState(UNIT_STATE_CASTING);
    if (!casting && !bot->IsWithinMeleeRange(target) && !_repositionMs)
    {
        _repositionMs = 1500;   // let the leg run before re-aiming, or the bot vibrates in place

        float const angle = target->GetAbsoluteAngle(bot);
        float x = target->GetPositionX() + std::cos(angle) * 3.0f;
        float y = target->GetPositionY() + std::sin(angle) * 3.0f;
        float z = target->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        bot->GetMotionMaster()->MovePoint(0xC06, x, y, z);
    }

    // 120 degrees of attack arc: a bot that is in range but facing away never swings.
    if (!bot->HasInArc(2 * float(M_PI) / 3, target))
        bot->SetFacingToObject(target);

    // SWING FROM HERE — where the conditions are checked at the moment they are true.
    //
    // The engine's own DoMeleeAttackIfReady runs in Player::Update, and this script tick runs AFTER
    // the objects, so the engine evaluates range and facing against the position the bot had before
    // this tick moved it. When it is a hair out of range it fails, sets the attack timer to 100 ms
    // and returns — and does exactly the same on the next tick, forever. That constant is measured:
    // every stuck bot logged `atimer 100`, which is precisely that error path's value, while this
    // tick saw `melee yes, arc yes, dist 0-1y` for the same bot.
    //
    // AttackerStateUpdate is the very call DoMeleeAttackIfReady makes once its checks pass. Gating it
    // on the engine's real attack timer and resetting that timer keeps the swing rate the engine's
    // own — this is not one free extra hit per tick.
    //
    // The gate is `timer <= 100`, NOT `isAttackReady()`: 100 ms is the engine's own "swing failed,
    // try again shortly" value (Unit.cpp: `else setAttackTimer(BASE_ATTACK, 100)`), so a bot stuck at
    // 1 yard with the wrong-facing frame is pinned at 100 and isAttackReady() is never true — that is
    // exactly the state every non-swinging bot was measured in. After a real swing the timer is reset
    // to the full weapon speed, so this does not create extra swings.
    //
    // Deliberately for EVERY class, not just melee. Ranged classes in this codebase do not land spell
    // damage (measured per class: hunter 3/15, druid 2/30, mage 0/2, warlock 0/3 targets hurt; the
    // engine reports the casts as STARTED — SPELL_IN_PROGRESS — so the loss is past the cast, not in
    // it). A caster with a mob in its face swings its staff in this game, and the bots stand at 0-2
    // yards anyway, so this gives them a damage source that demonstrably works instead of leaving
    // thirteen of sixty bots unable to hurt anything.
    if (bot->getAttackTimer(BASE_ATTACK) <= 100 && bot->IsWithinMeleeRange(target)
        && bot->HasInArc(2 * float(M_PI) / 3, target))
    {
        // Instrumentation for the "in melee range, target stays at 100%" report of 2026-10-22: the bot
        // stood 0-2 yards away, melee yes, arc yes, and the creature never lost health. One swing in
        // six bots is written out, at most every three seconds, with the damage it actually did - so
        // the same line shows a healthy hit and a dead one, and no conclusion needs guessing.
        uint32 const hpBefore = target->GetHealth();

        bot->AttackerStateUpdate(target, BASE_ATTACK);
        bot->resetAttackTimer(BASE_ATTACK);

        uint32 const hpAfter = target->GetHealth();


        if (bot->GetGUID().GetCounter() % 6 == 0)
        {
            static std::unordered_map<ObjectGuid, uint32> lastSwingLog;
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint32& last = lastSwingLog[bot->GetGUID()];
            if (!last || nowMs - last >= 3000)
            {
                last = nowMs;
                TC_LOG_DEBUG("scripts.bots", "pbot damage: {} hit '{}' (entry {}) for {} (hp {} -> {} of {}), "
                    "dist {:.1f}, arc {}, melee {}, see {}, phase {}, immune {}, attackstate {}, atimer {}",
                    bot->GetName(), target->GetName(), target->GetEntry(),
                    hpBefore > hpAfter ? hpBefore - hpAfter : 0,
                    hpBefore, hpAfter, target->GetMaxHealth(),
                    bot->GetDistance(target),
                    bot->HasInArc(2 * float(M_PI) / 3, target) ? "yes" : "no",
                    bot->IsWithinMeleeRange(target) ? "yes" : "no",
                    bot->CanSeeOrDetect(target) ? "yes" : "no",
                    bot->InSamePhase(target) ? "yes" : "no",
                    target->IsImmuneToPC() ? "yes" : "no",
                    bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) ? "melee" : "not-melee",
                    bot->getAttackTimer(BASE_ATTACK));
            }
        }
    }

    // Tick the reposition throttle down once per frame; DoReposition (called on target change below
    // and again from inside the rotation via ctx.Reposition) consults it to avoid thrashing.
    if (_repositionMs > diff)
        _repositionMs -= diff;
    else
        _repositionMs = 0;

    // Set the victim + initial chase only on a genuine target change. Unit::Attack no-ops for an
    // unchanged victim; melee keep engine auto-swings (meleeAttack=true) + chase to melee, ranged set
    // the victim WITHOUT melee state (meleeAttack=false) + hold the band. Issuing the chase here (not
    // only via ctx.Reposition) covers the warrior tank, whose ladder never calls Reposition. From
    // here Player::Update -> DoMeleeAttackIfReady drives the melee swings for melee classes.
    if (bot->GetVictim() != target)
    {
        bot->Attack(target, /*meleeAttack*/ !ranged);
        DoReposition(bot, target, ranged);
    }

    // Rotation runs every tick. DoCastChecked consults the real SpellHistory (GCD + per-spell CD),
    // so attempting each tick is not spam — it fires the next ready ability the instant the GCD
    // frees. Melee auto-attack continues underneath (engine-driven); casts are layered on top.
    RunRotation(bot, owner, target, classId);
}

// --- Phase 3.2 combat rotations + positioning (DESIGN_PHASE3 SS9 PART B) ----------------------

bool PbotAI::DoCastChecked(Unit* target, uint32 spellId)
{
    if (!target)
        return false;

    Player* bot = ResolveBot();
    if (!bot)
        return false;

    // A stale/removed spell id degrades to a silent skip, never a crash (B.1 / SS8.6). Retail
    // difficulty is DIFFICULTY_NONE for open-world content — the same lookup the creature kits use.
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
    if (!spellInfo)
        return false;

    // Real engine cooldown/GCD state (B.1): Player has a live SpellHistory exactly like a Creature
    // (Unit-level), so we consult it directly instead of a hand-rolled ms map. HasGlobalCooldown
    // covers the shared GCD; HasCooldown covers this spell's own cooldown. The engine refreshes both
    // for free as part of a successful cast below.
    SpellHistory* history = bot->GetSpellHistory();
    if (history->HasGlobalCooldown(spellInfo) || history->HasCooldown(spellInfo))
        return false;

    // Normal (untriggered) cast: respects range/LoS/facing/cost and, for cast-time spells, puts the
    // bot into UNIT_STATE_CASTING for the duration - the realistic path, and the same primitive the
    // creature kits reach via UnitAI::DoCast. CastSpell is a WorldObject method, identical on a
    // Player (B.1). Packet sends it triggers are null-socket-safe (SS1).
    //
    // One flag, and it is the engine's own documented exception: every bot cast came back
    // SPELL_FAILED_BAD_TARGETS (577 of ~690 refusals, across ALL classes), the same client-less
    // target-validity gate that stopped the melee swing. Spell.cpp handles exactly that code:
    //   if (triggeredFlags & TRIGGERED_IGNORE_TARGET_CHECK && result == SPELL_FAILED_BAD_TARGETS)
    //       result = SPELL_CAST_OK;
    // So pass ONLY that flag. Cast time, cooldown, GCD and mana all stay in force — the cost of a
    // spell is unchanged, and DoCastChecked above already gates on the engine's real GCD/cooldown.
    // Two flags, and only these two.
    //
    // History: with TRIGGERED_IGNORE_TARGET_CHECK alone, SPELL_FAILED_BAD_TARGETS disappeared but no
    // damage landed. With TRIGGERED_FULL_MASK, ranged damage appeared immediately (mage 7 of 11
    // targets hurt, warlock 3 of 3) — so some flag in that mask is the one that matters. Note that
    // FULL_MASK (0x7FFFF) does NOT contain IGNORE_TARGET_CHECK (0x100000), so both are needed:
    //   IGNORE_TARGET_CHECK  — clears the client-less target-validity refusal
    //   CAST_DIRECTLY        — Spell::prepare casts without installing container state; the one flag
    //                          in the mask that plausibly governs whether the hit is APPLIED at all
    // Deliberately NOT included: IGNORE_GCD, IGNORE_SPELL_AND_CATEGORY_CD, IGNORE_POWER_COST,
    // IGNORE_REAGENT_COST, IGNORE_CAST_TIME. Spells still cost mana, still respect the GCD and their
    // cooldown, and still take their cast time — otherwise the bots would be casting for free.
    CastSpellExtraArgs castArgs(TRIGGERED_IGNORE_TARGET_CHECK | TRIGGERED_CAST_DIRECTLY);
    SpellCastResult const result = bot->CastSpell(target, spellId, castArgs);

    // Say WHY a cast was refused, once per bot per 20 s. Without the engine's own reason a refusal
    // is unknowable: range, LoS, cost and target validity all live inside CastSpell.
    if (result != SPELL_CAST_OK)
    {
        static std::unordered_map<ObjectGuid, uint32> lastReport;
        uint32 const now = GameTime::GetGameTimeMS();
        uint32& last = lastReport[bot->GetGUID()];
        if (!last || now - last >= 20000)
        {
            last = now;
            TC_LOG_INFO("scripts.bots", "pbot cast: {} spell {} refused — result {} (dist {:.0f}y, "
                "class {}, in combat {})", bot->GetName(), spellId, uint32(result),
                bot->GetDistance(target), uint32(bot->GetClass()),
                bot->IsInCombat() ? "yes" : "no");
        }
    }

    return result == SPELL_CAST_OK;
}

void PbotAI::RunRotation(Player* bot, Player* owner, Unit* target, uint8 classId)
{
    bool const ranged = ClassIsRanged(classId);

    // Healers (priest/paladin) need an ally target (B.3); everyone else leaves it null and the
    // rotation skips its heal step.
    Unit* healTarget = ClassHeals(classId) ? HealLowestAlly(bot, owner, PBOT_HEAL_RANGE) : nullptr;

    // The shared BotCombatContext (bot_rotations.h) is built fresh on the stack each tick — the same
    // accepted cost as the creature adapter (DESIGN_PHASE3 SS9 B.2). Assign fields by name so we are
    // robust to the header's declaration order.
    BotCombatContext ctx{};
    ctx.Self = bot;
    ctx.Victim = target;
    ctx.IsAoEMode = CountNearbyEnemies(bot, PBOT_AOE_RANGE) >= PBOT_AOE_MIN_ENEMIES;
    ctx.HealTarget = healTarget;
    ctx.Owner = owner;

    // TryCast targets whatever unit the ladder chose (victim / heal target / self) — the header's
    // TryCast takes an explicit target precisely so one rotation can heal an ally AND strike the
    // enemy in a single tick. cooldownMs is ignored: the real SpellHistory inside DoCastChecked
    // already tracks GCD/cooldown (B.1), so there is no hand-rolled ms budget to honor here.
    ctx.TryCast = [this](Unit* castTarget, uint32 spellId, uint32 /*cooldownMs*/) -> bool
    {
        return DoCastChecked(castTarget, spellId);
    };

    // Tanks bind SustainThreat to real threat generation; the engine ThreatManager is Unit-level, so
    // a Player generates threat through the exact same call the creature warrior kit uses. Whether a
    // Player actually HOLDS aggro this way is a smoke-test item (B.2), not a correctness risk here.
    ctx.SustainThreat = [bot, target](float amount)
    {
        bot->GetThreatManager().AddThreat(target, amount);
    };

    // Positioning is invoked from inside the ladder at the exact point the kit repositioned (B.4).
    // DoReposition is self-throttled, so calling it every tick is safe.
    ctx.Reposition = [this, bot, target, ranged]()
    {
        DoReposition(bot, target, ranged);
    };

    switch (classId)
    {
        case CLASS_WARRIOR: RunWarriorRotation(ctx); break;
        case CLASS_PALADIN: RunPaladinRotation(ctx); break;
        case CLASS_PRIEST:  RunPriestRotation(ctx);  break;
        case CLASS_MAGE:    RunMageRotation(ctx);    break;
        case CLASS_HUNTER:
            // Hunter's second AoE signal is its own threat-list size (Phase 1/2 Multi-Shot swap),
            // distinct from the nearby-count IsAoEMode above.
            RunHunterRotation(ctx, bot->GetThreatManager().GetThreatListSize() >= PBOT_AOE_MIN_ENEMIES);
            break;

        // Phase 4B: the remaining eight (bot_rotations_ext.cpp).
        case CLASS_ROGUE:        RunRogueRotation(ctx);       break;
        case CLASS_WARLOCK:      RunWarlockRotation(ctx);     break;
        case CLASS_DRUID:        RunDruidRotation(ctx);       break;
        case CLASS_SHAMAN:       RunShamanRotation(ctx);      break;
        case CLASS_MONK:         RunMonkRotation(ctx);        break;
        case CLASS_DEATH_KNIGHT: RunDeathKnightRotation(ctx); break;
        case CLASS_DEMON_HUNTER: RunDemonHunterRotation(ctx); break;
        case CLASS_EVOKER:       RunEvokerRotation(ctx);      break;

        default: break;   // no shipped kit for this class — melee auto-attack only.
    }
}

void PbotAI::DoReposition(Player* bot, Unit* target, bool ranged)
{
    // Only (re)issue MoveChase on a target change or once the throttle has elapsed — MoveChase
    // installs a fresh generator per call, so an unthrottled call every rotation tick would reset
    // movement continuously (same guard as the creature kits' KeepRangedPosition).
    if (target->GetGUID() == _chaseTargetGuid && _repositionMs > 0)
        return;

    _chaseTargetGuid = target->GetGUID();
    _repositionMs = PBOT_REPOSITION_INTERVAL_MS;

    // Ranged positioning is decided in TickCombat, not here: the band generator this used to install
    // walked casters AWAY from a mob already at 0-2 yards, which broke every cast (SPELL_FAILED_MOVING)
    // and is why ranged classes recorded no damage at all.
    if (ranged)
        return;

    bot->GetMotionMaster()->MoveChase(target);   // chase to melee range (Phase 3.1 behavior)
}

Unit* PbotAI::HealLowestAlly(Player* bot, Player* owner, float range) const
{
    // Same candidate set + selection as the Phase 2 creature BotAI::HealLowestAlly, with the pbot
    // registry (PbotMgr::GetBotsOf) swapped in for the creature-bot one (B.4): self + owner + owner's
    // group + the owner's OTHER pbots, filtered by alive & in range, minimum health%.
    Unit* best = nullptr;
    float bestPct = 101.0f;

    auto consider = [&](Unit* u)
    {
        if (!u || !u->IsAlive())
            return;
        if (bot->GetExactDist(u) > range)
            return;
        float pct = u->GetHealthPct();
        if (pct < bestPct)
        {
            bestPct = pct;
            best = u;
        }
    };

    consider(bot);   // a healer is allowed to heal itself

    if (owner)
    {
        consider(owner);

        if (Group* group = owner->GetGroup())
        {
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                if (slot.guid == owner->GetGUID())
                    continue;
                if (Player* member = ObjectAccessor::GetPlayer(*bot, slot.guid))
                    consider(member);
            }
        }

        for (ObjectGuid const& guid : PbotMgr::GetBotsOf(owner))
        {
            if (guid == bot->GetGUID())
                continue;
            if (Player* otherBot = ObjectAccessor::GetPlayer(*bot, guid))
                consider(otherBot);
        }
    }

    return best;
}

uint32 PbotAI::CountNearbyEnemies(Player* bot, float range) const
{
    // "Enemies on me" within range — the AoE-mode signal (B.2). Iterating the bot's attacker set is
    // allocation-free (a reference to the live set), unlike a grid search.
    uint32 count = 0;
    for (Unit* attacker : bot->getAttackers())
        if (attacker && attacker->IsAlive() && attacker->GetExactDist(bot) <= range)
            ++count;
    return count;
}
