/*
 * Companion Bots — Phase 4A chat dispatcher (TrinityCore master, retail 12.0.7).
 *
 * Hooks the OWNER's outgoing chat (PlayerScript::OnChat, ScriptMgr.h:741/743/745), asks the parser
 * in pbot_chat.cpp whether the line is an order, resolves which bots it applies to, and drives
 * their PbotAI. Runs inline on the world tick that handled the chat packet, so it must stay cheap:
 * the very first thing every hook does is PbotMgr::OwnerHasBots(), an allocation-free probe that
 * short-circuits every line typed by the (overwhelmingly common) player who owns no bots.
 *
 * Why hook the owner instead of the bot: bots have no socket and therefore no inbound opcode path
 * (DESIGN_PHASE3 SS8.6) — nothing is ever "delivered" to them. Server-side interception is the only
 * mechanism available, and it has the pleasant side effect of working identically for say, party
 * and whisper without three different packet handlers.
 *
 * Reply discipline: exactly ONE bot answers a squad-wide order. Four bots chorusing "Иду за тобой."
 * on every command reads as broken, not alive. Per-owner throttling on top of that keeps a player
 * spamming orders from flooding nearby chat.
 */

#include "pbot_mgr.h"
#include "pbot_ai.h"
#include "pbot_chat.h"

#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Util.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Minimum gap between two spoken bot replies for the same owner. Orders themselves are always
    // applied — only the chatter is rate limited.
    constexpr uint32 PBOT_REPLY_THROTTLE_MS = 1500;

    std::unordered_map<ObjectGuid, uint32> g_lastReplyMs;

    bool ReplyAllowed(ObjectGuid ownerGuid)
    {
        uint32 const now = GameTime::GetGameTimeMS();
        auto it = g_lastReplyMs.find(ownerGuid);
        if (it != g_lastReplyMs.end() && now - it->second < PBOT_REPLY_THROTTLE_MS)
            return false;

        g_lastReplyMs[ownerGuid] = now;
        return true;
    }

    // Bots only react when they are actually present with the owner. A bot on another map (mid
    // teleport recovery) silently ignores orders rather than acting on stale context.
    bool BotCanHear(Player const* bot, Player const* owner)
    {
        return bot && owner && bot->IsInWorld() && owner->IsInWorld() && bot->GetMap() == owner->GetMap();
    }

    void Speak(Player* bot, Player* owner, char const* text, bool viaWhisper)
    {
        if (viaWhisper)
            bot->Whisper(text, LANG_UNIVERSAL, owner);
        else
            bot->Say(text, LANG_UNIVERSAL);
    }

    // Core dispatch shared by all three chat channels.
    //   explicitBot : set for whispers — the owner already chose the recipient.
    //   addressImplied : whispers need no "боты, ..." prefix (see PbotChat::Parse).
    //   viaWhisper : route replies back the way the order came in.
    void DispatchOwnerLine(Player* owner, std::string const& msg, ObjectGuid explicitBot,
                           bool addressImplied, bool viaWhisper)
    {
        if (!owner || msg.empty())
            return;

        if (!PbotMgr::OwnerHasBots(owner->GetGUID()))
            return;

        PbotChatCommand cmd;
        if (!PbotChat::Parse(msg, addressImplied, cmd))
            return;

        // Build the recipient set. A whisper targets exactly one bot; otherwise the order goes to
        // every bot whose name matches (or all of them when no name was typed).
        std::vector<ObjectGuid> recipients;
        if (!explicitBot.IsEmpty())
        {
            recipients.push_back(explicitBot);
        }
        else
        {
            for (ObjectGuid const& guid : PbotMgr::GetBotsOf(owner))
            {
                if (!cmd.TargetName.empty())
                {
                    Player* bot = PbotMgr::FindBot(guid);
                    // The parser hands us a lowercased candidate word; if it matches no bot name
                    // the line was ordinary conversation and we drop it entirely.
                    if (!bot || !StringEqualI(bot->GetName(), cmd.TargetName))
                        continue;
                }
                recipients.push_back(guid);
            }
        }

        if (recipients.empty())
            return;

        // Throttle is evaluated ONCE per order, not per bot: consulting it inside the loop would
        // let the first bot consume the window and silence the rest, which is wrong for the one
        // verb (Status) where every addressed bot is supposed to answer.
        bool const mayReply = ReplyAllowed(owner->GetGUID());
        bool spoken = false;   // one voice per order (see file header)

        for (ObjectGuid const& guid : recipients)
        {
            Player* bot = PbotMgr::FindBot(guid);
            if (!BotCanHear(bot, owner))
                continue;

            PbotAI* ai = PbotMgr::GetBotAI(guid);
            if (!ai)
                continue;

            switch (cmd.Verb)
            {
                case PbotVerb::Status:
                    // Status is per-bot information, so every addressed bot reports — that is the
                    // one case where hearing all of them is the point.
                    if (mayReply)
                        Speak(bot, owner, ai->DescribeState().c_str(), viaWhisper);
                    break;

                case PbotVerb::Help:
                    if (mayReply && !spoken)
                    {
                        for (uint32 i = 0; char const* line = PbotChat::HelpLine(i); ++i)
                            Speak(bot, owner, line, viaWhisper);
                        spoken = true;
                    }
                    break;

                default:
                    // The order itself is ALWAYS applied — only the chatter is rate limited.
                    ai->ApplyCommand(cmd.Verb);
                    if (mayReply && !spoken)
                    {
                        Speak(bot, owner, PbotChat::Acknowledgement(cmd.Verb), viaWhisper);
                        spoken = true;
                    }
                    break;
            }
        }
    }
}

class PbotChatScript : public PlayerScript
{
public:
    PbotChatScript() : PlayerScript("pbot_chatscript") { }

    // Say / Yell / Emote. Emote is excluded: "/e стоит рядом" is narration, not an order.
    void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        if (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL)
            return;

        DispatchOwnerLine(player, msg, ObjectGuid::Empty, /*addressImplied*/ false, /*viaWhisper*/ false);
    }

    // Whisper. The owner picked the recipient, so no address prefix is needed — but we verify the
    // recipient is one of THIS player's bots, so whispering someone else's bot does nothing.
    void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Player* receiver) override
    {
        if (!player || !receiver)
            return;

        if (PbotMgr::GetOwnerOf(receiver->GetGUID()) != player->GetGUID())
            return;

        DispatchOwnerLine(player, msg, receiver->GetGUID(), /*addressImplied*/ true, /*viaWhisper*/ true);
    }

    // Party / raid. Bots are not auto-grouped yet, but the hook fires on the SENDER, so orders
    // typed in party chat still reach them. Address prefix still required — party chat is
    // conversation too.
    void OnChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Group* /*group*/) override
    {
        DispatchOwnerLine(player, msg, ObjectGuid::Empty, /*addressImplied*/ false, /*viaWhisper*/ false);
    }

    // Forget an owner's throttle stamp when they leave, so the map cannot grow without bound
    // across a long uptime.
    void OnLogout(Player* player) override
    {
        if (player)
            g_lastReplyMs.erase(player->GetGUID());
    }
};

void AddSC_pbot_chatscript()
{
    new PbotChatScript();
}
