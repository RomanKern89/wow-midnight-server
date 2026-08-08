# Core patches

Patches against TrinityCore itself. Everything else in this repository is data
or documentation; these are the few places where the engine has to change.

Apply from the root of your TrinityCore checkout:

```bash
git apply /path/to/core-patches/0001-vmap-partial-parent-tile.patch
```

Each one is written to stay small so it keeps applying across core revisions.
The bots need one more patch of their own — see `bots/core-patch/` on the
`docker` branch.

---

## 0001-vmap-partial-parent-tile.patch

**Stops 35,398 spurious `Could not load VMAP` errors per server lifetime.**
Measured on our realm: 4,827 lines in one run before, 35 after — a 99.3%
reduction, and the remaining 35 are a different, genuine problem that the patch
deliberately leaves reported.

### The problem

A child map with no geometry of its own for a tile borrows the parent map's
`.vmtile` but keeps its own `.vmtileidx`. `StaticMapTree::LoadMapTile` requires
the spawn counts in the two files to be equal.

They are equal when the child inherited the parent's whole tile — 88,890 of
89,098 inherited tiles on our data. They are not when the child holds only a few
models that spill in from a neighbouring inherited ADT: `vmap4assembler` writes an
index file listing just those, while the parent's tile file lists all of its own.
The tile is then rejected, and because the check sits *before* the read loop,
nothing is loaded at all.

This is upstream behaviour, not damaged data. Re-running `vmap4assembler` on the
original `Buildings/dir_bin` inputs reproduces the same counts byte for byte, and
the mismatch is one-directional — indices are never *more* than spawns (0 cases
out of 208). Reported as **[TrinityCore#31964](https://github.com/TrinityCore/TrinityCore/issues/31964)**.

### What the patch changes

One condition. That single shape — a **borrowed** tile with **fewer** indices
than spawns — is reported as `FileNotFound`, which `TerrainInfo::LoadVMap`
handles silently, instead of `ReadFromFileFailed`, which it logs as an error.

Loading behaviour is unchanged: the tile did not load before and does not load
now. Only the false alarm goes away.

Deliberately narrow. A mismatch on a map's *own* tile, or *more* indices than
spawns, still reports as a failure — those would be real corruption.

### Why it does not just relax the check

Letting the loop run with fewer indices would be worse than the problem. The
index file is positional: the i-th index belongs to the i-th spawn in the tile
file. With a subset there is no way to tell which of the parent's spawns the
indices refer to, so the first N would be bound to the wrong models and produce
**wrong** collision. Absent collision is the safer failure.

### What you lose

Nothing that was working. Those 208 tiles never loaded. We converted all of them
to world coordinates and counted what is inside: **zero creature spawns and zero
gameobject spawns** — they are edge tiles that exist only because a neighbouring
model's bounding box spills across the boundary.

A proper fix has to make the two files agree, which means changing the file
format — an upstream decision, which is why this repository reports the defect
and silences the noise rather than pretending to repair it.
