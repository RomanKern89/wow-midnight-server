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

## 05 — Holidays drifted off the real calendar

**File:** `sql/05_world_events_realign.sql`

**Symptom:** by 2026 the yearly holidays fire 2-4 days early and the Darkmoon
Faire is about two weeks out of step.

**Root cause:** TrinityCore stores a holiday as an anchor date plus a fixed
`occurence` in **minutes**. A stock TDB anchors these in 2022-2023, and 525600
minutes is 365 days flat — no leap quarter — so even fixed-date holidays slide
roughly a day every four years. Worse, anything the real calendar defines by
*rule* (Darkmoon Faire is the first Sunday of the month, Noblegarden follows
Easter, the Lunar Festival follows the lunar new year) drifts away from that rule
entirely.

**Fix:** re-anchor each holiday to its next genuine occurrence, so later years
land correctly too. The file backs the table up before touching it. **Needs a
restart** — `game_event` is read at startup.

**Note when checking your work:** an event is active when
`MOD(minutes since start, occurence) < length` — *not* when "now" falls between
`start_time` and `end_time`. Those two columns bound the whole repeating series,
not one occurrence.

---

## 06 — Stray spawns (NPCs standing where they don't belong)

**File:** `sql/06_remove_stray_spawns.sql`

**Symptom:** raid bosses standing in starter zones, event props at the Stormwind
gates with no event attached, and Blizzard's own QA test creatures loose in the
world.

**Root cause:** an artifact of sniffed data — a client saw a creature somewhere
during a scripted moment, and the capture recorded it as a permanent resident.

**Why it matters:** a level-90 boss at the Stormwind gates is sudden death for
anything levelling past it. The companion bots in this repo were dying to exactly
that.

**Fix:** removes 30 such spawns. The file backs up every row it deletes first, so
the revert is a single INSERT. **Needs a restart.**

**How they were identified:** these legacy entries often 404 on external
databases because they are not in live retail data at all. The company an NPC
keeps is the more reliable signal — the neighbours around a spawn identify its
true home.

---

## Applying & reverting

Apply all: `scripts/apply_fixes.sh`. Every file is safe to run twice and ends
with a revert block in comments. Always back up your `world` DB first.
