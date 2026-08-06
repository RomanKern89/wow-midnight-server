/*
 * Companion Bots — PbotAI (fake-player bots, TrinityCore master, retail 12.0.7).
 *
 * Per-bot decision loop driven once per world tick from PbotWorldScript::OnUpdate
 * (DESIGN_PHASE3 SS8.2). PbotAI is our OWN class — deliberately NOT a PlayerAI subclass —
 * because the engine's generic AI auto-dispatch structurally excludes TYPEID_PLAYER
 * (DESIGN_PHASE3 SS3 / forbidden-list rule 6): we drive every decision explicitly instead.
 *
 * Phase 3.2 extends the frozen SS8.3 interface with class combat rotations + positioning
 * (DESIGN_PHASE3 SS9 PART B), driving combat through the shared bot_rotations.h ladders that the
 * creature kits also use. The added private members below are that spec change:
 *   - DoCastChecked: a SpellHistory-backed guarded cast (B.1) — real engine GCD/cooldown, no
 *     hand-rolled ms map (unlike the Phase 1/2 creature BotAI::DoCastChecked).
 *   - RunRotation: builds a BotCombatContext (bot_rotations.h) — binding TryCast/SustainThreat/
 *     Reposition to our own primitives, and setting Owner/HealTarget — then dispatches to the
 *     matching Run*Rotation by the bot's class.
 *   - DoReposition / HealLowestAlly / CountNearbyEnemies: the B.4 positioning + healer-target +
 *     AoE-signal helpers, reimplemented on Player* using the same Unit-level APIs the creature kits
 *     use. Positioning is invoked from inside the rotation via ctx.Reposition().
 * The bot's class is resolved fresh via bot->GetClass() each tick (no stored field, no ctor
 * change — keeps PbotMgr's construction untouched).
 */

#pragma once
#include "ObjectGuid.h"
#include "Position.h"    // home anchor for ownerless world bots (Phase 6)
#include "pbot_bg.h"     // PbotBgState — per-bot battleground objective state
#include "pbot_chat.h"   // PbotVerb — the chat layer drives this AI through ApplyCommand (Phase 4A)
#include "pbot_upkeep.h" // PbotUpkeep::State — the bot's vendor/repair errand

#include <string>
#include <unordered_set>

class Player;
class Unit;

// How willing the bot is to enter a fight (Phase 4A). Orthogonal to the follow/stay movement mode:
// a Passive bot still keeps formation, a Staying bot still defends itself.
enum class PbotStance : uint8
{
    Assist = 0,   // default — fight whatever the owner is fighting
    Defend,       // prioritise whatever is attacking the OWNER over the owner's own target
    Passive       // never engage; disengage on entry
};

class PbotAI
{
public:
    explicit PbotAI(ObjectGuid botGuid, ObjectGuid ownerGuid) : _botGuid(botGuid), _ownerGuid(ownerGuid) { }

    // Called once per world tick from PbotWorldScript::OnUpdate (SS8.2). Re-resolves both the bot
    // and the owner from ObjectAccessor every call - never caches Player pointers across ticks,
    // same discipline as Phase 1 BotAI owner lookup (Phase 1 SS2, "Owner lookup each tick").
    void UpdateBot(uint32 diff);

    // --- Phase 4A: chat-driven control -------------------------------------------------------
    // Applies a parsed chat verb. Called from the chat dispatcher on the world tick that handled
    // the owner's chat packet, i.e. never concurrently with UpdateBot. Only latches state and, for
    // the one-shot disengage verbs, issues an immediate AttackStop — all movement/targeting
    // consequences land on the next UpdateBot so there is exactly one place that drives the bot.
    void ApplyCommand(PbotVerb verb);

    // One-line human-readable state for the "статус" verb: level, health, stance, movement mode.
    // Returns a fallback string rather than failing if the bot is not resolvable this tick.
    std::string DescribeState() const;

    // --- Phase 6: ownerless world bots --------------------------------------------------------

    // A world bot is exactly "a bot with no owner". Everything else about it — the autonomous
    // behaviour, the home anchor, the self-resurrect — follows from there.
    bool IsWorldBot() const { return _ownerGuid.IsEmpty(); }

    // Anchor the bot's wandering. Set once by the spawn path; a world bot with no home set would
    // wander around the map origin, which is nowhere.
    //
    // The map id is stored with it because the anchor is only meaningful on the map it was taken
    // from. A bot that entered a battleground kept walking toward its Elwynn Forest coordinates on
    // the battleground map and marched straight off the playable area — measured at ~450 yards a
    // minute, which is why bots in Warsong Gulch never found a flag.
    void SetHome(Position const& home, uint32 mapId) { _home = home; _homeMapId = mapId; }

private:
    Player* ResolveBot() const;     // ObjectAccessor::FindPlayer(_botGuid) - global lookup, since
                                     // the bot map can differ transiently right after owner teleports
    Player* ResolveOwner() const;   // ObjectAccessor::FindPlayer(_ownerGuid)

    void TickFollow(Player* bot, Player* owner, uint32 diff);
    void TickCombat(Player* bot, Player* owner, Unit* target, uint32 diff);

    // --- Phase 4A helpers ---------------------------------------------------------------------

    // Stance-aware engagement decision + target pick, replacing Phase 3.2's unconditional
    // "owner is in combat -> assist". Returns nullptr when the bot should not be fighting at all
    // this tick. Defend flips the priority to the owner's attackers; Stay only lets the bot fight
    // what is already hitting it; Passive never returns a target.
    Unit* SelectCombatTarget(Player* bot, Player* owner) const;

    // Drops victim + chase generator and resets the reposition throttle. Extracted because three
    // call sites (fight ended, Stop verb, Passive stance) need exactly this.
    void Disengage(Player* bot);

    // The whole update path for an ownerless bot: die/recover, fight, rest, hunt, wander. Kept
    // separate from the companion path rather than threading null-owner checks through it, because
    // the two answer different questions — "what does my owner need?" versus "what would a player
    // standing here do next?".
    void UpdateAutonomous(Player* bot, uint32 diff);

    // --- Phase 3.2 combat rotations + positioning (DESIGN_PHASE3 SS9 PART B) ------------------

    // Guarded cast (B.1). Verifies the spell exists on this build, then consults the bot's real
    // SpellHistory for GCD + per-spell cooldown, then casts. A stale/removed spell id degrades to a
    // silent skip, never a crash. Returns true only when the cast actually started. Bound into
    // BotCombatContext::TryCast (target explicit, cooldownMs ignored — the engine tracks it).
    bool DoCastChecked(Unit* target, uint32 spellId);

    // Builds the shared BotCombatContext and dispatches to the matching Run*Rotation (B.2/B.3).
    void RunRotation(Player* bot, Player* owner, Unit* target, uint8 classId);

    // Class-gated chase, bound into ctx.Reposition() and issued on target change (B.4): ranged
    // (mage/hunter/priest) hold the 20-30y band via MoveChase(ChaseRange); melee chase to melee.
    // Self-throttled (target guid + interval) so calling it every rotation tick never thrashes the
    // motion generator — same pattern as the creature kits' KeepRangedPosition.
    void DoReposition(Player* bot, Unit* target, bool ranged);

    // Lowest-HP living ally within range across self + owner + owner's group + owner's OTHER pbots
    // (B.4), for the priest/paladin heal step.
    Unit* HealLowestAlly(Player* bot, Player* owner, float range) const;

    // Living enemies attacking the bot within range — the AoE-mode signal (B.2 IsAoEMode). Iterates
    // the bot's attacker set with no allocation.
    uint32 CountNearbyEnemies(Player* bot, float range) const;

    ObjectGuid _botGuid;
    ObjectGuid _ownerGuid;
    ObjectGuid _chaseTargetGuid;     // last target we issued a chase toward (DoReposition throttle)
    uint32 _repositionMs = 0;        // ms until the chase generator may be re-issued (DoReposition
                                     // throttle). Rotation casts are NOT throttled — DoCastChecked
                                     // gates them on the real engine GCD/cooldown instead.

    // --- Phase 4A chat-driven state -----------------------------------------------------------
    PbotStance _stance = PbotStance::Assist;
    bool _stay = false;              // "стой": hold position, do not keep formation
    bool _forcedAttack = false;      // "в бой": one-shot override that engages the owner's target
                                     // even while Staying. Cleared once that target is gone.

    // --- Phase 6 world-bot state ---------------------------------------------------------------
    Position _home;                  // anchor the random walk stays near (world bots only)
    uint32 _homeMapId = 0;           // the map _home was taken on; the anchor is meaningless elsewhere
    uint32 _lastMapId = 0;           // map the bot was on last tick — used to detect a map change
    uint32 _deathTimerMs = 0;        // counts down to the on-the-spot resurrect
    uint8 _consecutiveDeaths = 0;    // deaths in quick succession — the signature of a death loop
    uint32 _sinceLastDeathMs = 0;    // time survived since the last one
    uint32 _retreatMs = 0;           // non-zero while breaking off a fight that is already lost
    bool _resting = false;           // sitting to regenerate; hysteresis against the two health
                                     // thresholds so the bot does not stand/sit every other tick
    ObjectGuid _gatherNodeGuid;      // node currently being walked to (Phase 5 gathering)
    uint32 _gatherCooldownMs = 0;    // gate on how often the node search runs
    uint32 _gatherWaitMs = 0;        // non-zero while standing at a used node waiting for its loot

    ObjectGuid _questGiverGuid;      // questgiver currently being walked to (questing)
    uint32 _questCooldownMs = 0;     // gate on how often the questgiver search runs

    ObjectGuid _lootCorpseGuid;      // corpse currently being walked to (looting)
    uint32 _lootCooldownMs = 0;      // gate on how often the corpse search runs

    uint32 _migrateCooldownMs = 0;   // gate on the "have I outgrown this zone" check

    Position _goal;                  // where the bot is currently walking (quest objective or roam)
    bool _hasGoal = false;
    bool _goalIsQuest = false;
    uint32 _goalCooldownMs = 0;      // gate on picking the next destination
    uint32 _goalMs = 0;              // time spent walking to the current destination (give-up budget)
    uint32 _questWalkMs = 0;         // time spent walking to the chosen questgiver
    uint32 _lootWalkMs = 0;          // time spent walking to the chosen corpse
    uint32 _noQuestMs = 0;           // how long this bot has had no quest to pursue (hearth timer)

    uint32 _speakCooldownMs = 0;     // quiet time between anything this bot says
    uint32 _partyCooldownMs = 0;     // gate on looking for someone to group with
    uint8 _lastKnownLevel = 0;       // to notice a level-up without a hook

    PbotUpkeep::State _upkeep;       // the town errand: who is being walked to, and where to
    uint32 _marketCooldownMs = 0;    // gate on checking the mail and the auction house
    uint32 _craftCooldownMs = 0;     // gate on practising the bot's profession
    uint32 _workbenchMs = 0;         // walking to a forge; nothing else may divert the bot until 0
    uint32 _professionSetupMs = 0;   // granting the craft and learning recipes, above the ladder
    // Quests the engine declined to actually add for this bot, so it stops walking back to ask.
    std::unordered_set<uint32> _refusedQuests;

public:
    // What a bot was doing with a tick. Counted across the whole population so the question "where
    // does the day go" has an answer made of numbers.
    //
    // The stages are exclusive by construction: the decision loop is a ladder of early returns, so
    // exactly one of these claims each tick, and the tally IS the time budget. Asked for after the
    // crafting chain finally worked end to end and produced two items in forty minutes — with the
    // world healthy and the bots busy, "busy with WHAT" became the only question left.
    enum class Activity : uint8
    {
        Settling, Dead, Battleground, Retreat, Combat, Resting, Looting,
        Hunting, Gathering, Upkeep, Market, Workbench, Crafting, Questing,
        Travelling, Roaming, Idle,
        Count
    };

    static void ResetActivityBudget();
    static std::string DescribeActivityBudget();

private:
    Activity _activity = Activity::Idle;

    // Walks toward the current destination, choosing a new one when there is none. Returns true
    // while travelling. This is what replaced jittering around the spawn point.
    bool TickTravel(Player* bot, uint32 diff);

    // Gate on the idle target search. FindTarget/FindHostilePlayer each walk grid cells, and an
    // idle bot would otherwise run both on EVERY world tick — at the population sizes this feature
    // exists for (dozens of bots) that is hundreds of grid searches a second for nothing. A bot
    // that notices an enemy up to a second late is indistinguishable from one that does not.
    uint32 _huntCooldownMs = 0;

    // --- Phase 7 PvP ---------------------------------------------------------------------------
    // Set by the duel-request hook, consumed on the next tick (accepting from inside the duel
    // spell's own effect handler would mutate both duellists mid-execution — see pbot_pvp.h).
    bool _duelPending = false;

    // Battleground objective state: search throttle plus the remembered flag position. See
    // PbotBgState — the search has to cover most of the map, so it runs rarely and caches.
    PbotBgState _bgState;

public:
    // Called from the OnDuelRequest script hook; the accept itself happens on the next AI tick.
    void NoteDuelChallenge() { _duelPending = true; }
};
