-- =====================================================================
-- hotfixes/01_map_difficulty_unlock.sql
--
-- >>> THIS FILE IS APPLIED TO THE `hotfixes` DATABASE, NOT `world`. <<<
--     mysql -u trinity -p hotfixes < 01_map_difficulty_unlock.sql
--
-- Fix: eight maps where NOTHING spawns — not one creature, not one
--      gameobject — even though the terrain, the creatures and the objects
--      are all present in the database.
--
-- SYMPTOM
--   At startup the worldserver logs, thousands of times:
--     "Table `creature` has creature (GUID: N) that is not spawned in any
--      difficulty, skipped."
--   (TrinityCore prints the same wording for gameobjects — see the note at
--   the bottom.) Every affected map is 100% empty in game.
--
-- ROOT CAUSE
--   ObjectMgr::LoadCreatures builds a per-map set of legal difficulties from
--   sMapDifficultyStore, then ParseSpawnDifficulties keeps only the tokens of
--   `spawnDifficulties` that are members of that set. If nothing survives, the
--   spawn is skipped entirely.
--
--   For these eight maps the intersection is empty: six have no MapDifficulty
--   record at all in the 12.0.7 client data (they are retired scenarios whose
--   records Blizzard removed), and two have a record for a difficulty that is
--   not in the spawn strings. The spawn rows are fine — the map has no legal
--   difficulty to spawn them into.
--
-- FIX
--   Add one MapDifficulty row per map with DifficultyID 0.
--
--   This works because DB2 stores are file-plus-database: DB2Store::LoadFromDB
--   runs the loader twice, once for rows with VerifiedBuild > 0 and once for
--   VerifiedBuild <= 0, and merges both into the store. A custom row with
--   VerifiedBuild 0 is therefore loaded by design, not by accident.
--
-- WHY DifficultyID MUST BE 0 AND NOTHING ELSE
--   Difficulty.db2 has no record with id 0. GetDefaultMapDifficulty() filters
--   candidates through sDifficultyStore.HasRecord(), so these rows are skipped
--   by difficulty selection entirely — they cannot change how an instance is
--   created or scaled. They exist only to satisfy ParseSpawnDifficulties,
--   where token 0 needs nothing more than set membership.
--
--   Do NOT "improve" this by using DifficultyID 1 for the dungeon below. A
--   real difficulty IS selectable, and it would override Blizzard's own
--   difficulty and ContentTuning for that map.
--
-- CLIENT SAFETY
--   Hotfixes are pushed to game clients from the separate `hotfix_data` table
--   (DB2Manager::LoadHotfixData). This file does not touch it, so nothing is
--   broadcast — the change is server-side only.
--
-- COST
--   DB2 index tables are sized from MAX(ID)+1, so an id near 1,000,000 costs
--   roughly 8 MB of pointers at load. That is the price of an id band that
--   will not collide with Blizzard data (real MapDifficulty ids are < 7,000).
--
-- SAFE TO RE-RUN (INSERT IGNORE on the primary key).
--
-- Requires a worldserver restart — DB2 stores load before the spawn tables.
--
-- Revert:
--   DELETE FROM `map_difficulty` WHERE `ID` BETWEEN 999981 AND 999988;
-- =====================================================================

INSERT IGNORE INTO `map_difficulty`
    (`Message`, `ID`, `DifficultyID`, `LockID`, `ResetInterval`, `MaxPlayers`,
     `ItemContext`, `ItemContextPickerID`, `Flags`, `ContentTuningID`,
     `WorldStateExpressionID`, `MapID`, `VerifiedBuild`)
VALUES
    ('', 999981, 0, 0, 0, 0, 0, 0, 0, 0, 0,  999, 0),  -- Theramore's Fall (scenario)
    ('', 999982, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1005, 0),  -- A Brewing Storm (scenario)
    ('', 999983, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1031, 0),  -- Arena of Annihilation (scenario)
    ('', 999984, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1048, 0),  -- Unga Ingoo (scenario)
    ('', 999985, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1051, 0),  -- Brewmoon Festival (scenario)
    ('', 999986, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1112, 0),  -- Pursuing the Black Harvest
    ('', 999987, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1744, 0),  -- Trial of Style
    ('', 999988, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2236, 0);  -- Darkmaul Citadel (Exile's Reach dungeon)

-- Verify (in the `hotfixes` database): should return 8 rows.
--   SELECT `ID`, `MapID`, `DifficultyID` FROM `map_difficulty`
--     WHERE `ID` BETWEEN 999981 AND 999988 ORDER BY `MapID`;
--
-- Then restart the worldserver and confirm in the `world` database that these
-- maps are no longer empty — every spawn on them lists difficulty 0, so all of
-- them become live:
--   SELECT `map`, COUNT(*) FROM `creature`
--     WHERE `map` IN (999,1005,1031,1048,1051,1112,1744,2236) GROUP BY `map`;
--   SELECT `map`, COUNT(*) FROM `gameobject`
--     WHERE `map` IN (999,1005,1031,1048,1051,1112,1744,2236) GROUP BY `map`;
--
-- NOTE when reading the log: ObjectMgr::LoadGameObjects prints the creature
-- wording verbatim ("Table `creature` has creature (GUID: ...)") for gameobject
-- spawns too. Counting those log lines therefore double-counts across two
-- tables — trust the database, not the log.
