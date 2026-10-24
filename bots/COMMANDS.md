# Bot control — every switch, and what it is for

Two kinds of bot live in this system and they are managed differently:

* **World bots** — an autonomous population that belongs to nobody. Turned on by
  the server operator, they live their own lives whether or not anyone is logged
  in. All commands below start `.pbot world`.
* **Companions** — bots that belong to *you*, follow you, and take orders in chat.
  Commands start `.pbot`, and you talk to them in plain Russian.

Everything here is GM-gated except the companion commands, which need the
per-command permissions listed at the end.

---

## 1. Switching the population on

### The permanent way — config

```ini
# worldserver.conf (in this repo: docker/etc/worldserver.conf.template)

# How many autonomous bots to bring into the world at startup.
# 0 disables them entirely and the server costs nothing extra.
Pbot.WorldPopulation = 10

# How many different maps the population may spread across. Default 5.
# This is the memory dial, not the bot count: the cost of a population is the
# maps and zones it keeps loaded, so ten bots on ten maps cost far more than
# sixty on one. Lower it if the host is tight.
Pbot.WorldPopulationMaps = 5
```

Restart the server; the bots appear in staged batches over the first minutes.
Staged on purpose — spawning sixty players in one tick trips the core's 60-second
anti-freeze watchdog and takes the server down with it.

### The immediate way — console

Attach to the running server and type:

```bash
docker attach tc-worldserver     # leave without stopping it: Ctrl-P Ctrl-Q
```

```
.pbot world populate 20
```

---

## 2. Creating and removing world bots

| Command | Arguments | What it does |
|---|---|---|
| `.pbot world populate` | `<count> [level] [mapFilter] [maxMaps]` | Spreads `count` bots across the world's population spots — different zones, different continents, both factions. `mapFilter` restricts to one map id; `maxMaps` overrides `Pbot.WorldPopulationMaps` for this call. **The normal way to fill a world.** |
| `.pbot world spawn` | `<count> [level] [class]` | Spawns bots *around where you are standing*. In-game only. Useful for watching behaviour up close. |
| `.pbot world spawnat` | `<map> <x> <y> <z> <count> [level]` | The same, at explicit coordinates. Console-friendly — this exists so an operator with no game client can create and observe bots entirely from a terminal. |
| `.pbot world clear` | — | Removes every world bot. Their characters are deleted; this is not a pause button. |

`level` defaults to a spread appropriate to the spot. `class` accepts a class
token (`warrior`, `mage`, …); omit it and classes are picked at random.

---

## 3. Watching what they do

These are the reason the bots work at all. Every defect listed in
[`README.md`](README.md) was found by one of these contradicting an assumption —
not by reading code.

| Command | Answers |
|---|---|
| `.pbot world list` | Who is alive, what level, where, and what each is doing right now |
| `.pbot world time` | **Where the population's day actually goes**, as a share of ticks per activity: fighting, travelling, questing, gathering, town errands, idle. Add any argument (`.pbot world time reset`) to zero the counters and start a fresh window |
| `.pbot world craft` | The chain from "has a profession" to "is standing at a workbench": how many bots have a craft, know a recipe, hold materials for one, and need a forge. Whichever number collapses is the answer |
| `.pbot world market` | Goods worth selling, auctioneer reach, mail waiting. The three things that fail independently in an economy |
| `.pbot world upkeep` | Bags free, gear durability, junk carried, whether a vendor or repairer is reachable |
| `.pbot world combat` | Kills, deaths, and how targets are being chosen |
| `.pbot world quests` | Quest logs: how many held, how many complete, the newest one |
| `.pbot world nodes` | What gathering nodes a bot can see, and why it can or cannot open them |
| `.pbot world who` | Each bot's personality — greed, diligence, aggression, wanderlust |
| `.pbot world bands` | Level bands across the population, and who has outgrown their zone |
| `.pbot world hunt` | Target selection: what is in range and which of it passes the fair-fight filter |
| `.pbot world bglist` / `bgdiag` | Battleground templates, and per-bot queue/objective state |

### Reading `.pbot world time`

```
 29%  fighting        22%  travelling      20%  questing
 14%  gathering        8%  idle             3%  town errands
```

Exactly one stage claims each tick, so this tally *is* the day. A healthy
population looks roughly like the above. Two shapes that mean trouble:

* **Town errands above ~15%** — errands are being started and not finished.
  Bots are walking somewhere they never arrive.
* **Idle above ~15%** — they have run out of things to do where they are, or a
  stage above them is silently claiming nothing.

---

## 4. Moving them about

| Command | Arguments | What it does |
|---|---|---|
| `.pbot world tele` | `<map> <x> <y> <z>` | Teleports the whole population to a point. For staging a test, not for play |
| `.pbot world bg` | `<battlemasterListId>` | Queues the population for a battleground |

---

## 5. Companions — bots that belong to you

| Command | Arguments | Notes |
|---|---|---|
| `.pbot spawn` | `<class>` | Creates a companion of that class, levelled and geared to match you, and it follows you. In-game only |
| `.pbot dismiss` | — | Sends the companion away; the character survives |
| `.pbot retire` | — | Deletes the companion for good |
| `.pbot list` | — | Your companions, their level, health and stance |
| `.pbot selftest` | — | Runs the built-in behaviour checks and reports what passed |

The older creature-based companions are still there: `.bot add`, `.bot remove`,
`.bot info`.

### Talking to them

Companions listen to ordinary chat. Say the word, they do the thing — no command
prefix, no syntax:

| Say | They |
|---|---|
| `за мной`, `следуй`, `фолов` | Resume formation and follow |
| `стой`, `стоять`, `на месте`, `ждать`, `жди` | Hold position |
| `ко мне`, `сюда` | Come back and re-form now |
| `в бой`, `атакуй`, `бей`, `фас`, `атака` | Attack your current target immediately |
| `ассист`, `помогай`, `помощь` | Stance: fight what you fight (the default) |
| `защита`, `защищай`, `прикрой` | Stance: go after whatever is attacking *you* |
| `пассив`, `не атакуй`, `не лезь`, `не нападай` | Stance: never engage on their own |
| `стоп`, `отбой`, `хватит` | Break off the current fight — the stance is unchanged |
| `статус`, `доклад`, `как дела`, `инфо` | Report level, health and stance |
| `команды`, `хелп` | List what they understand |

Address a specific bot by name, or the lot of them with `все`, `всем`, `боты`,
`отряд`.

---

## 6. Permissions

| Commands | Permission |
|---|---|
| Everything under `.pbot world` | `RBAC_PERM_COMMAND_SERVER` — GM only, console allowed |
| `.pbot spawn` | 1010 |
| `.pbot dismiss`, `.pbot retire` | 1011 |
| `.pbot list` | 1012 |

The world commands are deliberately locked down: each bot costs a full `Player`
tick, and they belong to nobody who could clean them up.

> If a custom permission "does not exist" for a GM who plainly has rights, it was
> granted at security level 0 only. Grant it at the account's actual
> `SecurityLevel` as well, reload RBAC, and log the account out and back in.

---

## 7. When nothing seems to happen

**The bots say nothing at all.** Almost always the log level. TrinityCore's
levels run the opposite way to intuition and `Logger.root = 5` means errors only,
so everything the bots write is discarded — a server that looks exactly like one
with no bots in it. The config in this repository sets `Logger.scripts.bots =
3,Console`; if you are deploying elsewhere, set it yourself.

**`.pbot` is not a known command.** The scripts compiled but were never
registered: TrinityCore ships `AddCustomScripts()` as an empty stub. See
[`custom_script_loader.cpp`](custom_script_loader.cpp).

**The server was killed while they were spawning.** Bots appear in staged
batches for a reason — creating them all at once exceeds the 60-second anti-freeze
watchdog. If you have modified the spawner, keep the staging.

**The machine stops answering while still replying to pings.** Out of memory, not
crashed. A population's cost is the maps and zones it keeps loaded: lower
`Pbot.WorldPopulationMaps`, or give the host more RAM — see the table in
[`../docker/INSTALL.md`](../docker/INSTALL.md).
