/*
 * Companion Bots — speech implementation. See pbot_social.h for the two rules.
 */

#include "pbot_social.h"

#include "Cell.h"
#include "CellImpl.h"
#include "DatabaseEnv.h"       // the generated-line pool lives in `characters`
#include "Log.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"      // the pool breakdown in the log
#include "pbot_personality.h"

#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Minimum quiet time between anything a given bot says. Deliberately long: the failure mode
    // here is not "too quiet", it is a quest hub where twenty bots each announce every quest they
    // take and the zone channel becomes unreadable.
    constexpr uint32 SPEAK_COOLDOWN_MS = 180000;   // 3 minutes

    // Chance a bot with an audience and an expired cooldown actually says something. Without this
    // every bot speaks the instant its cooldown ends, and they all end up talking in lockstep.
    constexpr uint32 CHATTER_CHANCE_PCT = 25;

    // Russian, because this server's players are Russian-speaking and a bot speaking English in a
    // Russian zone is its own kind of tell.
    constexpr std::array<char const*, 10> IDLE_LINES =
    {
        "привет",
        "кто-нибудь тут есть?",
        "давно тут не был",
        "ну и глушь",
        "надо бы отдохнуть",
        "неплохо идёт",
        "кто на квесты?",
        "тут мобы злые",
        "здорово",
        "пойду дальше"
    };

    constexpr std::array<char const*, 5> QUEST_TAKEN_LINES =
    {
        "взял задание",
        "ну что, посмотрим",
        "надо сходить кое-куда",
        "есть работа",
        "займусь этим"
    };

    constexpr std::array<char const*, 5> QUEST_DONE_LINES =
    {
        "сдал, наконец",
        "готово",
        "ну вот и всё",
        "неплохая награда",
        "теперь дальше"
    };

    constexpr std::array<char const*, 5> LEVEL_LINES =
    {
        "уровень!",
        "растём",
        "ещё один уровень",
        "дело идёт",
        "уже лучше"
    };

    constexpr std::array<char const*, 5> DANGER_LINES =
    {
        "помогите!",
        "мне конец",
        "слишком много их",
        "хилла бы сюда",
        "убегаю"
    };

    template <size_t N>
    char const* Pick(std::array<char const*, N> const& lines)
    {
        return lines[urand(0, uint32(N) - 1)];
    }

    // ---------------------------------------------------------------------------------------------
    // Generated lines (the LLM pool).
    //
    // A separate program on the operator's machine reads `characters.pbot_llm_lines`, asks a local
    // model for fresh Russian lines and writes them back as `ready`. The world only ever READS that
    // table — and only when told to, never while a bot is speaking. That split is the whole point: a
    // bot's speech must not wait on a network call, because the world thread is single and a stall
    // there kills the realm (the anti-freeze watchdog). When the pool is empty — the model is off,
    // the operator's machine is asleep, the table was never filled — every call below quietly returns
    // the built-in line it was handed, and the world sounds exactly as it did before.
    struct GeneratedLine
    {
        std::string Mood;
        uint16      Band = 0;
        std::string Text;
    };

    std::unordered_map<std::string, std::vector<GeneratedLine>> _generated;

    // Level decade: a level 13 and a level 47 should not sound interchangeable.
    uint16 BandOf(Player* bot) { return uint16(bot->GetLevel() / 10); }

    // The strongest personality trait decides how the bot sounds — the same traits that already
    // drive how it fights, travels and gathers.
    char const* MoodOf(Player* bot)
    {
        PbotPersonality::Traits const& t = PbotPersonality::Of(bot->GetGUID());
        if (t.Sociability >= 65) return "chatty";
        if (t.Greed       >= 65) return "greedy";
        if (t.Caution     >= 65) return "careful";
        return "bold";
    }

    // The engine has no ready-made "any other player nearby" check (only a position-range variant),
    // so this follows the same shape as the hostile-player check the PvP module already uses.
    class AudienceCheck
    {
    public:
        AudienceCheck(Player const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Player* other) const
        {
            return other && other != _bot && other->IsInWorld() && other->IsAlive()
                && _bot->IsWithinDist(other, _range);
        }

    private:
        Player const* _bot;
        float _range;
    };

    // Is there anybody around to hear this? Any player counts — another bot is as much a reason to
    // speak as a human is, because the point is that the world SOUNDS inhabited.
    bool HasAudience(Player* bot)
    {
        Player* found = nullptr;
        AudienceCheck check(bot, PbotSocial::AUDIENCE_RANGE);
        Trinity::PlayerLastSearcher<AudienceCheck> searcher(bot, found, check);
        Cell::VisitWorldObjects(bot, searcher, PbotSocial::AUDIENCE_RANGE);

        return found != nullptr;
    }

    void SayIfQuiet(Player* bot, uint32& cooldownMs, char const* event, char const* fallback)
    {
        if (!bot || !bot->IsInWorld() || cooldownMs)
            return;

        if (!HasAudience(bot))
            return;   // never talk to an empty field

        std::string const line = PbotSocial::PickLine(bot, event, fallback);

        bot->Say(line, LANG_UNIVERSAL);
        cooldownMs = SPEAK_COOLDOWN_MS;

        // Logged from our side because the server console does not record player chat at all — the
        // first run reported "0 lines spoken" purely because there was nothing to count, which said
        // nothing about whether the bots had spoken.
        TC_LOG_INFO("scripts.bots", "pbot say: {}: {}", bot->GetName(), line);
    }
}

void PbotSocial::TickChatter(Player* bot, uint32& cooldownMs, uint32 diff)
{
    if (cooldownMs > diff)
    {
        cooldownMs -= diff;
        return;
    }
    cooldownMs = 0;

    if (!bot || !bot->IsInWorld() || bot->IsInCombat())
        return;

    // How talkative this particular bot is. A quiet one speaks about once an evening, a chatty one
    // every few minutes — which is the actual spread among real players and the reason a single
    // global chance made all sixty sound like one person.
    uint8 const sociability = PbotPersonality::Of(bot->GetGUID()).Sociability;
    if (!roll_chance(int32(PbotPersonality::Scale(sociability, float(CHATTER_CHANCE_PCT), 0.2f, 2.0f))))
        return;

    SayIfQuiet(bot, cooldownMs, "idle", Pick(IDLE_LINES));
}

void PbotSocial::OnQuestAccepted(Player* bot, uint32& cooldownMs, std::string const& /*questTitle*/)
{
    SayIfQuiet(bot, cooldownMs, "accept", Pick(QUEST_TAKEN_LINES));
}

void PbotSocial::OnQuestTurnedIn(Player* bot, uint32& cooldownMs, std::string const& /*questTitle*/)
{
    SayIfQuiet(bot, cooldownMs, "turnin", Pick(QUEST_DONE_LINES));
}

void PbotSocial::OnLevelUp(Player* bot, uint32& cooldownMs)
{
    SayIfQuiet(bot, cooldownMs, "levelup", Pick(LEVEL_LINES));
}

void PbotSocial::OnNearDeath(Player* bot, uint32& cooldownMs)
{
    SayIfQuiet(bot, cooldownMs, "neardeath", Pick(DANGER_LINES));
}

void PbotSocial::LoadGeneratedPool()
{
    _generated.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT event, mood, band, text FROM pbot_llm_lines WHERE kind = 'ready' AND text <> ''");

    uint32 rows = 0;
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            std::string event = f[0].GetString();
            std::string text  = f[3].GetString();
            if (event.empty() || text.empty())
                continue;

            GeneratedLine line;
            line.Mood = f[1].GetString();
            line.Band = uint16(f[2].GetUInt32());
            line.Text = std::move(text);
            _generated[event].push_back(std::move(line));
            ++rows;
        }
        while (result->NextRow());
    }

    // Per event, so a dry pool is visible before it is blamed on the bots being quiet.
    std::string breakdown;
    for (auto const& [event, lines] : _generated)
        breakdown += Trinity::StringFormat("{}={} ", event, uint32(lines.size()));

    if (breakdown.empty())
        TC_LOG_INFO("scripts.bots", "pbot llm: pool empty — the bots are using their built-in lines");
    else
        TC_LOG_INFO("scripts.bots", "pbot llm: {} generated lines ready ({})", rows, breakdown);
}

std::string PbotSocial::DescribeGeneratedPool()
{
    size_t total = 0;
    std::string out = "generated line pool:";
    for (auto const& [event, lines] : _generated)
    {
        total += lines.size();
        out += Trinity::StringFormat("\n  {}: {}", event, uint32(lines.size()));
    }

    if (!total)
        out += "\n  (empty — the bots are using their built-in lines)";

    return out;
}

bool PbotSocial::PoolNeedsRefill()
{
    // The five events the generator fills. A pool that was never loaded is one that "needs" filling,
    // so starting the generator at any time heals the world within one check interval.
    static constexpr char const* EVENTS[] = { "idle", "accept", "turnin", "levelup", "neardeath" };

    for (char const* event : EVENTS)
    {
        auto itr = _generated.find(event);
        if (itr == _generated.end() || itr->second.size() < 10)
            return true;
    }

    return false;
}

std::string PbotSocial::PickLine(Player* bot, char const* event, char const* fallback)
{
    if (!bot)
        return fallback;

    auto itr = _generated.find(event);
    if (itr == _generated.end() || itr->second.empty())
        return fallback;

    uint16 const band = BandOf(bot);
    char const* const mood = MoodOf(bot);

    // Score every remaining line and take a random one from the best bucket. The bot's own band and
    // mood score highest; a level-neutral line beats one for the wrong band, and a neutral mood beats
    // the wrong mood. Randomness inside the bucket is what stops two bots in one zone saying the same
    // sentence.
    int best = std::numeric_limits<int>::min();
    std::vector<size_t> bestIdx;
    for (size_t i = 0; i < itr->second.size(); ++i)
    {
        GeneratedLine const& line = itr->second[i];

        int score = 0;
        if (line.Band == band)       score += 2;
        else if (line.Band != 0)     score -= 3;   // the wrong decade is worse than no decade
        if (line.Mood == mood)       score += 2;
        else if (line.Mood != "any") score -= 1;

        if (score > best)
        {
            best = score;
            bestIdx.clear();
            bestIdx.push_back(i);
        }
        else if (score == best)
            bestIdx.push_back(i);
    }

    if (bestIdx.empty())
        return fallback;

    size_t const chosen = bestIdx[urand(0, uint32(bestIdx.size()) - 1)];
    std::string line = itr->second[chosen].Text;

    // Consumed: the same sentence is never said twice in one run. The pool refills on the next
    // `.pbot llm reload`, by which time the generator has had minutes to add more.
    itr->second.erase(itr->second.begin() + chosen);
    return line;
}
