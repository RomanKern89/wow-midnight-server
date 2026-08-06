/*
 * Companion Bots — speech implementation. See pbot_social.h for the two rules.
 */

#include "pbot_social.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Log.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Random.h"
#include "pbot_personality.h"

#include <array>
#include <string>

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

    void SayIfQuiet(Player* bot, uint32& cooldownMs, char const* line)
    {
        if (!bot || !bot->IsInWorld() || cooldownMs)
            return;

        if (!HasAudience(bot))
            return;   // never talk to an empty field

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

    SayIfQuiet(bot, cooldownMs, Pick(IDLE_LINES));
}

void PbotSocial::OnQuestAccepted(Player* bot, uint32& cooldownMs, std::string const& /*questTitle*/)
{
    SayIfQuiet(bot, cooldownMs, Pick(QUEST_TAKEN_LINES));
}

void PbotSocial::OnQuestTurnedIn(Player* bot, uint32& cooldownMs, std::string const& /*questTitle*/)
{
    SayIfQuiet(bot, cooldownMs, Pick(QUEST_DONE_LINES));
}

void PbotSocial::OnLevelUp(Player* bot, uint32& cooldownMs)
{
    SayIfQuiet(bot, cooldownMs, Pick(LEVEL_LINES));
}

void PbotSocial::OnNearDeath(Player* bot, uint32& cooldownMs)
{
    SayIfQuiet(bot, cooldownMs, Pick(DANGER_LINES));
}
