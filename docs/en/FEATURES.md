# Features & Content (EN)

> [Русский](../ru/FEATURES.md)

## Build & scope

- **TrinityCore master**, retail build **12.0.7.68275 ("Midnight")**.
- One world database spanning **Vanilla → Midnight**, with **517 maps**
  populated.

## Content counts (verified)

| Metric | Count |
|---|---:|
| Quests | **48,257** |
| Quest objectives | 61,101 |
| Quest starters / enders (NPC) | 27,694 / 34,524 |
| Creature templates / spawns | 227,684 / 733,928 |
| GameObject templates / spawns | 89,967 / 197,724 |
| Maps with spawns | 517 |
| Loot rows | 3,084,867 |
| Vendor rows | 172,414 |

## What works

- **All 26 races**, with **allied races unlocked** (config), including the
  **Dracthyr Evoker** starting experience (Forbidden Reach intro is fully
  scripted in TC and runs for the Evoker class).
- **Earthen** allied-race intro (Isle of Dorn "Awakening") — playable end to end
  after our chain fix (see [FIXES.md](FIXES.md)).
- **Leveling 1 → max** across all expansions via a saturated quest layer
  (48k quests, 27.7k NPC starters).
- **Dungeons/raids** — 104/119 instances fully functional (instance + boss
  scripts resolve); classic dungeons drive bosses via SmartAI.
- **Newest zones** (Khaz Algar, Isle of Dorn, **Harandar** map 2694) populated
  and **resurrect-able** (graveyards added for the newest zone).
- **Companion bots**:
  - `.bot` — creature-based companions (Phase 1/2).
  - `.pbot` — fake-player bots with persistence and combat rotations (Phase 3).

## Honest limitations (documented, not hidden)

These are engine/data ceilings, not silent breakage:

- **Legacy-raid boss combat AI** — 14 old raids have no `boss_*.cpp` in
  TrinityCore. We bind 5 of them to their instance script for **lockout +
  journal + boss-state tracking**, but the bosses stay on default AI. Full
  mechanics require C++ authoring (upstream ceiling).
- **Brand-new phased intro scenes** — the very newest scenes/conversations rely
  on scripting that TC does not yet implement; a few can strand a linear intro.
  Where we found one blocking a player (Earthen 79201), we fixed it.
- **Empty newest maps** (3047 / 3075) — no public spawn data exists yet.
- **Long-tail loot / vendors** — filled to the public-data ceiling; the residual
  gaps have no public source.

## Data completeness philosophy

The world DB is maximized to the **public-data ceiling** — the richest publicly
available master data set for this build. Where a gap exists because no public
data exists, it is **documented** rather than faked. The fixes in this repo are
the surgical, reversible closures we could make on top of that base.
