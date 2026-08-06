/*
 * Companion Bots — script loader glue (TrinityCore master, retail 12.0.7).
 * Follows the standard per-directory TC convention: forward-declare every AddSC_* in this
 * directory and expose a single AddBotsScripts() that the top-level scripts loader calls.
 * See DESIGN.md SS8. (Integrator: confirm src/server/scripts/CMakeLists.txt picks up the new
 * Bots/ directory and that the generated scripts loader invokes AddBotsScripts().)
 */

// Core (coder A)
void AddSC_bot_commandscript();
void AddSC_bot_playerscript();

// Class kits (coders B / C) — names must match each kit file's AddSC_ definition.
void AddSC_bot_warrior();
void AddSC_bot_paladin();
void AddSC_bot_priest();
void AddSC_bot_mage();
void AddSC_bot_hunter();

// Phase 3.1: fake-player bots (DESIGN_PHASE3 SS8.9).
void AddSC_pbot_mgr();
void AddSC_pbot_commandscript();

// Phase 4A: chat-driven orders (pbot_chat_script.cpp).
void AddSC_pbot_chatscript();

// Phase 4 diagnostics: ".pbot selftest", console-capable (cs_pbot_selftest.cpp).
void AddSC_pbot_selftest_commandscript();

// Phase 6: ownerless world bots, ".pbot world ..." (cs_pbot_world.cpp).
void AddSC_pbot_world_commandscript();

void AddBotsScripts()
{
    AddSC_bot_commandscript();
    AddSC_bot_playerscript();
    AddSC_bot_warrior();
    AddSC_bot_paladin();
    AddSC_bot_priest();
    AddSC_bot_mage();
    AddSC_bot_hunter();
    AddSC_pbot_mgr();
    AddSC_pbot_commandscript();
    AddSC_pbot_chatscript();
    AddSC_pbot_selftest_commandscript();
    AddSC_pbot_world_commandscript();
}
