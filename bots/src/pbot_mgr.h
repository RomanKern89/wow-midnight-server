/*
 * Companion Bots — Phase 3.1 PbotMgr: fake-player session/character fabrication + registry
 * (TrinityCore master, retail 12.0.7).
 *
 * PbotMgr is the load-bearing, highest-risk piece of Phase 3.1 (DESIGN_PHASE3 SS8.2/SS8.4/SS8.5).
 * It fabricates a socket-less WorldSession + a real Player character with NO client attached, adds
 * that Player to the owner's map, and drives it via our own PbotAI from PbotWorldScript::OnUpdate.
 *
 * READ DESIGN_PHASE3 SS8.6 (THE FORBIDDEN LIST) BEFORE TOUCHING THIS. In particular, and enforced
 * throughout the .cpp:
 *   - bot WorldSessions are NEVER passed to sWorld->AddSession() and WorldSession::Update() is NEVER
 *     called on them (either would silently delete the session+Player on the next world tick, or
 *     crash on the null-socket CloseSocket dereference).
 *   - bots are driven by Map::Update (physical sim, for free once added to a Map) + PbotAI
 *     (decisions), never by the native PlayerAI auto-dispatch (structurally excluded for
 *     TYPEID_PLAYER).
 *   - Player and WorldSession pointers are re-resolved through the registry / ObjectAccessor by guid each tick,
 *     never cached across ticks.
 *
 * The Phase 3.1 public interface below is DESIGN_PHASE3 SS8.2 verbatim. Phase 3.2 (SS9 PART A,
 * persistence) sanctions modifying this header (see the combined 3.2 manifest): dismiss is split
 * into DismissBot (temporary — character now PERSISTS) + RetireBot (permanent), a pbot_roster-backed
 * layer is added, and a small internal API is exposed for the async reload path in pbot_loader.cpp.
 */

#ifndef TRINITYCORE_PBOT_MGR_H
#define TRINITYCORE_PBOT_MGR_H

#include "ObjectGuid.h"
#include "Position.h"  // PbotPopulateRequest carries a spawn point
#include "ScriptMgr.h" // WorldScript base for PbotWorldScript (SS8.2 comment references it; required
                       // here so the class definition below has a complete base type)
#include "pbot_common.h"
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class WorldSession;
class PbotAI; // internal controller owned by BotRecord (see file header); not part of the SS8.2 API.

// Phase 4B: all 13 retail classes. The underlying values are the engine CLASS_* ids rather than a
// private 0..N sequence — the enum used to be a separate namespace that had to be mapped in two
// directions, and every extra class doubled that bookkeeping. Now PbotClass IS the class id, so
// ClassId() is a cast and the reverse lookup cannot drift out of sync.
enum class PbotClass : uint8
{
    Warrior     = 1,
    Paladin     = 2,
    Hunter      = 3,
    Rogue       = 4,
    Priest      = 5,
    DeathKnight = 6,
    Shaman      = 7,
    Mage        = 8,
    Warlock     = 9,
    Monk        = 10,
    Druid       = 11,
    DemonHunter = 12,
    Evoker      = 13
};

struct PbotSpawnError { std::string Reason; };

// One pending world-bot creation. Populating the world is expressed as a queue of these rather than
// a loop, because each one can block the world thread on disk I/O — see PBOT_POPULATE_INTERVAL_MS.
struct PbotPopulateRequest
{
    uint32    MapId = 0;
    Position  Pos;
    PbotClass Class = PbotClass::Warrior;
    uint8     Level = 1;
    uint32    Team  = 0;
};

// One pbot_roster row (DESIGN_PHASE3 SS9 A.2), as read back by the reload path. ClassId is the engine
// unit class id (CLASS_*), stored so the reload can reconstruct the registry's PbotClass metadata.
struct PbotRosterEntry
{
    ObjectGuid BotGuid;
    uint32     AccountId = 0;
    uint8      ClassId   = 0;
};

class PbotMgr
{
public:
    // Fabricates a bot account (if needed), a bot character (race chosen from owner faction,
    // SS8.7), a socket-less WorldSession, adds the Player to owner map next to the owner, and
    // registers a PbotAI controller for it. Returns nullptr and fills err on failure.
    static Player* SpawnBot(Player* owner, PbotClass classId, PbotSpawnError* err);

    // ---- Phase 6: ownerless world bots -------------------------------------------------------

    // Same fabrication, but the bot belongs to nobody: it is placed at an explicit position, given
    // an explicit level, and driven by PbotAI's autonomous path instead of following an owner.
    //
    // Deliberately transient — no pbot_roster row, so world bots do not survive a restart. They are
    // population and test scaffolding, and making them persistent would drag in orphan-sweep rules
    // for rows whose owner is by definition absent. Restarting the server clears them; that is the
    // intended way to reset a populated zone.
    // Takes a map ID rather than a Map*: with nobody logged in, no world map is loaded, so a
    // caller on the server console has no Map* to hand us and could not obtain one (MapManager's
    // world-map constructor is private). Resolving it here lets us use the engine's own
    // MapManager::CreateMap with the freshly built bot as the anchoring player — the exact path a
    // real character logging into that zone takes — so world bots can be created and observed with
    // no game client attached at all.
    static Player* SpawnWorldBot(uint32 mapId, Position const& pos, PbotClass classId, uint8 level,
                                 uint32 team, PbotSpawnError* err);

    // Queues a batch of world bots to be created one per PBOT_POPULATE_INTERVAL_MS instead of all
    // at once. The caller returns immediately; the bots appear over the following seconds.
    static void   QueuePopulate(std::vector<PbotPopulateRequest> const& requests);
    static uint32 PopulateQueueSize();

    // Drains the persisted-bot reload queue at the same pace as the spawn queue. Called from the
    // same world-tick hook.
    static void TickReloadQueue(uint32 diff);
    static void   TickPopulateQueue(uint32 diff);   // driven by PbotWorldScript::OnUpdate

    // Dismisses every live world bot. Returns how many were removed. Also drops anything still
    // queued — otherwise "clear" would be immediately undone by the pending batch.
    static uint32 RemoveAllWorldBots();
    static uint32 CountWorldBots();
    static std::vector<ObjectGuid> GetWorldBots();

    // Startup self-heal for the two things transient bots leak across a restart:
    //   - a character row on a PBOT account with no pbot_roster entry (a world bot that outlived
    //     the process that owned it, e.g. after a hard kill rather than a clean shutdown);
    //   - a PBOT account with no character at all, which AcquireBotAccount would otherwise skip
    //     forever because the username already exists while the in-memory free list is empty.
    // Returns the number of orphan characters removed.
    static uint32 SweepTransientBots();

    // TEMPORARY removal (DESIGN_PHASE3 SS9 A.2): WorldSession::LogoutPlayer(true) saves the character,
    // then we delete the WorldSession and drop the live registry entry. The character row, account,
    // and pbot_roster row are all deliberately PRESERVED so the bot reloads on the owner's next login.
    static bool DismissBot(ObjectGuid botGuid);
    static void DismissAll(Player* owner);
    static void DismissAllOnShutdown(); // called from OnShutdown (SS8.5), iterates every bot spawned

    // PERMANENT removal (DESIGN_PHASE3 SS9 A.2): everything DismissBot does (if the bot is live) PLUS
    // Player::DeleteFromDB, delete the pbot_roster row, and release the account back to the reusable
    // pool. Works whether the bot is currently live or only rostered (offline). This is the ONLY path
    // that ever calls ReleaseBotAccount (A.4 risk 2 — a rostered account is never reused while rostered).
    static bool RetireBot(ObjectGuid botGuid);
    static void RetireAll(Player* owner); // every bot the owner has: live AND offline-rostered

    static std::vector<ObjectGuid> GetBotsOf(Player const* owner);
    static Player* FindBot(ObjectGuid botGuid); // ObjectAccessor::FindPlayer under the hood

    // ---- Phase 4A: chat command layer support (pbot_chat_script.cpp) ------------------------
    // Allocation-free "does this player own any bots" probe. The chat hook runs for EVERY line
    // every player types, so the common case (a player with no bots) must not build a vector.
    static bool OwnerHasBots(ObjectGuid ownerGuid);

    // The live controller for a bot, or nullptr if that guid is not a registered live bot.
    // The returned pointer is owned by the registry and is valid only for the current world tick
    // — same "never cache across ticks" discipline as the Player pointers (see file header).
    static PbotAI* GetBotAI(ObjectGuid botGuid);

    // Owner of a registered bot, or an empty guid. Lets the whisper hook verify that the player
    // is whispering their OWN bot before obeying.
    static ObjectGuid GetOwnerOf(ObjectGuid botGuid);

    // ---- internal support for the async reload path (pbot_loader.cpp, SS9 A.1/A.2) ----------
    // Public because pbot_loader.cpp is a separate TU; not part of the SS8.2 command-facing surface.
    static WorldSession* CreateBotSession(uint32 accountId);          // socket-less session (SS8.4 args)
    static bool IsBotLive(ObjectGuid botGuid);                        // double-spawn guard (A.3)
    static bool FinalizeReloadedBot(ObjectGuid ownerGuid, Player* bot, WorldSession* session, uint8 unitClassId);

    // Brings the persisted world population back at startup: reloads every rostered ownerless bot,
    // and returns how many were enqueued so the caller only tops the population up by the shortfall
    // instead of creating a second set beside them.
    static uint32 ReloadWorldBots();
    static std::vector<PbotRosterEntry> QueryRosterForOwner(ObjectGuid ownerGuid);
    static std::vector<ObjectGuid>      QueryAllRosterOwners();       // distinct owner guids (orphan sweep)
    static void DeleteRosterRow(ObjectGuid botGuid);                  // stale-load cleanup (A.2 step 5)

private:
    struct BotRecord
    {
        ObjectGuid OwnerGuid;
        ObjectGuid BotGuid;
        WorldSession* Session;       // owned by us, never registered with sWorld AddSession
        PbotClass Class;
        std::unique_ptr<PbotAI> Ai;  // per-tick controller, driven by PbotWorldScript::OnUpdate
    };
    static std::unordered_map<ObjectGuid, BotRecord> _bots;             // key: bot guid
    static std::unordered_multimap<ObjectGuid, ObjectGuid> _byOwner;    // owner guid -> bot guid

    static std::deque<PbotPopulateRequest> _populateQueue;
    static uint32 _populateTimerMs;

    // Persisted bots waiting their turn to come back. Separate from the spawn queue because they
    // carry roster identity rather than a recipe, but drained at the same unhurried pace and for
    // the same reason: sixty players entering the world at once stalls the world thread past the
    // anti-freeze watchdog and crashes the server.
    static std::deque<PbotRosterEntry> _reloadQueue;
    static uint32 _reloadTimerMs;

    // Shared spawn/reload internals (extracted so SpawnBot and the reload path don't duplicate them).
    static bool PlaceInWorldNextToOwner(Player* owner, Player* bot);    // ResetMap/Relocate/SetMap/AddPlayerToMap/AddObject
    static void RegisterBotRecord(Player* owner, Player* bot, WorldSession* session, PbotClass cls);

    // The ownerless equivalents: a world bot comes back where it was saved, not beside anybody.
    static bool PlaceWorldBotAtSavedPosition(Player* bot);
    static bool FinalizeReloadedWorldBot(Player* bot, WorldSession* session, uint8 unitClassId);

    // Steps 3-8 of the spawn recipe — Player construction, Create(), empty-social init, SaveToDB,
    // character-cache entry and session wiring — which are byte-for-byte the same whether the bot
    // will follow an owner or live on its own. Returns nullptr on failure, having already torn the
    // half-built Player down; the caller still owns the session and the account.
    static Player* CreateBotCharacter(WorldSession* session, uint32 accountId, uint8 raceId,
                                      uint8 classIdRaw, std::string const& name);
    static void TeardownLiveRecord(std::unordered_map<ObjectGuid, BotRecord>::iterator it); // LogoutPlayer(true)+delete session, drops registry

    friend class PbotWorldScript; // OnUpdate/OnStartup/OnShutdown wiring, SS8.4/SS8.5/SS9
};

// A WorldScript, not a PlayerScript (Phase 1/2 used PlayerScript for per-owner hooks; this is a
// global per-tick driver instead):
class PbotWorldScript : public WorldScript // ScriptMgr.h:248
{
public:
    PbotWorldScript() : WorldScript("pbot_worldscript") { }
    void OnStartup() override;             // ScriptMgr.h:277 - PbotLoader orphan sweep (SS9 A.4 risk 1)

private:
    static void PopulateWorldFromConfig();  // Pbot.WorldPopulation — refill the world at boot
public:
    void OnUpdate(uint32 diff) override;   // ScriptMgr.h:274 - drives PbotAI + polls PbotLoader pump
    void OnShutdown() override;            // ScriptMgr.h:280 - PbotMgr::DismissAllOnShutdown()
};

#endif // TRINITYCORE_PBOT_MGR_H
