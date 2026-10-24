# What's new

Newest first. Every number here is a measurement from a live server or a real
deployment, not an estimate — including the ones that are unflattering.

---

## 2026-10-24 - The bots had never really seen the world

Every number below is from the live realm (60 bots) or from a database query,
and the section exists because one of these defects had been treated as a
"balance problem" for weeks.

### Fixed

**Casts did no damage, and the reason was not in the spell code.** A bot is a
`Player` with no socket, so it never runs the login or movement handshake that
sets `PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME`. Without that flag
`Player::CanNeverSee` returns true, `CanSeeOrDetect` answers false for every
object in the world, `IsValidAttackTarget` answers false, and
`Spell::AddUnitTarget` silently discards the target after `CheckTarget`
returns `BAD_TARGETS` - while the mana is still spent and the cast is reported
as started. Every nuke was a no-op and the population fought by melee only.
The flag is now set on all three paths that put a bot into the world.

Measured: kills 200 -> 1,016 per 25 minutes; deaths 27 -> 17; over a
6.3-hour run, **8,305 kills against 398 deaths - 4.4% per kill**, down from
15-20%. That is the number this change was for, and it was not reachable by
any amount of spell-flag tuning.

**Neutral traders were invisible.** The vendor filter required
`IsFriendlyTo`, which means reputation rank 4 or better. A neutral trader sits
at 3 - and neutral traders do trade. Repairers and auctioneers are now
accepted at `REP_NEUTRAL` and above; hostile ones are still refused. Verified
on the bots that had spent hours reporting a live repairer as
"NOT IN WORLD": they now repair at it. `Blacksmith Hammer` and the like began
appearing in bags the same day.

**Existing bots wore the kit they were born with.** `GearUp` ran only on the
creation path, which stopped happening once the population became persistent.
It now runs when a bot is reloaded as well, and it replaces worse slots
instead of only filling empty ones. 541 slots upgraded across 60 bots on the
first reload; a level-30 bot that had been wearing required-level-8 items now
wears level-30 ones.

**A bot that could not reach a questgiver asked for the same one again.**
163 give-ups in one window, one bot 29 times at the same NPC. A giver that
could not be reached is now ignored by that bot for 15 minutes: 163 -> 18.

**The cross-map lift outranked the quest module.** A bot that had just
arrived at a taker was teleported away to collect a different reward before it
could hand anything in. A taker for finished work within 30 yards now wins
the tick.

**Teleports to objectives.** `carried it to the objective` happened about 13
times an hour: a bot that could not walk out of the terrain was teleported to
its goal, which from outside is a player blinking across the map. The
teleport is gone - the destination is written off and the bot picks again -
and a bot genuinely under the terrain is lifted onto the ground it is
standing on, which is a coordinate correction rather than a journey.

**Death loops.** Relocation could pick the zone the bot was already dying in,
reset its death counter, and produce a series that read as 3+3+3+3 deaths. The
current zone is now excluded, the threshold is two deaths, and a relocation
that cannot find a refuge says so instead of staying silent.

**Phase was treated as a property of the spawn row.** One bot finding an NPC
in another phase wrote the spot off for every bot on the map. A phase is a
property of one character's quest state, so the memory is now per bot and per
spawn.

**`Server.log` grew by 392 MB an hour.** Every packet sent to a socket-less
bot logged `TC_LOG_ERROR("network.opcode", "Prevented sending of ...")` - one
line per packet, millions a day. The logger is routed out of the file and the
native `MaxFileSize` rotation is enabled as a second line of defence; growth
is now 0.

**23 million SQL error lines (2.2 GB) on every start.**
`ParseSpawnDifficulties` logs one line per spawn per difficulty its map does
not support, and the spawn table carries a 25-value list. Routed to
`DBErrors.log`, which is already size-capped; the console log now stays at its
300 MB cap.

**`systemctl start worldserver` did not start the realm.** The unit was
`Type=oneshot` + `RemainAfterExit=yes`, so `start` on an "active" unit is a
no-op and the tmux session it created lived outside the unit's control group.
It is a real supervisor now (`Type=simple`), which also restarts the world if
it dies. The deploy script was changed with it - killing the process directly
under a supervisor starts a second instance.

### Added

**Intent layer.** Six intents - fight, quest, gather, town, market, travel -
are scored every 30 seconds with per-bot personality weights and then
committed, so a bot finishes what it started instead of re-deciding every
tick.

**Cross-map delivery.** Finished quests whose taker stands on another
continent are collected by a deliberate trip, with a "worth the trip"
threshold so a bot does not cross the world for a single reward.

**Item objectives are work, not ballast.** A quest objective that names an
item is now resolved to the creatures that drop it: 4,955 items and 103,358
drop links from the loot tables. Hunt destinations grew from 14,852 creature
entries / 90,982 spawns to **24,303 / 226,239**.

**Quests without map markers get a destination.** When a quest carries no POI
data, the spawn points of its kill targets are used instead. Without this the
bot had no goal at all and hunted whatever was nearby - measured before the
change, 96% of its kills were creatures no held quest cared about.

**Profession tools are bought.** A recipe that needs a smith's hammer or a
mining pick is refused with `SPELL_FAILED_TOTEM_CATEGORY`, and walking to a
forge does not fix it. Bots now buy the tool at the vendor they are already
standing at - 11 tool categories across 2,872 vendor spawns.

**Two database files**, both self-backing-up and reversible:

* [`sql/12_zero_hp_templates.sql`](sql/12_zero_hp_templates.sql) - creature
  difficulty rows that leave a spawned creature with 0 health, so it dies to
  the first hit and any "health percent" logic reads zero. 9 rows are fixed
  from the same entry's healthy difficulty; 54 rows over 42 entries are named
  in the report instead of guessed at, because the modifier encodes absolute
  health per expansion (values run to 5e7) and an invented number would be
  wrong by orders of magnitude. The 86 rows with a tiny but non-zero modifier
  are deliberately untouched - they are one-hit objects by design.
* [`sql/13_creature_health_half.sql`](sql/13_creature_health_half.sql) -
  **optional balance tuning, not a fix.** Halves non-boss creature health
  (521,923 rows). It exists because on this bot-populated realm ordinary mobs
  took 130+ hits to kill - 20-100 damage against 3,000-1,400,000 health - so
  a bot died before finishing a fight. Measured: deaths per kill 31% -> 15.5%,
  kills per hour 435 -> 522. Do not apply it to a player server.

### Changed

`bots/src` gains ten files since the last publish - `pbot_bags`,
`pbot_belongings`, `pbot_delivery`, `pbot_flight` and `pbot_intent` - and
twenty-nine existing files were updated. The build is unchanged: the Docker
image still copies `bots/src` into the tree, and no registration file was
touched because none of the new files is a `ScriptObject`.

---

## 2026-08-07 — A world that lives, and a deployment that was finally built

### Added

**The companion bot system** — [`bots/`](bots/README.md) ([по-русски](bots/README.ru.md)).
72 scripts plus one nine-line core patch, compiled into the Docker image
automatically. Socket-less fake players with their own decision ladder: combat
rotations, breaking off lost fights, mining and herbing, smelting and crafting,
per-slot repair, auction selling and buying with the purchases actually worn,
questing with objective travel and turn-ins, grouping, chat, mounts, zone
migration, and progress that survives restarts. Personality (greed, diligence,
aggression, wanderlust) comes from the guid, so no two play alike.

Full control manual: [`bots/COMMANDS.md`](bots/COMMANDS.md) /
[`bots/COMMANDS.ru.md`](bots/COMMANDS.ru.md) — every command, argument, config
switch and chat verb, with what each is for.

**Diagnostics that are part of the product**, not scaffolding — `.pbot world
time` reports where the population's day actually goes, `.pbot world craft`
walks the chain from "has a profession" to "is standing at a workbench". Every
repair below was found by one of these contradicting an assumption.

**Two world-database fixes**, both self-backing-up and reversible:

* [`sql/05_world_events_realign.sql`](sql/05_world_events_realign.sql) — holidays
  re-anchored to their real dates. A stock TDB is anchored in 2022-2023 and by
  2026 runs the yearly festivals 2-4 days early, with the Darkmoon Faire two
  weeks out. Also writes down the engine's own "is this event running" predicate,
  because the obvious query reports every holiday as active at once.
* [`sql/06_remove_stray_spawns.sql`](sql/06_remove_stray_spawns.sql) — 30 spawns
  that stand where they do not belong: Uldum's Prince Sarsarun and a raid boss at
  the Stormwind gates, un-gated Scourge Invasion props, Blizzard's own QA test
  creatures, and Naxxramas and Icecrown bosses scattered across Kalimdor,
  Quel'Danas and Pandaria. Includes how they were found and — just as important —
  what must never be touched: classification 6 is 45567 invisible script helpers
  the world needs.

### Fixed — in the bots

Measured over 30-40 minute soaks of 60 bots, before → after:

| | before | after | what was actually wrong |
|---|---|---|---|
| Deaths | 205 | **9-17** | Not combat weakness — bots were pinned in loops and died where they stood |
| Quest accepts that did nothing | 3498 | **~1** | The engine can decline after the eligibility check passes; the code counted the attempt |
| Day spent on town errands | 20.7% | **3.3%** | Errands never *finished*: the travel step asked the motion generator whether the bot was moving, and a generator outlives its spline |
| Vendor transactions / 30 min | single digits | **74-90** | Same cause — the bots now arrive |
| Nodes gathered / 40 min | 6 | **84** | Modern ore and herbs are a game-object type the filter had never accepted |
| Bots with a craft skill | 0 | **60 of 60** | Setup hung on bot *creation*, which stopped happening once the population became persistent |

Four separate walls stood between a bot and a crafted item, each hidden behind
the last: a cast interrupted by walking, a missing forge, sitting on a mount,
and — the root of it — never being allowed to look at a gathering node.

### Fixed — in the deployment

The image had been published without ever being built. Building it found three
faults, none of them visible in the sources:

* **A missing core patch.** `Player::InitializeEmptySocial()` lived only in the
  live server's working tree. The build failed on the first attempt. The
  documentation had claimed the bots patch nothing — while the server's own
  version string had been printing `c9e3fd8df5cb+` for months, where the `+`
  means exactly this.
* **An empty script loader.** TrinityCore ships `AddCustomScripts()` as a stub;
  the bots compiled, linked and were never called. A clean build of a server
  where `.pbot` does not exist.
* **A log level that silenced them.** `Logger.root = 5` means errors only and
  TrinityCore's levels run the opposite way to intuition, so everything the bots
  wrote was discarded — a server indistinguishable from one with no bots in it.

Also: the build now fetches **one commit** instead of the whole history (65+
minutes → 6m35s), and the revision is pinned so the image built tomorrow is the
image that was tested today.

### Changed — system requirements

The RAM figure for a bot population was an estimate and it was wrong. Ten bots on
a 20 GB host running the full retail world took it to a standstill — still
answering pings, no longer answering SSH. Now **+6 / +8 GB**, with the reason
written down: the cost is the maps and zones a population keeps loaded, so ten
bots in ten zones cost more than sixty in one.

### Known limitations

Listed because a reader deserves to know where the edge is:

* **Quest turn-ins are slow** — a handful per session, with roughly 260 finished
  quests undelivered on a mature server, most of their takers on another
  continent. Machinery in place, throughput unsolved.
* **Crafting is a trickle** — about 10 bots in 60 hold materials at any moment,
  and nine of those need a workbench that is never the nearest errand.
* **Quest chains are not followed deliberately.**
* **Groups form but do nothing together**; no bot uses consumables.
* Battleground flag capture is written but never observed end to end.

---

## Earlier

See the git history on the `docker` branch for the Compose deployment work
(build fixes for third-party dumps, hotfix DB support, the beginner INSTALL
guide) and `main` for the original documentation and the first four SQL fixes.
