# Server Setup (EN)

How to build and run a TrinityCore **master** server at retail build
**12.0.7.68275 (Midnight)**. You need your own legally-owned retail WoW client.

> [Русский](../ru/SETUP.md)

## 1. Requirements

**Host (Linux recommended, e.g. Debian/Ubuntu):**
- 8+ CPU cores, **24 GB+ RAM** (retail DB + maps are heavy), 200 GB+ disk
- MySQL 8.0 / MariaDB 10.6+
- `git`, `cmake`, `clang`/`gcc`, `boost` (1.78+), `openssl`, `zlib`, `readline`

Modern retail is resource-hungry. Give the box swap and an OOM guard
(`earlyoom`) — a heavy build or load spike can otherwise hang the VM.

## 2. Build the core

```bash
git clone https://github.com/TrinityCore/TrinityCore.git
cd TrinityCore && mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/tc -DTOOLS=1 -DWITH_WARNINGS=0
make -j$(nproc) && make install
```

> ⚠️ Do **not** run a heavy `make -j` while a live worldserver is running on the
> same box — the 60s anti-freeze watchdog will force-crash it under CPU
> starvation. Build first, run second (or build elsewhere).

## 3. Extract game data (from YOUR client)

Using the `mapextractor`, `vmap4extractor` + `vmap4assembler`, and
`mmaps_generator` tools built above, run them **inside your retail client
folder** to produce `dbc/`, `maps/`, `vmaps/`, `mmaps/`, `cameras/`. Copy those
into `/opt/tc/data`.

For current retail you extract from the CASC storage (online or installed).
This data is Blizzard's — it stays on your machine and is never shared.

## 4. Databases

Create the four schemas and a user:

```sql
CREATE DATABASE auth       DEFAULT CHARSET utf8mb4;
CREATE DATABASE characters DEFAULT CHARSET utf8mb4;
CREATE DATABASE world      DEFAULT CHARSET utf8mb4;
CREATE DATABASE hotfixes   DEFAULT CHARSET utf8mb4;
CREATE USER 'trinity'@'localhost' IDENTIFIED BY 'trinity';
GRANT ALL ON *.* TO 'trinity'@'localhost';
```

Import the TrinityCore SQL base for `auth`, `characters`, `hotfixes`, and a
**world database that matches build 68275**. The world DB is built from your own
data — this repo does not ship one (see [DISCLAIMER](../../DISCLAIMER.md)).

## 5. Configure

Copy `worldserver.conf.dist` → `worldserver.conf` and `bnetserver.conf.dist` →
`bnetserver.conf` in `/opt/tc/etc`. Set the DB connection info to match your
`trinity` user. Set `DataDir` to `/opt/tc/data`. In `auth.realmlist` point the
realm's `address` at your server IP.

## 6. Run

```bash
# bnetserver (battle.net auth) — e.g. as a systemd unit
/opt/tc/bin/bnetserver -c /opt/tc/etc/bnetserver.conf

# worldserver — first boot builds caches, ~7 min
cd /opt/tc/bin && ./worldserver -c /opt/tc/etc/worldserver.conf
```

Run `worldserver` in `tmux`/`screen` so you keep the console (you'll need it for
`reload`, `.account`, GM commands). A systemd unit that launches it after MySQL
is recommended for reboots.

Open ports: **8085** (world), **1119** (bnet), **8081** (rest/login).

## 7. Apply the community fixes

```bash
cd scripts
DB_USER=trinity DB_PASS=trinity DB_NAME=world ./apply_fixes.sh
```

Then in the worldserver console: `reload quest_template` (quest fixes are live,
no restart). GameObject / graveyard / raid-binding changes need a restart. See
[FIXES.md](FIXES.md).

## 8. Create an account

In the worldserver console:

```
account create <name> <password>
account set gmlevel <name> 3 -1
```

Continue to **[CONNECT.md](CONNECT.md)** to patch and point your client.
