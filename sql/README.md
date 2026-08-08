# Community Fix SQL

Original, additive server-side fixes authored for a **TrinityCore master** world
database at retail build **12.0.7.68974 (Midnight)**.

Every file is **safe to apply twice** — each one clears its own custom ID band,
uses `INSERT IGNORE`, or creates its backup table only if absent — and every file
ends with a **revert comment** that undoes exactly what it did.

All **twelve** were verified by rolling a copy of a real database back to its
broken state, applying each fix, confirming the defect count reaches zero,
applying it a second time with the backup tables proven untouched, and finally
running the documented revert to check the original numbers come back exactly.

| File | What it fixes | Reload |
|------|---------------|--------|
| `01_quest_earthen_intro_fix.sql` | Earthen allied-race intro dead-lock (phantom quest 79201 blocking the chain) | `reload quest_template` (live, no restart) |
| `02_quest_gameobject_unblock.sql` | +28 `gameobject_template` and +89 `gameobject` spawns (guid band `8500000-8500088`) that unblock ~10 quests whose objective object is absent from the database. Spawn Z is derived from the nearest existing spawn, so an object may sit slightly high or low | worldserver restart |
| `03_raid_instance_bindings.sql` | Binds 5 legacy raids to their instance script for lockout + journal + boss-state tracking (stubs — no boss combat AI) | worldserver restart |
| `04_harandar_graveyards.sql` | 12 `world_safe_locs` + 12 `graveyard_zone` links + 12 Spirit Healers so the newest zone (Harandar, map 2694) is resurrect-able | worldserver restart |
| `05_world_events_realign.sql` | Re-anchors every holiday to its real date. A default TDB is anchored in 2022-2023, and by 2026 the yearly events run 2-4 days early while the Darkmoon Faire is two weeks out | worldserver restart |
| `06_remove_stray_spawns.sql` | Removes 30 stray spawns — raid bosses in starter zones, un-gated event props at the Stormwind gates, and Blizzard's own QA test creatures. Each backs itself up first | worldserver restart |
| `07_npc_frozen_patrols.sql` | NPCs set to patrol with no route exist, but never move. About **a third of every waypoint mover** on a stock database (3,924 of 10,993 measured). Re-points them at idle or random movement | worldserver restart |
| `08_npc_missing_models.sql` | Creatures spawned in the world that have no model, so the engine refuses to load them at all — mostly kill-credit and quest-objective NPCs, which silently breaks those objectives. 889 entries / 1,908 spawns measured | worldserver restart |
| `09_npc_broken_spawns.sql` | Spawns with no `creature_template`, at the map origin, outside the map grid, or fallen through the world. Sunken ones are lifted to the local ground where neighbours allow it, deleted only when they cannot be placed | worldserver restart |
| `10_npc_orphan_references.sql` | `creature_addon` rows pointing at a missing waypoint path or a deleted spawn, and formations whose leader or member no longer exists. Run after 07 and 09 | worldserver restart |
| `11_duplicate_spawns.sql` | The same NPC spawned two or more times in the same spot — 2,598 groups / 3,403 redundant spawns. Only removes copies identical in all thirteen fields that decide what a player sees; different phases, models, equipment and event/pool membership are left alone. Run after 07-10 | worldserver restart |

### `sql/hotfixes/` — a different database

| File | What it fixes | Apply to |
|------|---------------|----------|
| `hotfixes/01_map_difficulty_unlock.sql` | Eight maps where **nothing spawns at all** because the map has no legal difficulty for its spawns to live in — including Darkmaul Citadel, the Exile's Reach dungeon. Recovers 1,218 creature and 1,073 gameobject spawns | the **`hotfixes`** DB, then restart |

## Apply

```bash
# every file in sql/ -> the world database
DB_USER=trinity DB_PASS=trinity DB_NAME=world ../scripts/apply_fixes.sh

# include sql/hotfixes/ as well (separate database, opt-in)
DB_USER=trinity DB_PASS=trinity DB_NAME=world HOTFIXES_DB=hotfixes ../scripts/apply_fixes.sh

# or a single file:
mysql -u trinity -p world    < 01_quest_earthen_intro_fix.sql
mysql -u trinity -p hotfixes < hotfixes/01_map_difficulty_unlock.sql
```

Apply `07`-`10` in numerical order: `10` cleans up references that `09` turns
into orphans when it removes a broken spawn.

> These files use empty/custom ID bands (GameObject guids `8500000+`,
> creature guids `11000773+`) so they will not collide with a standard
> TrinityCore install. Always back up your DB before applying.
>
> Object/quest names and IDs are © Blizzard Entertainment. These files are
> server-configuration fixes only and contain no game assets.
