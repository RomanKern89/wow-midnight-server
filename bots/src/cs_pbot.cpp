/*
 * Fake-player companion bots — chat commands (TrinityCore master, retail 12.0.7).
 *   .pbot spawn <warrior|paladin|priest|mage|hunter>
 *   .pbot dismiss [all]   — TEMPORARY: unloads now, character persists and reloads on next login (SS9)
 *   .pbot retire  [all]   — PERMANENT: deletes the bot character forever (SS9 A.2)
 *   .pbot list
 *
 * Modern Trinity::ChatCommands table, modeled on Phase 1's cs_bot.cpp. Player-context only
 * (Console::No). Gated by custom RBAC perms 1010-1012 granted to security level 0 so regular
 * players can use them — see sql/auth_pbots_rbac.sql. All character/session fabrication lives
 * behind PbotMgr's SS8.2 public interface; this file only parses input and formats output.
 * See DESIGN_PHASE3.md SS8.1 / SS8.8.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "RBAC.h"
#include "Util.h"
#include "WorldSession.h"
#include "pbot_gear.h"
#include "pbot_mgr.h"

#include <vector>

using namespace Trinity::ChatCommands;

class pbot_commandscript : public CommandScript
{
public:
    pbot_commandscript() : CommandScript("pbot_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable pbotCommandTable =
        {
            { "spawn",   HandlePbotSpawnCommand,   static_cast<rbac::RBACPermissions>(1010), Console::No },
            { "dismiss", HandlePbotDismissCommand, static_cast<rbac::RBACPermissions>(1011), Console::No },
            { "retire",  HandlePbotRetireCommand,  static_cast<rbac::RBACPermissions>(1011), Console::No },
            { "list",    HandlePbotListCommand,    static_cast<rbac::RBACPermissions>(1012), Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "pbot", pbotCommandTable },
        };
        return commandTable;
    }

    static bool HandlePbotSpawnCommand(ChatHandler* handler, std::string_view classToken)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        // Phase 4B: all 13 classes, plus "random" so an owner can fill a squad without naming
        // each one. Race is chosen inside SpawnBot from whatever this realm can actually create.
        PbotClass classId;
        if (StringEqualI(classToken, "random") || StringEqualI(classToken, "any"))
        {
            classId = PbotIdentity::PickRandomClass();
        }
        else if (!PbotIdentity::ParseClass(classToken, classId))
        {
            handler->PSendSysMessage("Usage: .pbot spawn %s", PbotIdentity::ClassTokenList());
            handler->SetSentErrorMessage(true);
            return false;
        }

        PbotSpawnError err;
        Player* bot = PbotMgr::SpawnBot(player, classId, &err);
        if (!bot)
        {
            handler->PSendSysMessage("Could not spawn companion bot: %s",
                err.Reason.empty() ? "unknown error" : err.Reason.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Spawned %s the %s companion bot.",
            bot->GetName().c_str(), PbotIdentity::ClassName(classId));
        return true;
    }

    static bool HandlePbotDismissCommand(ChatHandler* handler, Optional<std::string_view> allToken)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (allToken && StringEqualI(*allToken, "all"))
        {
            PbotMgr::DismissAll(player);
            handler->PSendSysMessage("All companion bots dismissed (they will return when you next log in).");
            return true;
        }

        std::vector<ObjectGuid> bots = PbotMgr::GetBotsOf(player);
        if (bots.empty())
        {
            handler->PSendSysMessage("You have no companion bots to dismiss.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!PbotMgr::DismissBot(bots.front()))
        {
            handler->PSendSysMessage("Could not dismiss companion bot.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Companion bot dismissed; it will return on your next login (%u still active).",
            uint32(PbotMgr::GetBotsOf(player).size()));
        return true;
    }

    // Permanent counterpart to dismiss: deletes the bot character for good (SS9 A.2). ".pbot retire all"
    // also removes offline-rostered bots, so it clears everything the owner has persisted.
    static bool HandlePbotRetireCommand(ChatHandler* handler, Optional<std::string_view> allToken)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (allToken && StringEqualI(*allToken, "all"))
        {
            PbotMgr::RetireAll(player);
            handler->PSendSysMessage("All companion bots permanently retired.");
            return true;
        }

        std::vector<ObjectGuid> bots = PbotMgr::GetBotsOf(player);
        if (bots.empty())
        {
            handler->PSendSysMessage("You have no active companion bots to retire. Use \".pbot retire all\" to also remove offline ones.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!PbotMgr::RetireBot(bots.front()))
        {
            handler->PSendSysMessage("Could not retire companion bot.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Companion bot permanently retired (%u still active).",
            uint32(PbotMgr::GetBotsOf(player).size()));
        return true;
    }

    static bool HandlePbotListCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<ObjectGuid> bots = PbotMgr::GetBotsOf(player);
        if (bots.empty())
        {
            handler->PSendSysMessage("You have no companion bots.");
            return true;
        }

        handler->PSendSysMessage("Companion bots (%u):", uint32(bots.size()));
        uint32 idx = 1;
        for (ObjectGuid const& guid : bots)
        {
            if (Player* bot = PbotMgr::FindBot(guid))
                handler->PSendSysMessage("  %u. %s  (%s, level %u, %.0f%% hp)",
                    idx, bot->GetName().c_str(), PbotIdentity::ClassNameForClassId(bot->GetClass()),
                    uint32(bot->GetLevel()), bot->GetHealthPct());
            else
                handler->PSendSysMessage("  %u. <not in world>", idx);
            ++idx;
        }
        return true;
    }
};

void AddSC_pbot_commandscript()
{
    new pbot_commandscript();
}
