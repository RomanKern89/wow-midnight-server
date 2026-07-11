# Client-extracted data goes here

Mount your **own** client-extracted game data into this folder. The worldserver
container reads it read-only from `/opt/tc/data`. Expected subfolders:

```
data/
├── dbc/
├── maps/
├── vmaps/
├── mmaps/
└── cameras/
```

Produce these with the TrinityCore extractor tools run against **your own**
retail WoW client (build 68275). This is Blizzard's data — it stays on your
machine and is git-ignored here (never committed).
