/*
 * Companion Bots — owner PlayerScript hooks (TrinityCore master, retail 12.0.7).
 * Bridges the three player lifecycle events BotMgr cares about into the registry. Plain
 * PlayerScript subclass, instantiated directly (no macro exists for these). See DESIGN.md SS8.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "ThreatManager.h"
#include "bot_mgr.h"

class bot_playerscript : public PlayerScript
{
public:
    bot_playerscript() : PlayerScript("bot_playerscript") { }

    void OnLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        BotMgr::OnOwnerLevelChanged(player);
    }

    void OnMapChanged(Player* player) override
    {
        BotMgr::OnOwnerMapChanged(player);
    }

    void OnLogout(Player* player) override
    {
        BotMgr::OnOwnerLogout(player);
    }

    // Phase 2 gold share, "owner personally kills something a bot helped with" case. Mutually
    // exclusive with BotAI::KilledUnit (exactly one killer per death). We can NOT query the
    // victim's threat list here — Unit::Kill clears it (setDeathState → CombatStop →
    // ClearAllThreat) before this hook fires, so it is always empty (review finding #1). Instead
    // consult the engagement note BotAI records while fighting.
    void OnCreatureKill(Player* killer, Creature* killed) override
    {
        if (!killer || !killed)
            return;

        if (BotMgr::WasBotEngagedRecently(killer->GetGUID(), killed->GetGUID()))
            BotMgr::CreditGoldForKill(killer, killed);
    }
};

void AddSC_bot_playerscript()
{
    new bot_playerscript();
}
