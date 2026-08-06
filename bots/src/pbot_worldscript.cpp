/*
 * Companion Bots — Phase 3.1 pbot script wiring (TrinityCore master, retail 12.0.7).
 *
 * Two scripts, both registered by AddSC_pbot_mgr() (called from the Bots script loader — the
 * integrator wires that per DESIGN_PHASE3 SS8.9; this file only provides the definitions):
 *
 *   PbotWorldScript  — the global per-tick driver. Player::Update() (via Map::Update) already ticks
 *                      each bot's physical simulation for free; this hook decides what each bot WANTS
 *                      to do this tick by calling its PbotAI::UpdateBot(diff). It also owns shutdown
 *                      cleanup, the ONLY thing that saves/removes bot sessions at process exit since
 *                      they are deliberately kept out of World::m_sessions (SS8.6 rule 1).
 *
 *   PbotPlayerScript — dismisses an owner's pbots when that owner logs out, so we never leave bots
 *                      orphaned in the world after their controller leaves.
 *
 * PbotWorldScript is a friend of PbotMgr (declared in pbot_mgr.h), which is what lets OnUpdate walk
 * the private registry and drive each controller directly without widening PbotMgr's public API.
 */

#include "pbot_mgr.h"
#include "pbot_ai.h"
#include "pbot_loader.h"
#include "pbot_gear.h"         // PbotIdentity::PickRandomClass for the startup population
#include "pbot_questgoal.h"   // where finished quests get handed in
#include "pbot_world_spots.h"  // Preload at boot instead of stalling the world thread later
#include "pbot_upkeep.h"       // repairer locations, likewise loaded at boot

#include "Config.h"            // Pbot.WorldPopulation
#include "SharedDefines.h"     // ALLIANCE / HORDE

#include <algorithm>

#include "ObjectAccessor.h"
#include "PhasingHandler.h"  // re-apply phases after a teleport — nothing else does it for a bot
#include "Player.h"          // also TeleportState + PLAYER_FLAGS_IN_PVP (near-teleport finalisation)
#include "ScriptMgr.h"
#include "WorldSession.h"   // ResetTimeOutTime — see the idle-kick note below

#include <unordered_map>
#include <vector>

// ---- startup orphan sweep + per-tick driver + shutdown cleanup (SS8.2 / SS8.5 / SS9) --------

void PbotWorldScript::OnStartup()
{
    // SS9 A.4 risk 1: retire roster rows whose owner character no longer exists. Runs once at startup
    // while no player is online (so no bot can be live yet). The sweep verifies owner existence against
    // the characters table directly — NOT sCharacterCache, whose load ordering vs. OnStartup is not
    // verifiable from this checkout (see PbotLoader::SweepOrphansAtStartup).
    PbotLoader::SweepOrphansAtStartup();

    // Phase 6: reclaim what transient world bots leave behind — orphan characters on bot accounts,
    // and bot accounts that no longer back any character. Runs after the roster sweep above so the
    // roster it consults is already consistent.
    PbotMgr::SweepTransientBots();

    // Pay the world-population table's full-table scans here, at boot, rather than on the world
    // thread during play — see PbotWorldSpots::Preload.
    PbotWorldSpots::Preload();

    // Same reasoning, same place: where the repairers are is a full-table join, paid once here.
    PbotUpkeep::PreloadRepairSpots();

    // And where finished quests are handed in. Same full-table join, same reason to pay for it at
    // boot: a bot deciding what to do next must not stop to read the world database.
    PbotQuestGoal::PreloadTurnInSpots();

    PopulateWorldFromConfig();
}

// Brings the world population back at startup: first the bots that already exist, then enough new
// ones to reach the configured number.
//
// This used to simply create the whole population from scratch every boot, because world bots were
// transient by design. They are not transient any more — a restart no longer throws away what they
// earned — so recreating them wholesale would now do exactly the damage persistence was added to
// prevent, AND leave the old characters lying around beside the new ones.
//
// Off by default. Set Pbot.WorldPopulation in worldserver.conf to the number of bots wanted.
void PbotWorldScript::PopulateWorldFromConfig()
{
    int32 const wanted = sConfigMgr->GetIntDefault("Pbot.WorldPopulation", 0);
    if (wanted <= 0)
        return;

    // Revive first, then top up by the shortfall. The reload is asynchronous, so the count of what
    // was queued — not a live headcount taken a moment later — is what the arithmetic must use.
    uint32 const revived = PbotMgr::ReloadWorldBots();
    if (revived >= uint32(wanted))
    {
        TC_LOG_INFO("scripts.bots", "pbot: {} persisted world bots cover the configured population "
            "of {}; creating none.", revived, wanted);
        return;
    }

    uint32 const count = std::min<uint32>(uint32(wanted) - revived, PBOT_GLOBAL_MAX);
    int32 const mapBudget = sConfigMgr->GetIntDefault("Pbot.WorldPopulationMaps",
                                                      int32(PbotWorldSpots::DEFAULT_MAX_MAPS));

    std::vector<PbotWorldSpots::Spot> const spots =
        PbotWorldSpots::Pick(count, /*mapFilter*/ -1, uint32(std::max(mapBudget, 1)));
    if (spots.empty())
    {
        TC_LOG_ERROR("scripts.bots", "pbot: Pbot.WorldPopulation={} but no population spots are "
            "available", wanted);
        return;
    }

    std::vector<PbotPopulateRequest> batch;
    batch.reserve(spots.size());
    for (size_t i = 0; i < spots.size(); ++i)
    {
        PbotPopulateRequest req;
        req.MapId = spots[i].MapId;
        req.Pos   = spots[i].Pos;
        req.Class = PbotIdentity::PickRandomClass();
        req.Level = spots[i].SuggestedLevel ? spots[i].SuggestedLevel : uint8(10);
        // Alternate factions so the world has both sides in it, same rule the command uses.
        req.Team  = (i % 2) ? uint32(HORDE) : uint32(ALLIANCE);
        batch.push_back(req);
    }

    PbotMgr::QueuePopulate(batch);
    TC_LOG_INFO("scripts.bots", "pbot: queued {} world bots from Pbot.WorldPopulation; they appear "
        "over about {} seconds", uint32(batch.size()),
        uint32((batch.size() * PBOT_POPULATE_INTERVAL_MS) / 1000));
}

void PbotWorldScript::OnUpdate(uint32 diff)
{
    // SS9 A.1: poll our OWN async reload processor once per tick (never WorldSession::Update). Done
    // first so a bot that finished loading this frame is registered before UpdateBot iterates below.
    PbotLoader::PumpPendingLoads();

    // Create at most one queued world bot per interval. Doing a whole population batch inside the
    // command that asked for it froze the world thread past the 60-second anti-freeze watchdog and
    // crashed the server — see PBOT_POPULATE_INTERVAL_MS.
    PbotMgr::TickPopulateQueue(diff);
    PbotMgr::TickReloadQueue(diff);

    // Cross-map teleports (Phase 7 prerequisite). A bot has no client to answer the worldport, so
    // it used to sit in IsBeingTeleportedFar() limbo and get DISMISSED — which meant bots could
    // never follow an owner through a portal, and could never enter a battleground.
    //
    // The engine exposes the resolution explicitly: WorldSession::HandleMoveWorldportAck(), tagged
    // "for server-side calls" in WorldSession.h:1372. Calling it completes the port exactly as a
    // client's ack would. The dismiss path stays only as a bounded backstop, so a port that somehow
    // never resolves still cannot strand a bot forever.
    static std::unordered_map<ObjectGuid, uint32> portAttempts;
    constexpr uint32 MAX_PORT_ATTEMPTS = 100;

    static std::vector<ObjectGuid> strandedGuids; // cleared every pass; static to avoid per-tick alloc
    strandedGuids.clear();

    // Iterate the registry directly (friend access). UpdateBot never dismisses a bot or otherwise
    // mutates the registry (SS8.3 contract: "this class never self-terminates"), so iterating the
    // live map here is safe — no structural mutation occurs mid-loop.
    for (auto& kv : PbotMgr::_bots)
    {
        // Re-assert the never-idle sentinel. Map::Update calls WorldSession::Update on every
        // player's session, and its first statement closes an "idle" connection's socket WITHOUT a
        // null check — instant SIGSEGV for a socket-less bot. WorldSession::Update itself can push
        // the timeout back down to now+60s while processing, so writing the sentinel here keeps it
        // pinned. This is a crash guard, not a nicety — see PBOT_SESSION_NEVER_IDLE.
        if (kv.second.Session)
            kv.second.Session->m_timeOutTime = PBOT_SESSION_NEVER_IDLE;

        // Resolve through the SESSION, not ObjectAccessor. A player mid far-teleport has been
        // removed from the map and from ObjectAccessor until the worldport is acknowledged — so
        // looking it up there returns null exactly when we most need it, and the ack below would
        // never run. That left bots permanently invisible in teleport limbo: "TeleportTo=true"
        // followed by "not resolvable" forever, which also silently broke battleground queueing
        // because every command skipped them. The session's player pointer stays valid throughout.
        Player* bot = kv.second.Session ? kv.second.Session->GetPlayer() : nullptr;
        if (!bot)
            bot = ObjectAccessor::FindPlayer(kv.first);

        if (bot)
        {
            if (bot->IsBeingTeleportedFar())
            {
                uint32& attempts = portAttempts[kv.first];
                if (++attempts > MAX_PORT_ATTEMPTS)
                {
                    // Never resolved — fall back to the old behaviour rather than retrying forever.
                    strandedGuids.push_back(kv.first);
                    portAttempts.erase(kv.first);
                }
                else if (kv.second.Session)
                {
                    kv.second.Session->HandleMoveWorldportAck();

                    // The worldport lands the bot on a new map; its phase belongs to the old one
                    // until something re-applies it, and for a bot nothing ever does.
                    if (Player* landed = kv.second.Session->GetPlayer())
                        if (landed->IsInWorld())
                            PhasingHandler::OnMapChange(landed);
                }
                continue;   // land first; act normally from the next tick
            }

            portAttempts.erase(kv.first);   // arrived (or never left)

            // ★ SAME-MAP teleports need answering too, and this is a DIFFERENT handshake.
            //
            // Moving a bot within one map is a NEAR teleport: the engine parks the player in
            // WaitingForTeleportAck and waits for the client's CMSG_MOVE_TELEPORT_ACK. A bot has no
            // client, so it stayed parked forever — and because Player::TeleportTo refuses outright
            // while a teleport is pending, every LATER teleport for that bot was silently rejected
            // as well. Measured: 15 zone migrations succeeded and then 64 in a row failed with no
            // engine error logged at all, because the only check that fails quietly is that one.
            //
            // WorldSession::HandleMoveTeleportAck takes a client packet and validates the mover, so
            // it cannot be called server-side the way HandleMoveWorldportAck can. These are the
            // steps of it that matter with no client attached.
            if (bot->IsBeingTeleportedNear())
            {
                uint32 const oldZone = bot->GetZoneId();
                WorldLocation const destination = bot->GetTeleportDest().Location;

                bot->SetTeleportState(TeleportState::NotTeleporting);
                bot->UpdatePosition(destination, true);
                bot->SetFallInformation(0, bot->GetPositionZ());

                uint32 newZone = 0;
                uint32 newArea = 0;
                bot->GetZoneAndAreaId(newZone, newArea);
                bot->UpdateZone(newZone, newArea);

                // Re-apply the phase shift after moving: it is area-dependent, and the login path
                // that normally maintains it does not exist for a bot. See PbotMgr::SpawnWorldBot.
                PhasingHandler::OnMapChange(bot);

                if (oldZone != newZone && bot->IsPvP() && !bot->HasPlayerFlag(PLAYER_FLAGS_IN_PVP))
                    bot->UpdatePvP(false, false);

                bot->ProcessDelayedOperations();
                continue;   // land first; act normally from the next tick
            }
        }
        if (kv.second.Ai)
            kv.second.Ai->UpdateBot(diff);
    }

    for (ObjectGuid const& guid : strandedGuids)
        PbotMgr::DismissBot(guid);
}

void PbotWorldScript::OnShutdown()
{
    PbotMgr::DismissAllOnShutdown();
}

// ---- owner-logout hook ----------------------------------------------------------------------

class PbotPlayerScript : public PlayerScript
{
public:
    PbotPlayerScript() : PlayerScript("pbot_playerscript") { }

    // Reclaim (SS9 A.3): when an owner logs in, enqueue an async reload of each of their rostered
    // bots (skipping any already live). ScriptMgr.h:760 signature. firstLogin is irrelevant here — a
    // brand-new character has no roster rows, so ReloadOwnerRoster is a cheap no-op for it.
    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (player)
            PbotLoader::ReloadOwnerRoster(player->GetGUID());
    }

    // Kills are the only honest measure of combat effectiveness. "In combat" says nothing — for
    // most of this project the bots were in combat constantly while killing nothing at all, because
    // things were attacking them. A kill means the bot picked a fight and finished it.
    void OnCreatureKill(Player* killer, Creature* killed) override
    {
        if (!killer || !killed || !PbotMgr::GetBotAI(killer->GetGUID()))
            return;

        TC_LOG_INFO("scripts.bots", "pbot kill: {} (level {}) killed '{}' (level {})",
            killer->GetName(), uint32(killer->GetLevel()), killed->GetName(), uint32(killed->GetLevel()));
    }

    // When the owner leaves, tear down their bots (SS8.5 — now TEMPORARY: the characters persist and
    // reload via OnLogin next time). Fires inside the owner's own LogoutPlayer, while the owner and
    // its map are still valid.
    void OnLogout(Player* player) override
    {
        PbotMgr::DismissAll(player);
    }

    // Phase 7: a challenged bot accepts. Only the flag is set here — the state transition happens
    // on the next AI tick, because this hook fires from inside the duel spell's effect handler
    // (SpellEffects.cpp, right after both duellists get their DuelInfo) and mutating that state
    // mid-execution buys nothing. `target` is the challenged side, which is the one that accepts.
    void OnDuelRequest(Player* target, Player* /*challenger*/) override
    {
        if (!target)
            return;

        if (PbotAI* ai = PbotMgr::GetBotAI(target->GetGUID()))
            ai->NoteDuelChallenge();
    }
};

// ---- registration ---------------------------------------------------------------------------
// Registers PbotWorldScript (per-tick driver + shutdown) and PbotPlayerScript (owner logout). The
// cs_pbot.cpp command table registers its own CommandScript via AddSC_pbot_commandscript() (coder C).
void AddSC_pbot_mgr()
{
    new PbotWorldScript();
    new PbotPlayerScript();
}
