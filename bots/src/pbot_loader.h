/*
 * Companion Bots — Phase 3.2 pbot async reload machinery (TrinityCore master, retail 12.0.7).
 *
 * Reloads persisted fake-player bots from the DB when their owner logs in (or, defensively, sweeps
 * orphans at server startup), so bots survive a worldserver restart. See DESIGN_PHASE3.md SS9 PART A
 * (A.1 loader recipe, A.2 persistence model, A.3 owner-login reclaim, A.4 risks).
 *
 * ARCHITECTURE (SS8.6 FORBIDDEN LIST still absolute): the reload path owns its OWN
 * AsyncCallbackProcessor<SQLQueryHolderCallback>, polled once per world tick from the ALREADY
 * EXISTING PbotWorldScript::OnUpdate. It NEVER touches WorldSession::AddQueryHolderCallback /
 * WorldSession::Update / ProcessQueryCallbacks — those are the forbidden, WorldSession-private path.
 * This mirrors exactly what the engine does for a real login (CharacterDatabase.DelayQueryHolder +
 * a callback processor), just owned by us instead of by WorldSession.
 */

#ifndef TRINITYCORE_PBOT_LOADER_H
#define TRINITYCORE_PBOT_LOADER_H

#include "ObjectGuid.h"

namespace PbotLoader
{
    // Query pbot_roster for this owner and enqueue an async reload of every rostered bot that is not
    // already live (double-spawn guard, A.3). Called from PbotPlayerScript::OnLogin.
    void ReloadOwnerRoster(ObjectGuid ownerGuid);

    // Startup orphan sweep (A.4 risk 1): permanently retire roster rows whose owner character no
    // longer exists. Optional per spec; cheap and prevents accumulating dead bot characters. Called
    // from PbotWorldScript::OnStartup.
    void SweepOrphansAtStartup();

    // Non-blocking poll of pending async loads. MUST be called once per world tick from
    // PbotWorldScript::OnUpdate (the single integration point into the world loop).
    void PumpPendingLoads();

    // Review finding CRITICAL: true while a reload for this bot guid has been submitted but its async
    // DB work has not yet completed. RetireBot consults this before touching a non-live bot so it can
    // never delete a character row out from under an in-flight LoadFromDB.
    bool IsReloadPending(ObjectGuid botGuid);

    // Starts the reload of ONE known roster entry. Exposed so the world population can be brought
    // back a few bots at a time rather than all at once — see PbotMgr::TickReloadQueue for why that
    // matters (the whole-population version tripped the anti-freeze watchdog and crashed).
    void EnqueueRosterReload(ObjectGuid ownerGuid, ObjectGuid botGuid, uint32 accountId, uint8 classId);

    // Defer a retirement until the in-flight reload for this bot lands. Consumed by OnHolderReady,
    // which re-dispatches to PbotMgr::RetireBot once the bot is either live or safely torn down —
    // whichever, the pending flag is cleared by then so the retire path is race-free.
    void RequestRetireOnLoad(ObjectGuid botGuid);
}

#endif // TRINITYCORE_PBOT_LOADER_H
