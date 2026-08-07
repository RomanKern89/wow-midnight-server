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
past 68275, this gives you a client newer than your database and login will fail
with a version mismatch. Use Option B when you need a specific build.

---

## Option B — a CDN installer tool (pin an exact build)

Community tools exist that download product files **directly from Blizzard's own
CDN** using Blizzard's public product manifests. They fetch the same bits the
Battle.net app would; the difference is that you choose the product and build
instead of always receiving the newest one. **BNetInstaller** is the one this
project's reference server was built with.

These tools are maintained by individuals and move between forks, so this guide
deliberately does not hard-code a repository link that would rot. Search GitHub
for `BNetInstaller` and prefer an actively maintained fork; check that the
product you need is supported before relying on it, since support for a new
expansion often lands in a fork or an open pull request before the main branch.

The interface is typically a product code plus a target directory:

```bash
BNetInstaller.exe --prod wow --dir "C:\Games\WoW_Midnight"
```

Common flags are `--prod` (product code — `wow` is retail, and the `Product`
field in `.build.info` of any existing install shows you the exact code),
`--dir` (install directory) and `--lang` (locale, e.g. `enUS`, `ruRU`). Confirm
the actual flags against the tool you downloaded — they differ between forks.

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
| Login screen build ≠ 68275 | Client auto-updated, or installed live | Re-pin with Option B; stop the Battle.net app managing that folder |
| Extractors find no data | Run from the wrong directory | Run them **inside** the client folder, next to `WoW.exe` |
| CASC download is enormous | The cache is kept alongside the data | The CASC cache can be deleted after extraction; it re-downloads on demand |
| "Version mismatch" at login | Client build ≠ world DB build | Align the two — see above |

Once the build matches and the data is extracted, continue to
**[SETUP.md](SETUP.md)** and then **[CONNECT.md](CONNECT.md)**.
