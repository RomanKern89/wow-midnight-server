# Announcement — ready to post (EN)

> [Русский](../ru/ANNOUNCEMENT.md)
>
> Copy-paste this into a forum thread, a subreddit, or a Discord announcement.
> Everything in it is measured, not estimated — keep it that way if you edit it.
> Suitable places: TrinityCore community forums, r/wowservers, private-server
> Discords. **This announces the server project and its fixes. It does not
> distribute the game client — everyone brings their own.**

---

## Short version (for a Discord / chat post)

> **WoW Midnight private server toolkit — TrinityCore on current retail 12.0.7.68974**
>
> Not another 3.3.5a server. Documentation plus 11 original, reversible SQL fixes
> for a modern retail world database, and a companion-bot system that puts 60
> autonomous fake players into the world — they quest, gather, craft, use the
> auction house and fight in battlegrounds.
>
> The biggest fix unfreezes about **a third of every patrolling NPC in the world**
> (3,924 of 10,993 waypoint spawns never move on a stock database). Another makes
> **eight completely empty maps spawn again**, including Darkmaul Citadel — the
> Exile's Reach dungeon, which had never spawned a single NPC.
>
> MIT, no client files, no database dumps, bring your own retail client.
> https://github.com/RomanKern89/wow-midnight-server

---

## Full version (for a forum thread)

### WoW Midnight server toolkit — TrinityCore master, retail 12.0.7.68974

Most public private servers run old expansions. This project targets **current
retail** and tries to be honest about what actually works.

It is **documentation and original fixes only**. There is no game client, no
game data and no world-database dump in the repository, and there never will be —
you build those from your own client with the official TrinityCore tools.

### What's in it

**A curated fix pack — 11 SQL files, all additive, all reversible.**
Every one of them was verified by rolling a copy of a real database back to its
broken state, applying the fix, confirming the defect count reaches zero,
applying it a second time to prove it is safe to re-run, and finally running the
documented revert to check the original numbers come back exactly.

The ones worth your attention:

- **A third of the world's patrols never move.** A creature set to
  `MovementType = 2` takes its route from `creature_addon.PathId`. When there is
  no addon row, PathId is 0, or PathId names a `waypoint_path` that does not
  exist, the movement generator fails to initialise and the creature stands on
  its spawn point forever. That is **3,924 of 10,993** waypoint spawns. The spawn
  row is the anomaly, not the template — 3,773 of them belong to a
  `creature_template` whose own MovementType is idle.
- **Eight maps where nothing spawns at all.** `ObjectMgr` intersects each spawn's
  `spawnDifficulties` with the map's legal difficulties; when the intersection is
  empty the spawn is skipped. For these maps it is always empty. One MapDifficulty
  row per map recovers **1,218 creature and 1,073 gameobject spawns**, including
  **Darkmaul Citadel**, the Exile's Reach dungeon.
- **889 creatures that cannot appear at all** because they have no model, which
  the engine refuses to load. Mostly kill-credit and quest-objective NPCs — they
  are meant to be invisible, but a quest objective counting kills of a credit NPC
  can never complete while that NPC cannot spawn.
- Quest-chain dead-locks, quest-blocking GameObjects, graveyards for the newest
  zone, legacy-raid lockout bindings, holidays re-anchored to the real calendar,
  and stray raid bosses removed from starter zones.

**A companion-bot system.** Creature-based bots (`.bot`) and fake-player bots
(`.pbot`) that persist across restarts and actually live in the world: they take
and turn in quests, gather, learn and practise professions, buy and sell on the
auction house, repair, migrate zones when they outlevel them, and fight in
battlegrounds. Measured behaviour and honest limitations are both documented.

**Docker Compose deployment**, verified end to end on a clean host.

### What it does not pretend to do

Where the engine hits a real ceiling it is written down rather than faked. Boss
combat AI for the newest raids does not exist. Some quest data is simply not in
any public database. Bot quest turn-ins are still unreliable. All of that is in
the docs, with measurements.

### Getting started

- **[Getting the client](https://github.com/RomanKern89/wow-midnight-server/blob/main/docs/en/CLIENT.md)** — you download it from Blizzard
  yourself; it is a free download. Note that you always get the build that is
  live right now — nothing can install an older one, so check first.
- **[Server setup](https://github.com/RomanKern89/wow-midnight-server/blob/main/docs/en/SETUP.md)** — build TrinityCore, extract your data.
- **[Connecting](https://github.com/RomanKern89/wow-midnight-server/blob/main/docs/en/CONNECT.md)** — Arctium plus one line in `Config.wtf`.
- **[The fixes](https://github.com/RomanKern89/wow-midnight-server/blob/main/docs/en/FIXES.md)** — what each one repairs and why.

Bilingual (EN/RU) throughout. MIT licensed. Issues and pull requests welcome.

**https://github.com/RomanKern89/wow-midnight-server**

---

### A note on the client

Please do not ask for a client download link, and please do not post one. The WoW
client is free to download from Blizzard directly — a subscription is only needed
for Blizzard's own realms. Redistributing it is copyright infringement, and
third-party builds are unverifiable binaries you would be running on your own
machine. [CLIENT.md](https://github.com/RomanKern89/wow-midnight-server/blob/main/docs/en/CLIENT.md) covers getting it properly, including how to pin
a specific build so it does not auto-update out from under your server.
