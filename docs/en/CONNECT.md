# Connecting the Client (EN)

How to point a retail **12.0.7.68275** client at your server.

> [Русский](../ru/CONNECT.md)

## 1. Match the build

Your client build **must** match the server's world DB build (**68275**). Use a
tool such as the **Arctium** launcher / **BNetInstaller** to install and keep the
correct retail build. The client files themselves are Blizzard's — obtain them
through your own legitimate installation.

## 2. Patch the client for a custom server

Retail clients connect over Battle.net with certificate/portal checks. The
common private-server approach is the **Arctium Game Launcher**, which patches
the running client in memory so it accepts a custom **portal** and the server's
login certificate. No game files are modified permanently.

Typical flow:
1. Put the Arctium launcher next to your `WoW.exe` (retail build 68275).
2. Configure the **portal** to your server host (the address `bnetserver`
   advertises).
3. Launch through Arctium instead of the Blizzard launcher.

## 3. Login certificate (dev cert)

`bnetserver` presents a login certificate. For a private setup this is the
TrinityCore **dev certificate** served over the REST login endpoint (port 8081).
Make sure:
- `bnetserver.conf` `LoginREST.ExternalAddress` / `LoginREST.Port` are reachable
  from the client.
- The realm `address` in `auth.realmlist` is the IP the client can reach.

> Login for modern retail goes through the **HTTP REST dev-cert** path, not the
> old-style realmlist.wtf. If login "spins", it's almost always the REST
> endpoint address/port or a build mismatch — check those first.

## 4. First login

1. Launch via Arctium → Battle.net login screen.
2. Enter the account you made with `account create`.
3. Select the realm → you're in.

## 5. Troubleshooting

| Symptom | Likely cause |
|---|---|
| Stuck "Connecting" / "Authenticating" | REST login endpoint (8081) unreachable, or portal wrong |
| "Version mismatch" / can't pass login | Client build ≠ server build 68275 |
| Realm list empty | `auth.realmlist.address` wrong, or worldserver (8085) down |
| Character created but no quests | Data/quest-chain gaps — see [FIXES.md](FIXES.md) |

Companion bots: in-game `.bot` (creature bots) and `.pbot` (fake-player bots) —
see [FEATURES.md](FEATURES.md).
