-- =====================================================================
-- hotfixes/02_remove_empty_map_overrides.sql   (hotfixes DB)
-- Fix: instance maps whose collision geometry never loads, because an empty
--      override row tells the engine their parent map is Azeroth.
--
-- SYMPTOM
--   The server log fills with
--     Could not load VMAP name:City of Threads, id:2669, x:26, y:29 ...
--   for a handful of maps, over and over, for as long as it runs. On our realm
--   that was ~500 lines an hour across Priory of the Sacred Flame, City of
--   Threads, The Dawnbreaker, The Rookery and Vale of Eternal Blossoms. Inside
--   those tiles the engine has no line-of-sight, height or collision data at all.
--
-- ROOT CAUSE
--   A DB2 hotfix row REPLACES the whole record from the .db2 file — it does not
--   merge field by field. `hotfixes`.`map` contained 18 rows that are empty
--   apart from their ID: `Directory` NULL, `MapName` NULL, `WdtFileDataID` 0 and,
--   the damaging part, **`ParentMapID` = 0 and `CosmeticParentMapID` = 0**.
--
--   Zero is not "no parent" — that is -1. Zero is map 0, Azeroth. So for each of
--   those maps the engine believed Azeroth was the parent. A child map with no
--   geometry of its own for a tile borrows the parent's, and pairs it with its
--   own index file; borrowing from the wrong map gives two files that describe
--   different things, the spawn counts disagree, and the tile is rejected.
--
--   Measured, tile 26_29 of City of Threads: the map's own index file holds
--   2,494 entries, its real parent Khaz Algar (2601) holds 2,494 — an exact
--   match — while Azeroth holds 22. Every affected tile shows the same pattern.
--
--   These rows are also absent from `hotfix_data`, so they were never sent to
--   game clients: the damage was server-side only.
--
-- FIX
--   Delete them. With the override gone the engine reads the real record from
--   `Map.db2`, which carries the correct parent, and the tiles load.
--
--   Only rows that are empty in every one of those four fields are touched, so a
--   deliberate override cannot be caught by accident. `VerifiedBuild` is not used
--   as the test — a legitimate custom row may also carry 0.
--
-- SAFE TO RE-RUN. The backup table is created only if it does not already exist.
--
-- Requires a worldserver restart.
--
-- Revert:
--   INSERT INTO `map` SELECT * FROM `map_backup_empty_overrides`;
--   DROP TABLE `map_backup_empty_overrides`;
-- =====================================================================

CREATE TABLE IF NOT EXISTS `map_backup_empty_overrides` AS
SELECT * FROM `map`
WHERE `Directory` IS NULL
  AND `MapName` IS NULL
  AND `WdtFileDataID` = 0
  AND `ParentMapID` = 0
  AND `CosmeticParentMapID` = 0;

SELECT 'rows backed up' AS step, COUNT(*) AS n FROM `map_backup_empty_overrides`;

DELETE FROM `map`
WHERE `Directory` IS NULL
  AND `MapName` IS NULL
  AND `WdtFileDataID` = 0
  AND `ParentMapID` = 0
  AND `CosmeticParentMapID` = 0;

SELECT 'empty overrides removed' AS step, ROW_COUNT() AS n;

-- Verify: must return 0.
--   SELECT COUNT(*) FROM `map`
--     WHERE `Directory` IS NULL AND `MapName` IS NULL AND `WdtFileDataID` = 0
--       AND `ParentMapID` = 0 AND `CosmeticParentMapID` = 0;
--
-- And the surviving rows must still be the complete ones — this must return 0 too:
--   SELECT COUNT(*) FROM `map` WHERE `Directory` IS NULL OR `MapName` IS NULL;
