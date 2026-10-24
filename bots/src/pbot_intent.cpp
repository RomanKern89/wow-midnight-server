/*
 * Companion Bots — intent arbitration. See pbot_intent.h for why this exists.
 *
 * The scores below are deliberately cheap: every feature they read is already in the bot's own
 * object, and the expensive one (is there a taker I could actually reach?) is asked once per Decide,
 * which the caller runs on a timer rather than every tick.
 */

#include "pbot_intent.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"
#include "pbot_personality.h"
#include "pbot_questgoal.h"

#include <array>
#include <cmath>
#include <unordered_map>

namespace
{
    // How long an answer is trusted before the scores are recomputed. Long enough to be a plan (a
    // trip takes minutes), short enough to notice that the situation changed.
    constexpr uint32 DECIDE_INTERVAL_MS = 30000;

    // A rival has to beat the current intent by this much to take over. Without it two near-equal
    // options trade places every thirty seconds and the bot does neither.
    constexpr float SWITCH_MARGIN = 1.15f;

    constexpr float WEIGHT_MIN = 0.25f;
    constexpr float WEIGHT_MAX = 3.00f;

    constexpr size_t KIND_COUNT = size_t(PbotIntent::Kind::Count);

    // The same soft cap the quest module keeps (pbot_quest.cpp): the log is held below its true size
    // on purpose, so a quest the bot CAN finish is never blocked by a pile of ones it cannot.
    constexpr uint32 SOFT_QUEST_SLOTS = 20;

    using WeightSet = std::array<float, KIND_COUNT>;

    // Profiles from the generator, by bot. Absent means "use the personality defaults below".
    std::unordered_map<ObjectGuid, WeightSet> _profiles;

    // Stable per-bot jitter. Derived from the guid, so the same bot always has the same small bias
    // and sixty bots with identical traits still differ — but none of them changes its mind at random.
    float Jitter(ObjectGuid guid, PbotIntent::Kind kind)
    {
        uint64 const mixed = uint64(guid.GetCounter()) * 1000003ull + uint64(kind) * 2654435761ull;
        return 0.9f + float((mixed >> 13) % 21) / 100.0f;   // 0.90 .. 1.10
    }

    // What the bot's character says its priorities are. Read as multipliers on the shared scores, so
    // the tuning above still holds and personality only spreads bots around it.
    WeightSet DefaultWeights(Player* bot)
    {
        PbotPersonality::Traits const& t = PbotPersonality::Of(bot->GetGUID());

        WeightSet w;
        w.fill(1.0f);

        // A diligent bot finishes what it started and goes looking for more; a slack one does neither.
        w[size_t(PbotIntent::Kind::AdvanceQuest)]    = PbotPersonality::Scale(t.Diligence, 1.0f, 0.6f, 1.6f);
        w[size_t(PbotIntent::Kind::TakeWork)]        = PbotPersonality::Scale(t.Diligence, 1.0f, 0.7f, 1.4f);

        // A greedy one gathers and goes to market; a cautious one keeps its armour and its bags ready.
        w[size_t(PbotIntent::Kind::GatherCraft)]     = PbotPersonality::Scale(t.Greed, 1.0f, 0.6f, 1.7f);
        w[size_t(PbotIntent::Kind::SupplyAndRepair)] = PbotPersonality::Scale(t.Caution, 1.0f, 0.8f, 1.5f);

        // A wanderer goes to look at things.
        w[size_t(PbotIntent::Kind::Explore)]         = PbotPersonality::Scale(t.Wanderlust, 1.0f, 0.5f, 1.8f);

        return w;
    }

    float Score(PbotIntent::Kind kind, Player* bot, uint32 finished, uint32 unfinished,
                uint32 usedSlots, bool takerKnown)
    {
        switch (kind)
        {
            case PbotIntent::Kind::DeliverFinished:
                // The reward leg. A finished quest the bot can actually deliver is the most useful
                // thing it owns: it pays, and it frees a log slot for work it could take next.
                return finished ? (2.0f + (takerKnown ? 1.5f : 0.0f)) : 0.0f;

            case PbotIntent::Kind::AdvanceQuest:
                return unfinished ? 1.6f : 0.0f;

            case PbotIntent::Kind::TakeWork:
                // Only worth walking to a questgiver with room to take something. Asking with a full
                // log is the walk that ends in AcceptAt declining everything.
                return usedSlots < SOFT_QUEST_SLOTS ? 0.9f : 0.1f;

            case PbotIntent::Kind::SupplyAndRepair:
                // Bags nearly full is the one condition that blocks everything else, so it is the one
                // that scales sharply.
                return bot->GetFreeInventorySlotCount() < 4 ? 2.4f : 0.3f;

            case PbotIntent::Kind::GatherCraft:
                return 0.9f;

            case PbotIntent::Kind::Explore:
                return 0.5f;

            default:
                return 0.0f;
        }
    }
}

char const* PbotIntent::Name(Kind kind)
{
    switch (kind)
    {
        case Kind::DeliverFinished: return "deliver finished work";
        case Kind::AdvanceQuest:    return "advance a quest";
        case Kind::TakeWork:        return "find work";
        case Kind::SupplyAndRepair: return "town business";
        case Kind::GatherCraft:     return "gather and craft";
        case Kind::Explore:         return "see the zone";
        default:                    return "idle";
    }
}

void PbotIntent::LoadProfiles()
{
    _profiles.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, deliver, advance, takework, supply, gather, explore "
        "FROM pbot_llm_profiles");

    uint32 rows = 0;
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(f[0].GetUInt64());

            WeightSet w;
            for (size_t i = 0; i < KIND_COUNT; ++i)
                w[i] = std::clamp(f[i + 1].GetFloat(), WEIGHT_MIN, WEIGHT_MAX);

            _profiles[guid] = w;
            ++rows;
        }
        while (result->NextRow());
    }

    if (rows)
        TC_LOG_INFO("scripts.bots", "pbot intent: {} generated profiles loaded", rows);
    else
        TC_LOG_INFO("scripts.bots", "pbot intent: no profiles — bots use their personality priorities");
}

float PbotIntent::Weight(ObjectGuid guid, Kind kind)
{
    auto itr = _profiles.find(guid);
    if (itr != _profiles.end())
        return itr->second[size_t(kind)];

    return 1.0f;   // the caller multiplies in the personality defaults
}

PbotIntent::Kind PbotIntent::Decide(Player* bot, Kind current, uint32& cooldownMs, uint32 diff)
{
    if (!bot || !bot->IsInWorld())
        return current;

    if (cooldownMs > diff)
    {
        cooldownMs -= diff;
        return current;
    }
    cooldownMs = DECIDE_INTERVAL_MS;

    uint32 finished = 0;
    uint32 unfinished = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
            ++finished;
        else if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            ++unfinished;
    }

    // Is there a taker this bot could actually walk to? The one expensive question here, asked once
    // per decide rather than per tick — see PbotQuestGoal::FindTurnIn for what "could walk to" means.
    Position taker;
    bool const takerKnown = finished > 0 && PbotQuestGoal::FindTurnIn(bot, taker);

    // No profile for this bot: its personality is the profile.
    WeightSet const personality = DefaultWeights(bot);

    Kind best = Kind::Explore;
    float bestScore = -1.0f;
    float currentScore = 0.0f;

    for (size_t i = 0; i < KIND_COUNT; ++i)
    {
        Kind const kind = Kind(i);

        float const profile = Weight(bot->GetGUID(), kind);
        float const score = Score(kind, bot, finished, unfinished, finished + unfinished, takerKnown)
            * personality[i] * profile * Jitter(bot->GetGUID(), kind);

        if (kind == current)
            currentScore = score;

        if (score > bestScore)
        {
            bestScore = score;
            best = kind;
        }
    }

    // Keep what the bot is already doing unless a rival wins clearly. This is what makes the answer a
    // plan: a bot that set out for town reaches town.
    if (current != Kind::Count && currentScore * SWITCH_MARGIN >= bestScore)
        return current;

    return best;
}

std::string PbotIntent::Describe()
{
    std::string out = Trinity::StringFormat("intent profiles loaded: {}", uint32(_profiles.size()));
    if (_profiles.empty())
        out += "\n  (none — every bot is using its own personality, which is the default)";
    return out;
}

std::string PbotIntent::DescribeBot(Player* bot)
{
    if (!bot)
        return "no bot";

    uint32 finished = 0;
    uint32 unfinished = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
            ++finished;
        else if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            ++unfinished;
    }

    return Trinity::StringFormat("{}: {} finished, {} unfinished, {} free bag slots, profile {}",
        bot->GetName(), finished, unfinished, uint32(bot->GetFreeInventorySlotCount()),
        _profiles.count(bot->GetGUID()) ? "yes" : "no");
}
