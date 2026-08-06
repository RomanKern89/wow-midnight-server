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
#include "pbot_migrate.h"      // move to a new zone once this one is outgrown
#include "pbot_travel.h"       // stepped long-distance movement + roam points
#include "pbot_questgoal.h"    // where an unfinished quest is actually done
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

#include <algorithm>           // sorting that report by share
#include <array>
#include <atomic>              // maps tick in parallel threads; the tally is written from all of them
#include <sstream>             // DescribeState (Phase 4A status report)
#include <utility>
#include <vector>

namespace
{
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
    Position deliverable;
    bool const oweDeliveries = PbotQuestGoal::FindTurnIn(bot, deliverable);

    if (!oweDeliveries && PbotMigrate::Tick(bot, _migrateCooldownMs, diff, _home, _homeMapId))
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

    // Loot before looking for the next fight. Deliberately ahead of the hunt: in a mob-dense zone
    // the hunt always finds something and returns, so a bot that looted afterwards would never
    // loot at all — it would leave a trail of full corpses behind it, which is both un-player-like
    // and fatal to every "collect N of X" quest objective.
    if (PbotLoot::Tick(bot, _lootCorpseGuid, _lootCooldownMs, _lootWalkMs, diff))
    {
        _activity = Activity::Looting;
        return;
    }

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
        float const huntRange = PbotPersonality::Scale(self.Aggression, PbotAutonomy::SEARCH_RANGE, 0.6f, 1.4f);
        if (Unit* prey = PbotAutonomy::FindTarget(bot, huntRange))
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
    if (self.Greed >= MIN_GREED_TO_GATHER
        && PbotGather::Tick(bot, _gatherNodeGuid, _gatherCooldownMs, _gatherWaitMs, diff))
    {
        _activity = Activity::Gathering;
        return;
    }

    // Town business outranks questing: a bot with full bags cannot pick up a single quest item, so
    // going shopping first is not a detour, it is what makes the next quest possible at all.
    if (PbotUpkeep::Tick(bot, _upkeep, _home, _homeMapId, diff))
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
        _refusedQuests, diff))
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

bool PbotAI::TickTravel(Player* bot, uint32 diff)
{
    PbotPersonality::Traits const& self = PbotPersonality::Of(bot->GetGUID());

    if (_goalCooldownMs > diff)
        _goalCooldownMs -= diff;
    else
        _goalCooldownMs = 0;

    if (_hasGoal)
    {
        // Same budget as everything else that walks somewhere: a destination the pathfinder cannot
        // reach must not become a life sentence. Without this the bot re-issues a failing move
        // every tick and reports "busy", so wandering, hunting and everything else never runs.
        _goalMs += diff;
        if (_goalMs > PBOT_GOAL_WALK_BUDGET_MS)
        {
            _hasGoal = false;
            _goalMs = 0;
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

        // Arrived (or the path gave out). Make this the new home: the point of the journey was to
        // live somewhere else for a while, and leaving the anchor behind would drag the bot back.
        _home = bot->GetPosition();
        _homeMapId = bot->GetMapId();
        _hasGoal = false;
        _goalMs = 0;

        // Linger longer after reaching a quest objective than after a plain roam — the bot is now
        // standing among the creatures the quest wants, and the hunt logic needs time to work.
        _goalCooldownMs = _goalIsQuest ? PBOT_GOAL_QUEST_LINGER_MS : PBOT_GOAL_ROAM_LINGER_MS;
        return false;
    }

    if (_goalCooldownMs)
        return false;

    // FINISHED work first, before going looking for more. A player who has killed the eight wolves
    // walks back and collects the reward; only then do they ask what else needs doing.
    //
    // This ordering is not a nicety, it is the difference between questing and hoarding: measured,
    // the population was holding 255 COMPLETED quests and handing in roughly one every half hour.
    // The log fills, the soft cap stops new quests being taken, and the bot spends the rest of its
    // life carrying finished errands it never delivers. Nobody chases the reward harder than the
    // person who already earned it, so this one is not gated on diligence.
    if (PbotQuestGoal::FindTurnIn(bot, _goal))
    {
        _hasGoal = true;
        _goalIsQuest = true;
        _noQuestMs = 0;
        return true;
    }

    // An unfinished quest is a real reason to be somewhere; roaming is what a player does when
    // they have none. A diligent bot always goes and does the quest; a slack one often cannot be
    // bothered and wanders off to kill things instead, which is a recognisable kind of player.
    if (roll_chance(int32(PbotPersonality::Scale(self.Diligence, 100.0f, 0.35f, 1.0f)))
        && PbotQuestGoal::Find(bot, _goal))
    {
        _hasGoal = true;
        _goalIsQuest = true;
        _noQuestMs = 0;
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
            _goalCooldownMs = PBOT_GOAL_QUEST_LINGER_MS;   // settle in before deciding anything
            return true;
        }
    }

    if (PbotTravel::PickRoamPoint(bot, _goal))
    {
        _hasGoal = true;
        _goalIsQuest = false;
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
    // bot into UNIT_STATE_CASTING for the duration — the realistic path, and the same primitive the
    // creature kits reach via UnitAI::DoCast. CastSpell is a WorldObject method, identical on a
    // Player (B.1). Packet sends it triggers are null-socket-safe (SS1).
    return bot->CastSpell(target, spellId) == SPELL_CAST_OK;
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

    if (ranged)
        bot->GetMotionMaster()->MoveChase(target, ChaseRange(PBOT_RANGED_CHASE_MIN, PBOT_RANGED_CHASE_MAX));
    else
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
