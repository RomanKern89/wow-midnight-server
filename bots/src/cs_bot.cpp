/*
 * Companion Bots — chat commands (TrinityCore master, retail 12.0.7).
 *   .bot add <warrior|paladin|priest|mage|hunter>
 *   .bot remove [all]
 *   .bot info
 * Modern Trinity::ChatCommands table, modeled on cs_pet.cpp. Gated by the custom RBAC perms
 * added in RBAC.h (1000-1002) and granted to security level 0 so regular players can use them.
 * See DESIGN.md SS6.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "Util.h"
#include "WorldSession.h"
#include "bot_common.h"
#include "bot_mgr.h"

using namespace Trinity::ChatCommands;

namespace
{
    bool ParseBotClass(std::string_view token, BotClass& out)
    {
        if (StringEqualI(token, "warrior")) { out = BOT_CLASS_WARRIOR; return true; }
        if (StringEqualI(token, "paladin")) { out = BOT_CLASS_PALADIN; return true; }
        if (StringEqualI(token, "priest"))  { out = BOT_CLASS_PRIEST;  return true; }
        if (StringEqualI(token, "mage"))    { out = BOT_CLASS_MAGE;    return true; }
        if (StringEqualI(token, "hunter"))  { out = BOT_CLASS_HUNTER;  return true; }
        return false;
    }
}

class bot_commandscript : public CommandScript
{
public:
    bot_commandscript() : CommandScript("bot_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable botCommandTable =
        {
            { "add",    HandleBotAddCommand,    static_cast<rbac::RBACPermissions>(1000),    Console::No },
            { "remove", HandleBotRemoveCommand, static_cast<rbac::RBACPermissions>(1001), Console::No },
            { "info",   HandleBotInfoCommand,   static_cast<rbac::RBACPermissions>(1002),   Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "bot", botCommandTable },
        };
        return commandTable;
    }

    static bool HandleBotAddCommand(ChatHandler* handler, std::string_view classToken)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        BotClass botClass;
        if (!ParseBotClass(classToken, botClass))
        {
            handler->PSendSysMessage("Usage: .bot add warrior|paladin|priest|mage|hunter");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string err;
        if (!BotMgr::AddBot(player, botClass, err))
        {
            handler->PSendSysMessage("Could not add companion bot: %s", err.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Companion bot summoned (%u/%u).", BotMgr::GetBotCount(player), MAX_BOTS_PER_PLAYER);
        return true;
    }

    static bool HandleBotRemoveCommand(ChatHandler* handler, Optional<std::string_view> allToken)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (allToken && StringEqualI(*allToken, "all"))
        {
            BotMgr::RemoveAllBots(player);
            handler->PSendSysMessage("All companion bots dismissed.");
            return true;
        }

        if (!BotMgr::RemoveBot(player))
        {
            handler->PSendSysMessage("You have no companion bots to dismiss.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Companion bot dismissed (%u remaining).", BotMgr::GetBotCount(player));
        return true;
    }

    static bool HandleBotInfoCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<ObjectGuid> bots = BotMgr::GetBots(player);
        if (bots.empty())
        {
            handler->PSendSysMessage("You have no companion bots.");
            return true;
        }

        handler->PSendSysMessage("Companion bots (%u/%u):", uint32(bots.size()), MAX_BOTS_PER_PLAYER);
        uint32 idx = 1;
        for (ObjectGuid const& guid : bots)
        {
            if (Creature* bot = ObjectAccessor::GetCreature(*player, guid))
                handler->PSendSysMessage("  %u. %s  (level %u, %.0f%% hp)",
                    idx, bot->GetName().c_str(), uint32(bot->GetLevel()), bot->GetHealthPct());
            else
                handler->PSendSysMessage("  %u. <not on this map>", idx);
            ++idx;
        }
        return true;
    }
};

void AddSC_bot_commandscript()
{
    new bot_commandscript();
}
