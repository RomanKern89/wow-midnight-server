# DB dumps go here

Drop your TrinityCore database dumps (build **12.0.7.68275**) into this folder.
On `docker compose up`, `db-init` imports them and applies our community fixes.

| File | Imported into | Needed for |
|------|---------------|------------|
| `world.sql` | `world` schema | quests, spawns, loot, vendors (+ our fixes on top) |
| `hotfixes.sql` | `hotfixes` schema | **required** — modern retail worldserver crash-loops without it |

Name them so they match: a file containing `hotfixes` → hotfixes DB; any other
`.sql` (or one named `world*`) → world DB.

We ship **no** dumps: they derive from Blizzard's client and cannot be
redistributed (see [../../DISCLAIMER.md](../../DISCLAIMER.md)). Produce your own,
e.g. from an existing TrinityCore install:

```bash
mysqldump --single-transaction --no-tablespaces --routines world    > world.sql
mysqldump --single-transaction --no-tablespaces --routines hotfixes > hotfixes.sql
```

`*.sql` here is git-ignored so you never accidentally commit game data.
