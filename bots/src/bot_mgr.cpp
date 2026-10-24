/*
 * Companion Bots — BotMgr implementation (TrinityCore master, retail 12.0.7).
 * Registry + summon ordering + level sync + owner-event handlers. See DESIGN.md SS2/SS5/SS10.
 */

#include "bot_mgr.h"
#include "bot_ai.h"
#include "Creature.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "ObjectAccessor.h"
#include "Map.h"
#include "GameTime.h"
#include <cmath>
#include <unordered_map>

namespace
{
    // One registry slot: the summoned creature plus the class we summoned it as (needed to
    // resummon on a map change, when the old creature is no longer reachable).
    struct BotSlot
    {
        ObjectGuid guid;
        BotClass   botClass;
    };

    // owner guid -> that player's bots. Process-wide, single-threaded world update.
    std::unordered_map<ObjectGuid, std::vector<BotSlot>> _registry;

    // Phase 2 anti-double-count guard: owner guid -> (victim guid -> ms timestamp of last credit).
    // Pruned lazily on lookup; bounded in practice since kills are infrequent vs the TTL.
    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> _recentCreditedKills;

    // Gold-share tunables (DESIGN_PHASE2 SS4a).
    constexpr uint32 BOT_KILL_CREDIT_TTL_MS = 5000;  // suppress a duplicate credit within this window
    constexpr int64  BOT_GOLD_PER_LEVEL     = 5;     // flat "small gold" per victim level

    // Review finding #1: owner guid -> (victim guid -> ms of last engagement note). Written by
    // BotAI (JustEngagedWith + throttled refresh), read by OnCreatureKill via
    // WasBotEngagedRecently, since the victim's threat list is empty by the time that hook fires.
    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> _recentEngagements;
    constexpr uint32 BOT_ENGAGEMENT_TTL_MS = 10000;

    uint32 EntryForClass(BotClass botClass)
    {
        switch (botClass)
        {
            case BOT_CLASS_WARRIOR: return BOT_ENTRY_WARRIOR;
            case BOT_CLASS_PALADIN: return BOT_ENTRY_PALADIN;
            case BOT_CLASS_PRIEST:  return BOT_ENTRY_PRIEST;
            case BOT_CLASS_MAGE:    return BOT_ENTRY_MAGE;
            case BOT_CLASS_HUNTER:  return BOT_ENTRY_HUNTER;
            default:                return 0;
        }
    }

    void DespawnByGuid(WorldObject const& ref, ObjectGuid guid)
    {
        if (Creature* bot = ObjectAccessor::GetCreature(ref, guid))
            bot->DespawnOrUnsummon();
    }
}

bool BotMgr::AddBot(Player* owner, BotClass botClass, std::string& err)
{
    if (!owner)
    {
        err = "No player.";
        return false;
    }

    uint32 entry = EntryForClass(botClass);
    if (!entry)
    {
        err = "Unknown bot class.";
        return false;
    }

    std::vector<BotSlot>& bots = _registry[owner->GetGUID()];
    if (bots.size() >= MAX_BOTS_PER_PLAYER)
    {
        err = "You already have the maximum number of companion bots.";
        return false;
    }

    uint8 slot = uint8(bots.size());
    float angle = BOT_FOLLOW_ANGLES[slot % MAX_BOTS_PER_PLAYER];

    // Offset the spawn so multiple bots don't stack on the owner (DESIGN SS2).
    Position pos = owner->GetPosition();
    pos.RelocateOffset(Position(std::cos(angle) * BOT_SUMMON_SPREAD, std::sin(angle) * BOT_SUMMON_SPREAD, 0.0f));

    TempSummon* bot = owner->SummonCreature(entry, pos, TEMPSUMMON_MANUAL_DESPAWN);
    if (!bot)
    {
        err = "Failed to summon bot creature.";
        return false;
    }

    // Exact ordering mandated by DESIGN SS2.
    bot->SetOwnerGUID(owner->GetGUID());
    bot->SetFaction(owner->GetFaction());
    bot->SetLevel(owner->GetLevel());
    bot->UpdateLevelDependantStats();
    bot->SetReactState(REACT_DEFENSIVE);

    BotAI* ai = dynamic_cast<BotAI*>(bot->AI());
    if (!ai)
    {
        // ScriptName not registered / wrong AI attached — don't leak a dumb creature.
        bot->DespawnOrUnsummon();
        err = "Bot AI is not registered (check ScriptName and that the bot scripts are built in).";
        return false;
    }

    ai->InitializeBot(owner->GetGUID(), slot);
    bots.push_back({ bot->GetGUID(), botClass });
    return true;
}

bool BotMgr::RemoveBot(Player* owner)
{
    if (!owner)
        return false;

    auto it = _registry.find(owner->GetGUID());
    if (it == _registry.end() || it->second.empty())
        return false;

    ObjectGuid guid = it->second.back().guid;
    it->second.pop_back();
    DespawnByGuid(*owner, guid);

    if (it->second.empty())
        _registry.erase(it);
    return true;
}

void BotMgr::RemoveAllBots(Player* owner)
{
    if (!owner)
        return;

    auto it = _registry.find(owner->GetGUID());
    if (it == _registry.end())
        return;

    for (BotSlot const& s : it->second)
        DespawnByGuid(*owner, s.guid);
    _registry.erase(it);
}

std::vector<ObjectGuid> BotMgr::GetBots(ObjectGuid ownerGuid)
{
    std::vector<ObjectGuid> result;
    auto it = _registry.find(ownerGuid);
    if (it != _registry.end())
    {
        result.reserve(it->second.size());
        for (BotSlot const& s : it->second)
            result.push_back(s.guid);
    }
    return result;
}

std::vector<ObjectGuid> BotMgr::GetBots(Player* owner)
{
    if (!owner)
        return {};
    return GetBots(owner->GetGUID());
}

uint32 BotMgr::GetBotCount(Player* owner)
{
    if (!owner)
        return 0;
    auto it = _registry.find(owner->GetGUID());
    return it != _registry.end() ? uint32(it->second.size()) : 0;
}

void BotMgr::OnOwnerLevelChanged(Player* owner)
{
    if (!owner)
        return;

    auto it = _registry.find(owner->GetGUID());
    if (it == _registry.end())
        return;

    for (BotSlot const& s : it->second)
    {
        if (Creature* bot = ObjectAccessor::GetCreature(*owner, s.guid))
        {
            bot->SetLevel(owner->GetLevel());
            bot->UpdateLevelDependantStats();   // recompute hp/mana/damage/armor (DESIGN SS5)

            // Re-apply Phase 2 gear illusion on top of the freshly-leveled base stats: item-level
            // power scaling + weapon visuals for the new bracket (DESIGN_PHASE2 SS4c).
            if (BotAI* ai = dynamic_cast<BotAI*>(bot->AI()))
                ai->ApplyGearIllusion();
        }
    }
}

void BotMgr::OnOwnerMapChanged(Player* owner)
{
    if (!owner)
        return;

    auto it = _registry.find(owner->GetGUID());
    if (it == _registry.end() || it->second.empty())
        return;

    // Snapshot the classes, drop the old registry slots, then resummon fresh on the new map.
    // The old-map creatures self-despawn via BotAI::TickLeash (map mismatch) — they are not
    // reachable from the owner's new map for a direct despawn here (DESIGN SS2, risk #4).
    std::vector<BotClass> classes;
    classes.reserve(it->second.size());
    for (BotSlot const& s : it->second)
        classes.push_back(s.botClass);
    _registry.erase(it);

    std::string err;
    for (BotClass c : classes)
        AddBot(owner, c, err);   // best-effort; ignore per-bot failure
}

void BotMgr::OnOwnerLogout(Player* owner)
{
    RemoveAllBots(owner);
    if (owner)
    {
        // Review finding #3: without these erases every player who ever earned a credit (or had a
        // bot engage anything) leaves a permanent outer-map entry for the process lifetime.
        _recentCreditedKills.erase(owner->GetGUID());
        _recentEngagements.erase(owner->GetGUID());
    }
}

void BotMgr::NoteBotEngagement(ObjectGuid ownerGuid, ObjectGuid victimGuid)
{
    if (ownerGuid.IsEmpty() || victimGuid.IsEmpty())
        return;

    uint32 now = GameTime::GetGameTimeMS();
    auto& engaged = _recentEngagements[ownerGuid];

    // Same lazy pruning discipline as the credit map: drop stale victims so the per-owner map
    // stays bounded by "victims fought in the last 10s".
    for (auto it = engaged.begin(); it != engaged.end(); )
    {
        if (now - it->second > BOT_ENGAGEMENT_TTL_MS)
            it = engaged.erase(it);
        else
            ++it;
    }

    engaged[victimGuid] = now;
}

bool BotMgr::WasBotEngagedRecently(ObjectGuid ownerGuid, ObjectGuid victimGuid)
{
    auto ownerIt = _recentEngagements.find(ownerGuid);
    if (ownerIt == _recentEngagements.end())
        return false;

    auto victimIt = ownerIt->second.find(victimGuid);
    if (victimIt == ownerIt->second.end())
        return false;

    uint32 now = GameTime::GetGameTimeMS();
    bool fresh = (now - victimIt->second) <= BOT_ENGAGEMENT_TTL_MS;

    // Consume the note either way — one credit per death, and dead guids must not linger.
    ownerIt->second.erase(victimIt);
    if (ownerIt->second.empty())
        _recentEngagements.erase(ownerIt);

    return fresh;
}

void BotMgr::CreditGoldForKill(Player* owner, Creature const* victim)
{
    if (!owner || !victim)
        return;

    ObjectGuid victimGuid = victim->GetGUID();
    uint32 now = GameTime::GetGameTimeMS();
    auto& recent = _recentCreditedKills[owner->GetGUID()];

    // Prune stale entries so the per-owner map stays bounded, then apply the TTL guard.
    for (auto it = recent.begin(); it != recent.end(); )
    {
        if (now - it->second >= BOT_KILL_CREDIT_TTL_MS)
            it = recent.erase(it);
        else
            ++it;
    }
    if (auto it = recent.find(victimGuid); it != recent.end())
        return;   // already credited this victim within the TTL — defense-in-depth (SS4a)
    recent[victimGuid] = now;

    int64 bonus = int64(victim->GetLevel()) * BOT_GOLD_PER_LEVEL;
    owner->ModifyMoney(bonus);
}
