# Connecting the Client — step by step (EN)

Point a retail **12.0.7.68275** client at your server. This page is deliberately
concrete: it separates **what you change on the server** from **what you change
in the client**.

> [Русский](../ru/CONNECT.md)

---

## TL;DR — what actually changes in the client

You do **not** edit game data. You only:

1. Make sure the client is **exactly build 68275**.
2. Drop the **Arctium launcher** next to `WoW.exe`.
3. Add **one line** to `WTF/Config.wtf`: `SET portal "YOUR_SERVER_IP"`.
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

Create an account (worldserver console):

```
account create myname mypassword
account set gmlevel myname 3 -1
```

---

## Client-side changes (the part you asked about)

### 1. Match the build — 68275

The client build **must** equal the server's world-DB build. Use **BNetInstaller**
to fetch/keep build **12.0.7.68275**, or a full retail install pinned to it. A
mismatched build will fail at login every time.

### 2. Add the Arctium launcher

Copy **`Arctium WoW Client Launcher.exe`** into the client folder, right next to
`WoW.exe`. Arctium patches the running client **in memory** so it:
- accepts a **custom portal**, and
- trusts the server's **self-signed login certificate**.

No game file is permanently modified.

### 3. Set the portal — one line in Config.wtf

Open `WTF/Config.wtf` in the client folder and add (or edit):

```
SET portal "YOUR_SERVER_IP"
```

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

1. Enter the **account name + password** you created with `account create`.
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
