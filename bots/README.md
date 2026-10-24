# Companion Bots — a populated world without players

Custom TrinityCore scripts that put **socket-less fake players** into the world:
real `Player` objects with a null-socket `WorldSession`, driven by their own
decision loop. They fight, gather, craft, quest, repair, trade at the auction
house, group up and talk. Nobody has to be logged in for any of it to happen.

Written for **TrinityCore master, retail 12.0.7 (build 68974)**. Almost all of it
lives under `src/server/scripts/Custom/Bots/` and uses public engine APIs, with
**one nine-line core patch** — `bots/core-patch/`, applied automatically by the
Docker build. A socket-less bot never runs the login pipeline that loads its
social list, `m_social` is private with no setter, and the first piece of engine
code to consult it dereferences null.

---

## What a bot actually does

Each bot runs one decision ladder per world tick. Exactly one stage claims the
tick, so the tally of stages *is* the bot's day — and that tally is readable
live with `.pbot world time`:

```
 29%  fighting        22%  travelling      20%  questing
 14%  gathering        8%  idle             3%  town errands
```

| Stage | Behaviour |
|---|---|
| **Death** | Dies, waits, revives. After two deaths in one place it moves house — to a *different* zone, because the zone it is dying in is the trap — and the log says so when there is no refuge outside it |
| **Retreat** | Breaks off a fight already lost. Decided once and then honoured, because a bot that re-decides every tick swings, steps back, swings again and dies anyway |
| **Combat** | Class rotations through the engine's own `SpellHistory`; ranged classes hold a 20–30 y band, melee close. Cast damage now reaches its target: before the visibility fix every spell was discarded after `CheckTarget` and only the staff swing worked |
| **Rest** | Eats and sits when hurt |
| **Loot** | Loots its own kills |
| **Hunt** | Picks a fair fight from local wildlife; hostile players outrank wildlife |
| **Gather** | Mines and picks herbs — the modern `GAMEOBJECT_TYPE_GATHERING_NODE`, not just old-style chests |
| **Town errands** | Sells junk, repairs per slot (weapons first), visits the auction house, walks to a forge, buys the profession tool a recipe needs. Whichever destination is *nearest* wins; worn-out gear jumps the queue |
| **Market** | Reads its mail, lists surplus, buys upgrades it can actually wear, and wears them. Gear is re-checked every time a bot is reloaded, so bots that predate a gear rule are brought up to their level |
| **Craft** | Practises one profession per bot, walks to a workbench when a recipe needs one, and buys the tool when the recipe needs that instead |
| **Quest** | Takes quests, goes where the objectives are — kill targets by their spawn points when a quest has no map marker, item objectives by the creatures that drop them — and returns to hand them in. A giver it could not reach is left alone for 15 minutes instead of being asked for again |
| **Delivery** | A finished quest whose taker stands on another continent is collected by a deliberate trip, once the waiting work is worth the journey |
| **Travel / roam** | Stepped long-distance movement, mounts for the long stretches, migrates zone when it outgrows one |

Bots also have **personality**: greed, diligence, aggression and wanderlust are
derived from the guid, so one stops for every ore vein and another walks past
them for its whole career. Over an hour that reads as different players.

---

## What was measured, and what it cost to learn

Every number below is a 30–40 minute soak over 60 bots, before → after. They are
here because each one was a *defect that looked like working software*.

| | before | after | what was actually wrong |
|---|---|---|---|
| Deaths per kill | 15–20% | **4.4%** | Not a balance problem. A socket-less bot is invisible to the engine's own targeting, so every spell target was discarded before damage; with that flag fixed, 8,305 kills against 398 deaths over 6.3 h |
| Deaths | 205 | **9–17** | Not combat weakness. Bots were pinned in loops and died where they stood |
| Quest accepts that did nothing | 3498 | **~1** | `AddQuestAndCheckCompletion` can decline after `CanTakeQuest` says yes; the code counted the attempt |
| Day spent on town errands | 20.7% | **3.3%** | Errands never *finished*: the travel step asked the motion generator whether the bot was moving, and a generator outlives its spline |
| Vendor transactions per 30 min | single digits | **74–90** | Same cause — bots now arrive |
| Nodes gathered per 40 min | 6 | **84** | Modern ore and herbs are a game-object type the filter had never accepted, so bots had literally never seen one |
| Bots with a craft skill | 0 | **60 of 60** | Setup was hung on bot *creation*, which stopped happening once the population became persistent |

Four separate walls stood between a bot and a crafted item, each hidden behind
the last: a cast interrupted by walking, a missing forge, sitting on a mount,
and — the root of all of it — never being allowed to look at a gathering node.

---

## Commands

Full manual: **[COMMANDS.md](COMMANDS.md)** — every command, every argument,
the config switches, the chat vocabulary companions understand, and what to look
at when nothing seems to happen.

The short version, all GM-gated (`RBAC_PERM_COMMAND_SERVER`) and all usable from
the server console:

```
.pbot world populate <count>     spread a population across the world
.pbot world list                 who is alive and where
.pbot world time [reset]         where the day goes, per activity
.pbot world craft                craft -> recipes known -> materials -> workbench
.pbot world market               goods, auctioneer reach, mail
.pbot world upkeep               bags, durability, vendor reach
.pbot world combat               kill/death and target selection
.pbot spawn <count>              companion bots that follow YOU
```

The diagnostics matter as much as the behaviour. Every fix above was found by
one of them contradicting an assumption.

---

## Building them in

The Docker image in this repository does it for you — `docker/Dockerfile` copies
`bots/src` into the TrinityCore tree before compiling:

```dockerfile
RUN git -C /src apply /core-patch/0001-player-initialize-empty-social.patch
COPY bots/src /src/src/server/scripts/Custom/Bots/
COPY bots/custom_script_loader.cpp /src/src/server/scripts/Custom/custom_script_loader.cpp
```

By hand, into an existing checkout — **all three steps**:

```bash
cd /path/to/TrinityCore
git apply /path/to/bots/core-patch/0001-player-initialize-empty-social.patch
cp /path/to/bots/src/* src/server/scripts/Custom/Bots/
cp /path/to/bots/custom_script_loader.cpp src/server/scripts/Custom/
cd build && cmake .. -DSCRIPTS=static && make -j$(nproc) && make install
```

The second file matters more than it looks. `bot_script_loader.cpp` follows the
standard per-directory convention and defines `AddBotsScripts()`, but the core
only ever calls `AddCustomScripts()` — and TrinityCore ships that as an **empty
stub**. Copy only the sources and you get a clean build, a server that starts
normally, and no `.pbot` command anywhere: the scripts are compiled in and never
registered.

TrinityCore collects source files at CMake **configure** time, so re-run `cmake`
after adding files, not just `make`.

**Cost:** a bot is a full `Player` tick. Sixty bots on five continents run
comfortably in the RAM figures given in `docker/INSTALL.md` — the memory goes on
the *maps and zones they occupy*, not on the bots themselves, so spreading them
thin costs more than stacking them.

---

## Verification status — read before deploying

The bot scripts are built and run continuously on a live server (Ubuntu 24.04,
gcc 13.3.0, TrinityCore `c9e3fd8df5cb`, `SCRIPTS=dynamic`) — that is where every
measurement above comes from.

**Verified end to end in Docker.** `docker compose build` produces an image with
the bots and the core patch in it; the stack comes up; the world initialises in
about 90 seconds; the scripts register and load their six tables (quest hubs,
population spots, repairers, auctioneers, workbenches, turn-in locations); and
`Pbot.WorldPopulation = 10` brings ten bots into the world at startup, which then
gather, learn recipes and break off fights they are losing.

Getting there took three attempts, and each failure was invisible from the source
alone:

1. **The build failed.** `pbot_mgr.cpp:349: error: 'class Player' has no member
   named 'InitializeEmptySocial'` — the core patch above, which had lived only in
   the live server's working tree. This README used to claim the bots patch
   nothing; the server's own version string had been printing `c9e3fd8df5cb+` for
   months, where the `+` means exactly this.
2. **The scripts compiled and never registered.** TrinityCore ships an empty
   `AddCustomScripts()`; see `custom_script_loader.cpp` above.
3. **The bots ran and said nothing.** `Logger.root = 5` means errors only, and
   TrinityCore's levels run the opposite way to intuition, so every line they
   wrote was discarded — a server that looked exactly like one with no bots in
   it. Fixed by `Logger.scripts.bots = 3,Console` in the config template.

**And one limit, measured the hard way:** twenty gigabytes is not enough for the
full retail world *plus* a scattered population. The host stopped answering SSH
while still replying to pings — starved rather than crashed, which reads as a
hang. See the RAM row in `docker/INSTALL.md`.

---

## Honest limitations

* **Quest turn-ins are still slow** — roughly 7-8 per hour, with about 30 finished
  quests waiting at any moment, most of them with a taker on another continent or
  inside a phase. Delivery trips and the taker-first rule raised this from a
  handful per session; the limit is now the bot's time, not reach.
* **Levels take about ten hours.** 60 bots produce ~1,300 kills an hour between
  them — about nine per bot — and a level needs dozens. That is pacing, not a
  defect, and it was left as it is on purpose.
* **Crafting is a trickle.** Tools are bought and workbenches are walked to, but
  only about ten bots in sixty hold materials for something at any moment.
* **Quest chains are not followed deliberately.** A bot continues a chain only
  when the next step happens to come from the NPC it just visited.
* **Bots still fight mostly in melee.** A cast now lands, but every class closes
  in while it is not casting, because the staff swing is the fallback that always
  works.
* **54 creature difficulty rows still leave a creature at 0 health** — named in
  `sql/12_zero_hp_templates.sql`. Their intended health is not derivable from the
  data: the modifier encodes absolute health per expansion and runs to 5e7.
* **About seven trips an hour still end at an NPC who exists in another phase.**
  The write-off is per bot and per spawn now, so it repeats far less than before,
  but it cannot be filtered before arrival — the grid is not loaded at the moment
  the destination is chosen.
* **Groups exist but do nothing together**, and no bot uses consumables.
* Battleground flag capture is written but has never been observed end to end.

These are listed because a reader deserves to know where the edge is. Everything
in the table above is measured; everything here is measured too.

---

## Licence and content

These scripts are original work under the repository's MIT licence. They contain
no game assets. Creature, quest and item identifiers referenced in comments are
© Blizzard Entertainment and appear only as the coordinates any server-side fix
must name.
