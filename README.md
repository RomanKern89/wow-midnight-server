<!-- Language: **English** · [Русский](README.ru.md) -->

![WoW Midnight Server](assets/banner.png)

# WoW Midnight Server — TrinityCore 12.0.7 (build 68275)

> A documented, community-maintained **World of Warcraft: Midnight** private
> server built on **TrinityCore master**, at the current retail build
> **12.0.7.68275** — plus a set of original server-side fixes that make the
> newest content actually playable.

**🌐 Language:** **English** · [Русский](README.ru.md)

> ⚠️ **Read first:** this repo contains **documentation + our original fixes only**.
> It does **not** include the game client, game data, or a world database —
> those are Blizzard's property and cannot be redistributed. You build those
> yourself from your own retail client. See **[DISCLAIMER.md](DISCLAIMER.md)**.

---

## What makes this project different

Most public WoW private servers run **old expansions** (Wrath 3.3.5a, Cata,
MoP…). This one targets **current retail — patch 12.0.7 "Midnight"** and is
kept honest and playable:

| | This project | Typical private server |
|---|---|---|
| **Client build** | **12.0.7.68275 (Midnight, current retail)** | 3.3.5a / 4.3.4 / 5.4.8 |
| **Races** | All 26 incl. **allied races unlocked**, Dracthyr **Evoker intro** working | Classic races only |
| **Content span** | Vanilla → Midnight in one DB, **517 maps** populated | Single-expansion |
| **Companion bots** | Creature bots (`.bot`) + fake-player bots (`.pbot`) | Usually none / 3rd-party only |
| **Data policy** | Maximized to the **public-data ceiling**, gaps documented honestly | Often silently broken |
| **Fixes** | Curated, reversible, ID-band-safe **community fixes** (this repo) | Ad-hoc |

### Highlights
- **Newest content, actually reachable** — allied-race and class intros, modern
  zones (Khaz Algar, Isle of Dorn, Harandar) resurrect-able and quest-able.
- **A third of the world's patrols were frozen — now they aren't.** On a stock
  database **3,924 of 10,993** waypoint spawns never move: the spawn says
  "patrol" but no route exists, so the movement generator never initialises.
  [Fix 07](docs/en/FIXES.md) repairs every one of them.
- **Eight maps that spawned nothing now spawn again** — including **Darkmaul
  Citadel**, the Exile's Reach dungeon, which had never spawned a single NPC.
  1,218 creatures and 1,073 gameobjects recovered.
- **Curated fix pack** — 11 files: quest-chain repairs, quest-blocking GameObject
  spawns, legacy-raid lockout/journal bindings, newest-zone graveyards, holidays
  re-anchored to the real calendar, stray raid bosses removed from starter zones,
  and the NPC repairs above. All additive, all reversible, all on empty ID bands.
  Each one verified by breaking a real database, fixing it, re-running the fix,
  and then reverting to confirm the original numbers come back exactly.
- **Honest engineering** — where the engine hits a true ceiling (boss combat AI,
  brand-new phased-scene scripting), it's documented, not faked.

---

## Content at a glance (verified DB counts)

| Metric | Count |
|---|---:|
| Quests | **48,257** |
| Quest objectives | 61,101 |
| Quest starters / enders (NPC) | 27,694 / 34,524 |
| Creature templates / spawns | 227,684 / **733,485** |
| GameObject templates / spawns | 89,967 / 197,724 |
| Maps with spawns | **517** |
| Loot table rows | 3,084,867 |
| Vendor rows | 172,414 |
| Retail build | **12.0.7.68275** |

---

![Content statistics](assets/stats.png)

## Gallery

### Architecture

![Architecture](assets/architecture.png)

### In-game screenshots

> These are placeholders — drop your own real gameplay screenshots into
> `assets/screenshots/` and they'll show up here. (We ship **no** Blizzard game
> imagery; see [DISCLAIMER](DISCLAIMER.md).)

<!--
![Character creation](assets/screenshots/01-character.png)
![Isle of Dorn](assets/screenshots/02-isle-of-dorn.png)
![Companion bots](assets/screenshots/03-bots.png)
-->

_No screenshots yet — contributions welcome._

## Two ways to install

| | Path | Guide |
|---|------|-------|
| 🛠 **Manual** | Build & run on the host directly | this branch — [docs/en/SETUP.md](docs/en/SETUP.md) |
| 🐳 **Docker Compose** | Everything in containers, fixes auto-applied | [`docker` branch](https://github.com/RomanKern89/wow-midnight-server/tree/docker/docker) |

Both lead to the same server. Either way you supply your own client-extracted
data and world DB (Blizzard's property — see [DISCLAIMER](DISCLAIMER.md)).

## Quick start (manual)

0. **Get the client** — you download it from Blizzard yourself; it's a free
   download and the guide pins the exact build so it can't drift out of sync
   with your database. → **[docs/en/CLIENT.md](docs/en/CLIENT.md)**
1. **Build the core** — TrinityCore master. → [docs/en/SETUP.md](docs/en/SETUP.md)
2. **Extract game data** from your own retail client (maps/vmaps/mmaps/dbc).
3. **Import the world DB** and run `worldserver` + `bnetserver`.
4. **Apply the community fixes** in [`sql/`](sql/). → [sql/README.md](sql/README.md)
5. **Connect** your client. → [docs/en/CONNECT.md](docs/en/CONNECT.md)

Full walkthrough: **[docs/en/SETUP.md](docs/en/SETUP.md)** ·
Features & content: **[docs/en/FEATURES.md](docs/en/FEATURES.md)** ·
Fix details: **[docs/en/FIXES.md](docs/en/FIXES.md)**

### The short way (two scripts)

```powershell
# on YOUR PC - installs the client at the right build and points it at the server
.\scripts\setup-client.ps1 -InstallDir C:\Games\WoW -ServerIP 192.168.1.50
```
```bash
# on the SERVER - creates a login you can actually use, with GM rights
SOAP_USER='1#1' SOAP_PASS='...' scripts/create-account.sh -e you@example.com -p secret -g 3
```
The account name is an **email**: a retail client logs in through Battle.net, so
`account create` produces something you cannot log in with. See
[CONNECT.md](docs/en/CONNECT.md).

> **Where do I get the client?** From Blizzard, free, yourself — see
> [CLIENT.md](docs/en/CLIENT.md). Please don't ask for or share a client
> download; it's copyright infringement, third-party builds are unverifiable
> binaries, and the official download takes one command.

---

## Repository layout

```
.
├── README.md / README.ru.md      Project overview (EN / RU)
├── DISCLAIMER.md                 Legal — what we can and cannot ship
├── LICENSE                       MIT (original materials only)
├── docs/
│   ├── en/  CLIENT · SETUP · CONNECT · FEATURES · FIXES · ANNOUNCEMENT
│   └── ru/  CLIENT · SETUP · CONNECT · FEATURES · FIXES · ANNOUNCEMENT
├── sql/                          Community fix SQL (reversible, ID-band-safe)
│   └── hotfixes/                 Applies to the hotfixes DB, not world
└── scripts/
    ├── setup-client.ps1          Install the client at the right build + point it at your server
    ├── create-account.sh         Create a Battle.net account (+ GM level) on a running server
    ├── apply_fixes.sh            Apply the SQL fix pack
    └── vmssh.py                  SSH helper (credentials from env vars)
```

---

## Contributing

Issues and PRs welcome — especially additional **reversible, ID-band-safe**
data fixes for the newest content. Please never commit secrets or Blizzard game
assets (the `.gitignore` guards against both). See [DISCLAIMER.md](DISCLAIMER.md).

## Credits

Built on [TrinityCore](https://www.trinitycore.org/) (GPL-2.0). World of
Warcraft® © Blizzard Entertainment. Fan, non-commercial, educational project.
