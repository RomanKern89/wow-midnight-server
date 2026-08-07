# What's new

Newest first. Every number here is a measurement from a live server or a real
deployment, not an estimate — including the ones that are unflattering.

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
