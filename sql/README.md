# Community Fix SQL

Original, additive server-side fixes authored for a **TrinityCore master** world
database at retail build **12.0.7.68275 (Midnight)**. Each file is idempotent
where possible (`INSERT IGNORE`) and ships with a revert comment.

| File | What it fixes | Reload |
|------|---------------|--------|
| `01_quest_earthen_intro_fix.sql` | Earthen allied-race intro dead-lock (phantom quest 79201 blocking the chain) | `reload quest_template` (live, no restart) |
| `02_quest_gameobject_unblock.sql` | +28 `gameobject_template` and +89 `gameobject` spawns (guid band `8500000+`) that unblock ~10 quests missing their objective object | worldserver restart |
| `03_raid_instance_bindings.sql` | Binds 5 legacy raids to their instance script for lockout + journal + boss-state tracking (stubs — no boss combat AI) | worldserver restart |
| `04_harandar_graveyards.sql` | 12 `world_safe_locs` + 12 `graveyard_zone` links + 12 Spirit Healers so the newest zone (Harandar, map 2694) is resurrect-able | worldserver restart |
| `05_world_events_realign.sql` | Re-anchors every holiday to its real date. A default TDB is anchored in 2022-2023, and by 2026 the yearly events run 2-4 days early while the Darkmoon Faire is two weeks out | worldserver restart |
| `06_remove_stray_spawns.sql` | Removes 30 stray spawns — raid bosses in starter zones, un-gated event props at the Stormwind gates, and Blizzard's own QA test creatures. Each backs itself up first | worldserver restart |

## Apply

```bash
DB_USER=trinity DB_PASS=trinity DB_NAME=world ../scripts/apply_fixes.sh
# or a single file:
mysql -u trinity -p world < 01_quest_earthen_intro_fix.sql
```

> These files use empty/custom ID bands (GameObject guids `8500000+`,
> creature guids `11000773+`) so they will not collide with a standard
> TrinityCore install. Always back up your DB before applying.
>
> Object/quest names and IDs are © Blizzard Entertainment. These files are
> server-configuration fixes only and contain no game assets.
