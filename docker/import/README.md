# World DB dump goes here

Drop **one** `.sql` file — a TrinityCore **world** database dump that matches
build **12.0.7.68275** — into this folder. On first `docker compose up`, the
`db-init` service imports it into the `world` schema and then applies our
community fixes on top.

We ship **no** world dump: it is derived from Blizzard's client and cannot be
redistributed (see [../../DISCLAIMER.md](../../DISCLAIMER.md)). You build/obtain
your own via the standard TrinityCore process.

`*.sql` here is git-ignored so you never accidentally commit game data.
