# Connecting the Client — step by step (EN)

Point a retail **12.0.7.68275** client at your server. This page is deliberately
concrete: it separates **what you change on the server** from **what you change
in the client**.

> [Русский](../ru/CONNECT.md)

---

## TL;DR — what actually changes in the client

You do **not** edit game data. You only:

1. Make sure the client is **exactly build 68275** — see **[CLIENT.md](CLIENT.md)**.
2. Drop **`Arctium Game Launcher.exe`** in the install root (beside `_retail_`).
3. Add **one line** to `_retail_\WTF\Config.wtf`: `SET portal "YOUR_SERVER_IP"`.
4. Launch through **Arctium** (not the Blizzard app).

That's it. Everything else is server-side.

---

## Server-side prerequisites (do these first)

In `bnetserver.conf`:

```ini
; The address the CLIENT will reach (public IP or LAN IP of the server)
LoginREST.ExternalAddress = YOUR_SERVER_IP
LoginREST.LocalAddress    = YOUR_SERVER_IP
LoginREST.Port            = 8081
```

In the `auth` database, the realm the client sees comes from the `realmlist`
table (not a `realmlist.wtf`):

```sql
UPDATE realmlist
SET address = 'YOUR_SERVER_IP', localAddress = 'YOUR_SERVER_IP', port = 8085
WHERE id = 1;
```

Make sure these ports are open to the client: **8081** (REST login), **1119**
(bnet), **8085** (world).

Create an account (worldserver console). **It must be a Battle.net account, and
the name must be an email address** — a retail client logs in through Battle.net:

```
bnetaccount create you@example.com yourpassword
```

That prints the linked game account it created for you, named `<id>#1`. Grant GM
rights on *that* name, not on the email:

```
bnetaccount listgameaccounts you@example.com
account set gmlevel 1#1 3 -1
```

> **Do not use `account create` here.** It makes a plain game account with no
> Battle.net link, which the client cannot log in with. TrinityCore is explicit
> about it: `account create` refuses any name containing `@` and tells you to use
> the bnet commands, while `bnetaccount create` requires one.

Or let a script do all of it, including the GM level:

```bash
SOAP_USER='1#1' SOAP_PASS='...' scripts/create-account.sh -e you@example.com -p yourpassword -g 3
```

---

## Client-side changes (the part you asked about)

### 1. Match the build — 68275

The client build **must** equal the server's world-DB build. A mismatched build
fails at login every time. **[CLIENT.md](CLIENT.md)** covers obtaining the client
and what to do when the live build no longer matches your server — or just run
`scripts/setup-client.ps1`, which checks the build before downloading and sets
the portal.

### 2. Add the Arctium launcher

Copy **`Arctium Game Launcher.exe`** into the **install root** — beside
`_retail_`, `Data` and `.build.info`, **not** inside `_retail_` next to
`Wow.exe`. This is the single most common setup mistake.

```
WoW_Midnight\
├── Data\
├── _retail_\Wow.exe
└── Arctium Game Launcher.exe      <- here
```

Arctium patches the running client **in memory** so it:
- accepts a **custom portal**, and
- trusts the server's **self-signed login certificate**.

No game file is permanently modified.

### 3. Set the portal — one line in Config.wtf

Open **`_retail_\WTF\Config.wtf`** (note the `_retail_` prefix — there is no
`Config.wtf` at the install root) and add, or edit if it is already present:

```
SET portal "YOUR_SERVER_IP"
```

The file is a flat list of `SET key "value"` lines in no particular order. Add
yours on its own line; leave the graphics settings alone.

Optional, if you want to force locale / skip the launcher update check:

```
SET textLocale "enUS"
SET audioLocale "enUS"
```

> `portal` is the **only** required client edit. It tells the client which
> Battle.net host to talk to — your `bnetserver`.

### 4. Launch through Arctium

Run **Arctium**, not `Battle.net.exe`. It starts the patched client and shows the
Battle.net login screen pointed at your server.

### 5. Log in

1. Enter the **email address + password** of the Battle.net account you created
   with `bnetaccount create`.
2. Pick the realm.
3. You're in.

---

## Why login "spins" — and how to fix it fast

Modern retail login goes through the **HTTP REST dev-certificate** path (port
8081), not the old `realmlist.wtf`. 95% of stuck logins are one of these:

| Symptom | Cause | Fix |
|---|---|---|
| Stuck "Connecting" / "Authenticating" | REST endpoint 8081 unreachable, or `portal` wrong | Verify `LoginREST.ExternalAddress` + port 8081 open; check `SET portal` |
| "Version mismatch" / bounced at login | Client build ≠ 68275 | Reinstall/repin the client to 68275 |
| Realm list empty | `realmlist.address` wrong or worldserver (8085) down | Fix the `realmlist` row; confirm 8085 is listening |
| Certificate/handshake error | Arctium not used, or cert not trusted | Always launch via Arctium |
| In game, but a character has no quests | Data/quest-chain gap | See [FIXES.md](FIXES.md) |

---

## In-game companion bots

- `.bot` — creature-based companions.
- `.pbot` — fake-player bots with persistence + combat rotations.

See [FEATURES.md](FEATURES.md).
