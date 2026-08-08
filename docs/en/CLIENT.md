# Getting the Game Client (EN)

> [Русский](../ru/CLIENT.md)

This page answers the question that stops most people before they ever reach
[SETUP.md](SETUP.md): **where does the client come from?**

Short answer: **from Blizzard, by you.** The World of Warcraft client is a free
download — a subscription is only required to play on Blizzard's own realms.
Nothing in this project ships, mirrors or redistributes it, and you do not need
anyone else's copy.

> **Do not download the client from a torrent tracker or a file-sharing site.**
> Redistributing Blizzard's client is copyright infringement, third-party builds
> are unverifiable binaries you would be running on your own machine, and it is
> completely unnecessary — the official download is free and takes one command.
> See [DISCLAIMER.md](../../DISCLAIMER.md).

---

## What you need

| | |
|---|---|
| **A Battle.net account** | Free to create. No subscription needed to download. |
| **Disk space** | ~100 GB for a full modern retail install. |
| **The right build** | This project targets **12.0.7.68275**. The build must match your world database. |

---

## Option A — the Battle.net app (simplest)

1. Install the Battle.net desktop app from Blizzard.
2. Log in and install **World of Warcraft**.
3. Note the install path, e.g. `C:\Program Files (x86)\World of Warcraft\_retail_`.

**Caveat:** the app always installs the *current live* build. If retail has moved
past the build your server expects, this gives you a client newer than your
database and login fails with a version mismatch.

---

## Read this before you download 100 GB

**Nothing can install an old build for you.** Not the Battle.net app, and not
Battle.Net-Installer below — it drives Blizzard's Agent, which only ever fetches
whatever is live right now. There is no argument for a build id or a buildconfig
hash, and Blizzard does not serve arbitrary past builds.

Check what is live *before* downloading:

```powershell
(Invoke-WebRequest 'http://us.patch.battle.net:1119/wow/versions' -UseBasicParsing).Content
```

The `VersionsName` column on the `us` row is the build you will get.
`scripts/setup-client.ps1` runs this check for you and refuses to start a
download that would produce the wrong build.

**If the live build is newer than your server's**, there are two real options:

1. **Move the server to the live build.** Update TrinityCore, re-extract the game
   data from the new client, and set the realm's gamebuild accordingly. This is
   the sustainable answer — retail moves every few weeks, and a server pinned to
   a build nobody can download any more is a dead end for new players.
2. **Keep an existing install of the old build** and stop the Battle.net app from
   updating it. Point it at your server with
   `scripts/setup-client.ps1 -SkipInstall`.

> This repository documents build **12.0.7.68275**, which was the live build on
> 2026-07-05. Retail has moved on since. Someone downloading today gets a newer
> client, so expect to take option 1.

---

## Option B — Battle.Net-Installer (choose product, locale and directory)

[**Battle.Net-Installer**](https://github.com/barncastle/Battle.Net-Installer)
drives Blizzard's own Battle.net Agent to install, update or repair a product
into a directory you choose. It is not a downloader that works around Blizzard —
it uses Blizzard's Agent and CDN, which is why it needs the Battle.net app
installed and recently logged in.

Requirements: **Windows**, **.NET 8.0**, Battle.net installed and authenticated.
Only products marked *Active* in the TACT database will install.

```
BNetInstaller.exe --prod wow --lang enUS --dir "C:\Games\WoW_Midnight" --uid wow --verbose false
```

| Flag | Meaning |
|---|---|
| `--prod` | TACT product code. `wow` is retail — the `Product` field in `.build.info` of any existing install confirms it. |
| `--lang` | Asset language, e.g. `enUS`, `ruRU`. Availability varies per product. |
| `--dir` | Install directory. |
| `--uid` | Agent UID, when it differs from the product code. |
| `--repair` | Repair instead of install/update. |

### Three things that will waste your evening

1. **Close the Battle.net app first.** It fights the Agent for the same install.
2. **Pass `--verbose false`.** Verbose progress reporting reads the cursor
   position and crashes when there is no interactive desktop session.
3. **Agent error 2310 — as of 2026-08-08 this tool does not work at all.**
   Verified on a machine where it worked in July: the upstream v2.1 release
   fails with `Agent Error: 2310`, and so does a fresh build of the
   [fork](https://github.com/xCortlandx/Battle.Net-Installer) whose pull request
   #27 ("Nice try Blizzard") fixed 2310 back in February 2026. Both fail
   identically, with the Agent running and authenticated, with and without
   `--uid`, in any locale, into an empty directory.

   Blizzard evidently tightened Agent-side validation again. The February fix
   set `MonitorPid` to a real process id and added proper JSON headers; that is
   no longer enough. Until someone updates the tool, **use Option A — the
   Battle.net app installs WoW into a directory you choose and is not affected.**

   (An earlier version of this page blamed a missing `phoenix-agent/1.0`
   User-Agent. That was wrong — upstream has always sent that header.)

> If you only need the client to *extract server data* rather than to play, you
> can skip this entirely: the TrinityCore extractors read Blizzard's CASC storage
> online. See [Extracting without a full install](#extracting-without-a-full-install).

---

## What a correct install looks like

Verified against a working 12.0.7.68275 install (~131 GB):

```
WoW_Midnight\
├── .build.info                     <- product + exact version live here
├── .product.db
├── Data\                           <- CASC storage (config, data, indices, …)
├── _retail_\
│   ├── Wow.exe                     <- the game binary
│   └── WTF\Config.wtf              <- the one file you edit
├── Arctium Game Launcher.exe       <- at the ROOT, not inside _retail_
└── World of Warcraft Launcher.exe
```

Two details that trip people up: **Arctium sits at the install root**, beside
`_retail_` — not next to `Wow.exe`. And the file you edit is
`_retail_\WTF\Config.wtf`, not a `Config.wtf` at the root.

## Verify the build

Everything downstream depends on this. The fastest and most reliable check is
`.build.info` in the install root — it is a pipe-delimited text file, and the
`Version` field holds the exact build:

```bash
# Windows (PowerShell)
Get-Content .build.info | Select-String "12\.0\.7"
```

You are looking for **`12.0.7.68275`**. The same file's `Product` field should
read `wow` (retail) — that is the product code to pass to `--prod`.

You can also read the build on the login screen (bottom corner), or from
`Wow.exe` → *Properties* → *Details*.

If the build does not match your world database, login fails every time with a
version mismatch. There is no workaround other than aligning the two.

### Stop it from auto-updating

If you installed through the Battle.net app, it will happily update the client
out from under you and break the match. Either keep a separate copy of the client
folder that the app does not manage, or install with Option B into a directory
the app knows nothing about.

---

## What happens to this client next

1. **Extract the game data** — `mapextractor`, `vmap4extractor` +
   `vmap4assembler`, and `mmaps_generator` run inside your client folder and
   produce `dbc/`, `maps/`, `vmaps/`, `mmaps/`, `cameras/`. Those go to your
   server's data directory. This is covered in **[SETUP.md](SETUP.md)**.
2. **Point the client at your server** — Arctium launcher plus one line in
   `Config.wtf`. This is covered in **[CONNECT.md](CONNECT.md)**.

The extracted data is Blizzard's. It stays on your machine and your server. It is
never shared, and it is not in this repository.

---

## Extracting without a full install

The TrinityCore extractors can read Blizzard's **CASC** storage online, so a
server can be built from a data-only fetch rather than a complete game install.
This is how the reference server for this project was built. It is still
Blizzard's data, fetched from Blizzard, under the same terms — the convenience is
that you do not need the full client sitting on the server host.

See [SETUP.md](SETUP.md) for the extractor flags.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Login screen build ≠ the server's | Client auto-updated, or was installed at the live build | It cannot be downgraded — move the server to the live build, or restore an install of the old one and stop the Battle.net app managing that folder |
| Extractors find no data | Run from the wrong directory | Run them **inside** the client folder, next to `WoW.exe` |
| CASC download is enormous | The cache is kept alongside the data | The CASC cache can be deleted after extraction; it re-downloads on demand |
| "Version mismatch" at login | Client build ≠ world DB build | Align the two — see above |

Once the build matches and the data is extracted, continue to
**[SETUP.md](SETUP.md)** and then **[CONNECT.md](CONNECT.md)**.
