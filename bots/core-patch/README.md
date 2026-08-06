# The one core patch the bots need

`0001-player-initialize-empty-social.patch` — nine lines against
`src/server/game/Entities/Player/Player.{h,cpp}`.

## What it does

Adds `Player::InitializeEmptySocial()`, which hands a socket-less bot a valid,
empty `PlayerSocial`.

A bot is a real `Player` with a null-socket `WorldSession`, so it never runs the
login pipeline that would normally load its friends list from the database. The
pointer is therefore null, and the first piece of engine code that consults it —
anything social, and several things that are not obviously social — dereferences
nothing. The patch calls the same `SocialMgr::LoadFromDB` the login path calls,
with an empty result set, which that function is written to tolerate.

## Why it is a patch and not a script

`m_social` is private and there is no public setter. Everything else the bots do
is reachable through public APIs — this is the single exception, and it is kept
deliberately minimal so it applies cleanly across core revisions.

## How it is applied

The Docker build applies it automatically after fetching the source. By hand:

```bash
cd /path/to/TrinityCore
git apply /path/to/bots/core-patch/0001-player-initialize-empty-social.patch
```

If it ever stops applying, the fix is nine lines of hand-editing: add the method
declaration next to `GetSocial()` in `Player.h` and the body next to
`CleanupsBeforeDelete` in `Player.cpp`.

## How this was discovered, since the honesty is the point

The repository originally claimed the bots patch nothing. That claim was written
from memory of how they were designed, not from a build — and the live server's
own version string had been saying otherwise for months: `c9e3fd8df5cb+`, where
the trailing `+` means a modified working tree.

The Docker image failed to compile the moment it was actually built from a clean
checkout:

```
pbot_mgr.cpp:349: error: 'class Player' has no member named 'InitializeEmptySocial'
```

Nothing else in the image, the sources or the documentation would have revealed
it. Building the thing did.
