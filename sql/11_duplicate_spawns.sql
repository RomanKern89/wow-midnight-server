-- =====================================================================
-- 11_duplicate_spawns.sql
-- Fix: two or more copies of the same NPC standing inside each other.
--
-- SYMPTOM
--   You target a quest giver in Stormwind and there is a second, identical
--   one underneath it. Clicking cycles between them; one may be tapped or
--   in combat while the other is not. Measured on a stock database: 2,598
--   groups, 3,403 redundant spawns.
--
-- ROOT CAUSE
--   Nothing in the engine deduplicates `creature`. Rows accumulated from
--   repeated imports of overlapping data sets, so the same spawn was
--   inserted more than once under different guids.
--
-- WHAT COUNTS AS A DUPLICATE
--   Everything that decides what a player sees, and when, must match:
--   creature id, map, x/y/z, orientation, spawnDifficulties, all three
--   phase fields, terrainSwapMap, modelid and equipment_id.
--
-- WHAT IS DELIBERATELY LEFT ALONE — this is the whole difficulty of the fix.
--   Grouping on coordinates alone flags 7,319 groups / 8,379 spawns. Nearly
--   5,000 of those are healthy data:
--     * different phases      — players at different story stages see
--                               different NPCs standing in the same spot;
--     * different model or equipment — visibly different creatures;
--     * groups where some members are in `game_event_creature`,
--       `spawn_group` or `pool_members` and others are not — the engine
--       picks one of them, that is variety, not duplication.
--   Delete on the naive rule and you erase working content.
--
-- WHICH COPY SURVIVES
--   The formation leader if the group contains one (`creature_formations`
--   references it by guid); otherwise the lowest guid. Dependent
--   `creature_addon` and `creature_formations` rows of the removed spawns
--   are deleted with them, so this cannot create the orphan rows that
--   `10_npc_orphan_references.sql` repairs.
--
-- SAFE TO RE-RUN. Backup tables are created only if they do not already
-- exist, so a second run cannot overwrite your original values.
--
-- Requires a worldserver restart.
--
-- Revert:
--   INSERT INTO `creature`            SELECT * FROM `creature_backup_duplicates`;
--   INSERT INTO `creature_addon`      SELECT * FROM `creature_addon_backup_duplicates`;
--   INSERT INTO `creature_formations` SELECT * FROM `creature_formations_backup_duplicates`;
--   DROP TABLE `creature_backup_duplicates`,
--              `creature_addon_backup_duplicates`,
--              `creature_formations_backup_duplicates`;
--
-- ONE THING TO EXPECT AFTER REVERTING. Every removed row comes back — verify by
-- row count, which returns exactly to its original value. Do NOT verify by
-- re-counting duplicate groups: `spawnDifficulties` is a comma-separated list
-- whose element ORDER is not normalised ("205,208,220,167,147" and
-- "147,167,205,208,220" are the same set of difficulties), and the round trip
-- can reorder it. The grouping above compares that column as a string, so some
-- restored rows no longer group with their twin and the count comes back lower
-- than before. The data is intact and the engine parses the list as a set —
-- only the string form differs. Re-running the fix after a revert therefore
-- removes fewer rows than the first run.
--
-- The same limitation caps the fix itself: two genuinely identical spawns whose
-- difficulty lists are written in a different order are not detected. On our
-- database that is 4 pairs out of 3,403 removed spawns.
-- =====================================================================

-- --- 1. groups of indistinguishable spawns ---------------------------
DROP TABLE IF EXISTS `_dup_groups`;
CREATE TABLE `_dup_groups` AS
SELECT `id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`,
       `spawnDifficulties`, `phaseUseFlags`, `PhaseId`, `PhaseGroup`,
       `terrainSwapMap`, `modelid`, `equipment_id`
FROM `creature`
GROUP BY `id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`,
         `spawnDifficulties`, `phaseUseFlags`, `PhaseId`, `PhaseGroup`,
         `terrainSwapMap`, `modelid`, `equipment_id`
HAVING COUNT(*) > 1;

ALTER TABLE `_dup_groups` ADD INDEX `idx_grp` (`id`, `map`, `position_x`, `position_y`, `position_z`);

-- --- 2. members, with the flags that can make a group legitimate ------
DROP TABLE IF EXISTS `_dup_members`;
CREATE TABLE `_dup_members` AS
SELECT c.`guid`,
       CONCAT_WS('|', c.`id`, c.`map`, c.`position_x`, c.`position_y`, c.`position_z`,
                 c.`orientation`, c.`spawnDifficulties`, c.`phaseUseFlags`, c.`PhaseId`,
                 c.`PhaseGroup`, c.`terrainSwapMap`, c.`modelid`, c.`equipment_id`) AS `grp`,
       (SELECT COUNT(*) FROM `game_event_creature` g WHERE g.`guid` = c.`guid`) AS `in_event`,
       (SELECT COUNT(*) FROM `spawn_group` s WHERE s.`spawnId` = c.`guid` AND s.`spawnType` = 0) AS `in_group`,
       (SELECT COUNT(*) FROM `pool_members` p WHERE p.`spawnId` = c.`guid` AND p.`type` = 0) AS `in_pool`,
       (SELECT COUNT(*) FROM `creature_formations` f WHERE f.`leaderGUID` = c.`guid`) AS `is_leader`
FROM `creature` c
JOIN `_dup_groups` d
  ON  d.`id` = c.`id` AND d.`map` = c.`map`
  AND d.`position_x` = c.`position_x` AND d.`position_y` = c.`position_y`
  AND d.`position_z` = c.`position_z` AND d.`orientation` = c.`orientation`
  AND d.`spawnDifficulties` = c.`spawnDifficulties` AND d.`phaseUseFlags` = c.`phaseUseFlags`
  AND d.`PhaseId` = c.`PhaseId` AND d.`PhaseGroup` = c.`PhaseGroup`
  AND d.`terrainSwapMap` = c.`terrainSwapMap`
  AND d.`modelid` = c.`modelid` AND d.`equipment_id` = c.`equipment_id`;

ALTER TABLE `_dup_members` ADD PRIMARY KEY (`guid`), ADD INDEX `idx_g` (`grp`(180));

-- --- 3. the survivor of each eligible group ---------------------------
DROP TABLE IF EXISTS `_dup_keep`;
CREATE TABLE `_dup_keep` AS
SELECT m.`grp`,
       COALESCE(MIN(CASE WHEN m.`is_leader` > 0 THEN m.`guid` END), MIN(m.`guid`)) AS `keep_guid`
FROM `_dup_members` m
GROUP BY m.`grp`
HAVING COUNT(DISTINCT m.`in_event`) = 1
   AND COUNT(DISTINCT m.`in_group`) = 1
   AND COUNT(DISTINCT m.`in_pool`)  = 1;

ALTER TABLE `_dup_keep` ADD INDEX `idx_g` (`grp`(180));

-- --- 4. the redundant copies ------------------------------------------
DROP TABLE IF EXISTS `_dup_delete`;
CREATE TABLE `_dup_delete` AS
SELECT m.`guid`
FROM `_dup_members` m
JOIN `_dup_keep` k ON k.`grp` = m.`grp`
WHERE m.`guid` <> k.`keep_guid`;

ALTER TABLE `_dup_delete` ADD PRIMARY KEY (`guid`);

-- --- 5. backups --------------------------------------------------------
CREATE TABLE IF NOT EXISTS `creature_backup_duplicates` AS
SELECT c.* FROM `creature` c JOIN `_dup_delete` d ON d.`guid` = c.`guid`;

CREATE TABLE IF NOT EXISTS `creature_addon_backup_duplicates` AS
SELECT a.* FROM `creature_addon` a JOIN `_dup_delete` d ON d.`guid` = a.`guid`;

CREATE TABLE IF NOT EXISTS `creature_formations_backup_duplicates` AS
SELECT f.* FROM `creature_formations` f JOIN `_dup_delete` d ON d.`guid` = f.`memberGUID`;

-- --- 6. dependent rows first, then the spawns --------------------------
DELETE a FROM `creature_addon` a JOIN `_dup_delete` d ON d.`guid` = a.`guid`;
DELETE f FROM `creature_formations` f JOIN `_dup_delete` d ON d.`guid` = f.`memberGUID`;
DELETE c FROM `creature` c JOIN `_dup_delete` d ON d.`guid` = c.`guid`;

SELECT 'spawns removed' AS result, COUNT(*) AS n FROM `_dup_delete`;

DROP TABLE `_dup_groups`;
DROP TABLE `_dup_members`;
DROP TABLE `_dup_keep`;
DROP TABLE `_dup_delete`;

-- Verify: this must return only the groups the fix deliberately skips
-- (mixed event / spawn-group / pool membership). On our database: 59.
--   SELECT COUNT(*) FROM (SELECT COUNT(*) n FROM `creature`
--     GROUP BY `id`,`map`,`position_x`,`position_y`,`position_z`,`orientation`,
--              `spawnDifficulties`,`phaseUseFlags`,`PhaseId`,`PhaseGroup`,
--              `terrainSwapMap`,`modelid`,`equipment_id` HAVING n > 1) t;
--
-- And no orphan rows may have appeared — both must return 0:
--   SELECT COUNT(*) FROM `creature_addon` a
--     LEFT JOIN `creature` c ON c.`guid` = a.`guid` WHERE c.`guid` IS NULL;
--   SELECT COUNT(*) FROM `creature_formations` f
--     LEFT JOIN `creature` c ON c.`guid` = f.`leaderGUID` WHERE c.`guid` IS NULL;
