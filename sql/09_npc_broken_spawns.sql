-- =====================================================================
-- 09_npc_broken_spawns.sql
-- Fix: spawn rows that describe a creature which cannot exist, or exists
--      somewhere no player can reach.
--
-- Four separate defects, each with its own backup so you can revert them
-- independently.
--
--   A. Spawns whose `creature_template` does not exist.
--      The engine logs "Table `creature` has creature (GUID: N) with non
--      existing creature entry N, skipped." and drops them. They are dead
--      rows: nothing can bring them back except recreating the template,
--      and a template cannot be reconstructed from a spawn row.
--
--   B. Spawns at the exact map origin (0, 0, 0).
--      Never a real placement — the map origin is empty space or ocean.
--      Note that some of these entries also exist as vehicle passengers via
--      `vehicle_template_accessory` (mount vendors, for example). That
--      mechanism does not read the `creature` table, so removing the stray
--      row does not remove the passenger.
--
--   C. Spawns outside the map grid.
--      A WoW map is 64x64 cells of 533.33 yards, so every valid coordinate
--      is within +-17066.66. Anything beyond that is corrupt data.
--
--   D. Spawns that fell through the world (z <= -2000).
--      -2000 is used as a "no height data" sentinel, and values far below it
--      are corruption. This is well clear of legitimately deep places: the
--      deepest real content, Vashj'ir, sits around z = -700.
--      These are LIFTED rather than deleted where possible — the fix takes
--      the average z of healthy spawns within 60 yards and adds a metre.
--      The engine's GetMapHeight() searches about 50 yards and snaps the
--      creature to the ground, so an estimate that lands anywhere near the
--      real surface self-corrects on spawn. Only spawns with fewer than 3
--      healthy neighbours to derive a height from are deleted.
--
-- SAFE TO RE-RUN. Backups are created only if absent.
--
-- Requires a worldserver restart.
--
-- Revert:
--   INSERT INTO `creature` SELECT * FROM `creature_backup_broken_spawns`;
--   DELETE c FROM `creature` c JOIN `creature_backup_sunken` b ON b.`guid` = c.`guid`;
--   INSERT INTO `creature` SELECT * FROM `creature_backup_sunken`;
--   DROP TABLE `creature_backup_broken_spawns`, `creature_backup_sunken`;
-- =====================================================================

-- ---------- A, B, C: rows that have to go -----------------------------
CREATE TABLE IF NOT EXISTS `creature_backup_broken_spawns` AS
SELECT c.* FROM `creature` c
  LEFT JOIN `creature_template` t ON t.`entry` = c.`id`
  WHERE t.`entry` IS NULL
UNION
SELECT c.* FROM `creature` c
  WHERE c.`position_x` = 0 AND c.`position_y` = 0 AND c.`position_z` = 0
UNION
SELECT c.* FROM `creature` c
  WHERE ABS(c.`position_x`) > 17066 OR ABS(c.`position_y`) > 17066;

DELETE c FROM `creature` c
  LEFT JOIN `creature_template` t ON t.`entry` = c.`id`
  WHERE t.`entry` IS NULL;

DELETE FROM `creature`
  WHERE `position_x` = 0 AND `position_y` = 0 AND `position_z` = 0;

DELETE FROM `creature`
  WHERE ABS(`position_x`) > 17066 OR ABS(`position_y`) > 17066;

-- ---------- D: lift what can be lifted, drop the rest ------------------
CREATE TABLE IF NOT EXISTS `creature_backup_sunken` AS
SELECT c.* FROM `creature` c WHERE c.`position_z` <= -2000;

DROP TEMPORARY TABLE IF EXISTS `tmp_sunken_fix`;
CREATE TEMPORARY TABLE `tmp_sunken_fix` AS
SELECT
    c.`guid`,
    (SELECT AVG(n.`position_z`) + 1 FROM `creature` n
       WHERE n.`map` = c.`map` AND n.`position_z` > -500
         AND ABS(n.`position_x` - c.`position_x`) < 60
         AND ABS(n.`position_y` - c.`position_y`) < 60) AS `new_z`,
    (SELECT COUNT(*) FROM `creature` n
       WHERE n.`map` = c.`map` AND n.`position_z` > -500
         AND ABS(n.`position_x` - c.`position_x`) < 60
         AND ABS(n.`position_y` - c.`position_y`) < 60) AS `neighbours`
FROM `creature` c
WHERE c.`position_z` <= -2000;

UPDATE `creature` c
  JOIN `tmp_sunken_fix` f ON f.`guid` = c.`guid`
  SET c.`position_z` = f.`new_z`
  WHERE f.`neighbours` >= 3 AND f.`new_z` IS NOT NULL;

-- anything still down there had nothing nearby to derive a height from
DELETE FROM `creature` WHERE `position_z` <= -2000;

DROP TEMPORARY TABLE IF EXISTS `tmp_sunken_fix`;

-- Verify: all four should return 0 after applying.
--   SELECT COUNT(*) FROM `creature` c LEFT JOIN `creature_template` t
--     ON t.`entry` = c.`id` WHERE t.`entry` IS NULL;
--   SELECT COUNT(*) FROM `creature`
--     WHERE `position_x` = 0 AND `position_y` = 0 AND `position_z` = 0;
--   SELECT COUNT(*) FROM `creature`
--     WHERE ABS(`position_x`) > 17066 OR ABS(`position_y`) > 17066;
--   SELECT COUNT(*) FROM `creature` WHERE `position_z` <= -2000;
