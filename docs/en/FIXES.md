# Community Fixes — details (EN)

> [Русский](../ru/FIXES.md)

All fixes are **additive**, **reversible**, and placed on **empty/custom ID
bands** so they don't collide with a stock TrinityCore install. Files live in
[`../../sql/`](../../sql/).

---

## 01 — Earthen intro dead-lock (phantom quest)

**File:** `sql/01_quest_earthen_intro_fix.sql`

**Symptom:** a fresh **Earthen** (allied race) character finishes the first quest
of the Isle of Dorn "Awakening" intro, then no NPC offers anything — the intro
is a linear phased corridor, so the player is stranded.

**Root cause:** the chain is
`79200 "Who am I?" → 79201 "The Analysis Interface" → 83328 "The Analysis Interface"`.
Quest **79201 is a deprecated duplicate** (identical title to 83328) with **no
quest-starter and no quest-ender** anywhere — it cannot be obtained or completed.
But 83328 (Foreman Uzjax) is gated on 79201 (`PrevQuestID = 79201`), so the chain
dead-locks.

**Fix:** bypass the phantom → chain flows `79200 → 83328` directly.
Reload live with `reload quest_template` (no restart).

**Bug class:** a *real but giver-less/ender-less* quest set as another quest's
`PrevQuestID` hard-blocks the chain. Note that sentinel `PrevQuestID` values
(`999999`, `99999`, …) are **not** this bug — those IDs don't exist, so the
engine skips the prerequisite. Only prerequisites that **exist but are
un-obtainable** block.

---

## 02 — Quest-blocking GameObjects

**File:** `sql/02_quest_gameobject_unblock.sql`

+28 `gameobject_template` and +89 `gameobject` spawns (guid band `8500000+`)
for objects that quests need but that were missing from the base DB, unblocking
~10 quests. DisplayIDs sourced authentically; coordinates converted from map%
to world via `UiMapAssignment` for build 68275, Z/orientation taken from the
nearest existing spawn on the same map. **Needs a worldserver restart.**

---

## 03 — Legacy-raid instance bindings

**File:** `sql/03_raid_instance_bindings.sql`

Binds 5 raids — Dragon Soul (967), End Time (938), Hour of Twilight (940),
Well of Eternity (939), Throne of the Four Winds (754) — to their compiled
instance script. This enables **lockout**, **DungeonEncounter journal
completion**, and **boss-state persistence** (tracked/clearable).

**Limitation:** these instance scripts are stubs; they do **not** add boss combat
AI (no `boss_*.cpp` exists for them). Bosses stay on default AI. **Needs a
restart.**

---

## 04 — Harandar graveyards

**File:** `sql/04_harandar_graveyards.sql`

12 `world_safe_locs` + 12 `graveyard_zone` links + 12 Spirit Healers (creature
guid band `11000773+`) for **Harandar (map 2694)**, one of the newest zones, so
death/resurrect works there. Cherry-picked as a self-contained additive file.
**Needs a restart.**

---

## Applying & reverting

Apply all: `scripts/apply_fixes.sh`. Each SQL file includes a revert block in
comments. Always back up your `world` DB first.
