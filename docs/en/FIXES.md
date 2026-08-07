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

## 07 — NPCs that should patrol but stand frozen

**File:** `sql/07_npc_frozen_patrols.sql`

**Symptom:** a guard or wandering mob never moves. Not a pathing problem — the
creature never starts moving at all.

**Root cause:** `creature.MovementType = 2` means "follow a waypoint path", and
the path id comes from `creature_addon.PathId`. When the spawn has no addon row,
or PathId is 0, or PathId names a `waypoint_path` that does not exist,
`WaypointMovementGenerator::DoInitialize()` cannot load a path and returns false.
The generator never initialises, so the creature stands still for as long as the
grid is loaded, logging `couldn't load path for ...` each time.

**Scale:** 3,924 of 10,993 waypoint spawns — about **a third of every patrolling
NPC in the world**. The spawn row is the anomaly, not the template: 3,773 of them
belong to a `creature_template` whose own MovementType is 0 (idle).

**Fix:** make the spawn honest — `wander_distance > 0` becomes random movement in
that radius, otherwise idle. This is the same mapping TrinityCore applies when it
repairs other contradictory MovementType/wander_distance combinations at load.
Real patrol routes cannot be recovered; the waypoint data was never there. **Needs
a restart.**

---

## 08 — Creatures that cannot appear at all

**File:** `sql/08_npc_missing_models.sql`

**Symptom:** the log repeats `Creature (Entry: N) has no model defined ... can't
load.` The creature does not exist in the world — it cannot be seen, targeted or
killed.

**Root cause:** a creature needs at least one `creature_template_model` row.
889 spawned entries across 1,908 spawns had none.

**Why it matters:** most are invisible utility NPCs — kill-credit counters and
quest-objective bunnies. They are *meant* to be invisible, but they still have to
exist: a quest objective that counts kills of a credit NPC can never complete if
that NPC cannot spawn.

**Fix:** give every spawned entry with no model the canonical invisible display
`11686` — the model TrinityCore's own invisible stalkers use. Scope is limited to
entries that are actually spawned. **Needs a restart.**

---

## 09 — Spawns that cannot exist, or are somewhere unreachable

**File:** `sql/09_npc_broken_spawns.sql`

Four defects, each with its own backup:

* **no `creature_template`** — the engine skips them; a template cannot be
  reconstructed from a spawn row, so they are removed (261).
* **at the map origin (0,0,0)** — never a real placement (146). Some of these
  entries are also vehicle passengers via `vehicle_template_accessory`; that
  mechanism does not read the `creature` table, so the passenger is unaffected.
* **outside the map grid** — a map is 64×64 cells of 533.33 yd, so every valid
  coordinate is within ±17066.66 (2).
* **fallen through the world (z ≤ −2000)** — these are **lifted, not deleted**,
  to the average z of healthy spawns within 60 yards. `GetMapHeight()` searches
  about 50 yards and snaps the creature to the ground, so a near-enough estimate
  self-corrects. Only spawns with fewer than 3 neighbours to derive a height from
  are removed (86).

**Needs a restart.**

---

## 10 — References pointing at nothing

**File:** `sql/10_npc_orphan_references.sql`

`creature_addon` rows naming a missing waypoint path (387), addon rows for a
spawn that no longer exists (26), and formations whose leader or member is gone
(237 + 355). A formation with a missing leader never forms up, so the members
that should march behind it stand on their own spawn points instead.

**Run after 07 and 09** — 09 removes broken spawns, which turns their addon rows
into orphans that this file then collects. **Needs a restart.**

---

## hotfixes/01 — Eight maps where nothing spawns at all

**File:** `sql/hotfixes/01_map_difficulty_unlock.sql`
**Applies to the `hotfixes` database, not `world`.**

**Symptom:** eight maps are completely empty in game, and the log repeats
`Table \`creature\` has creature (GUID: N) that is not spawned in any difficulty,
skipped.` thousands of times.

**Root cause:** `ObjectMgr::LoadCreatures` builds the set of legal difficulties
for each map from `sMapDifficultyStore`, then keeps only the `spawnDifficulties`
tokens that are in it. For these maps the intersection is empty — six are retired
scenarios whose MapDifficulty records Blizzard removed from the client data, and
two have a record for a difficulty that is not in the spawn strings. The spawn
rows are fine; the map has no difficulty to spawn them into.

**Fix:** add one MapDifficulty row per map with **DifficultyID 0**. DB2 stores are
file-plus-database — `DB2Store::LoadFromDB` runs the loader twice, once for
`VerifiedBuild > 0` and once for `<= 0` — so a custom row with VerifiedBuild 0 is
merged into the store by design.

**Do not use DifficultyID 1.** `Difficulty.db2` has no record 0, so
`GetDefaultMapDifficulty()` filters these rows out and they cannot influence how
an instance is created or scaled. A real difficulty *is* selectable and would
override Blizzard's own tuning for the map.

**Recovers:** 1,218 creature and 1,073 gameobject spawns, including **Darkmaul
Citadel** — the Exile's Reach dungeon, which had never spawned a single NPC.
Nothing is broadcast to game clients: those come from the separate `hotfix_data`
table, which this file does not touch. **Needs a restart.**

---

## Applying & reverting

Apply all: `scripts/apply_fixes.sh` (add `HOTFIXES_DB=hotfixes` to include
`sql/hotfixes/`). Every file is safe to run twice — backups are created only if
absent, so a second run cannot overwrite your original values — and every file
ends with a revert block in comments. Always back up your `world` DB first.

Every file in this document was verified by rolling a copy of a real database
back to its broken state, applying the fix, confirming the defect count reaches
zero, applying it a second time, and then running the documented revert to check
the original numbers come back exactly.
