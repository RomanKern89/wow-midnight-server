-- =====================================================================
-- 10_npc_orphan_references.sql
-- Fix: rows that point at something which is not there.
--
-- None of these crash the server. They produce startup error spam, and each
-- one silently disables a piece of NPC behaviour.
--
--   A. `creature_addon` rows whose `PathId` names a `waypoint_path` that does
--      not exist. On a spawn with MovementType 2 this freezes the creature
--      (see 07); on any other spawn the value is a dangling reference that
--      SmartAI can still try to follow. Cleared to 0.
--
--   B. `creature_addon` rows for a spawn that no longer exists. Pure orphans.
--      Note this file is ordered AFTER 09 for a reason: 09 deletes broken
--      spawns, which turns their addon rows into orphans that this file then
--      collects.
--
--   C. `creature_formations` rows whose leader or member spawn is missing.
--      A formation with a missing leader never forms up, so the members that
--      should march behind it stand on their own spawn points instead.
--
-- Run this AFTER 07 and 09.
--
-- SAFE TO RE-RUN. Backups are created only if absent.
--
-- Requires a worldserver restart.
--
-- Revert:
--   UPDATE `creature_addon` a
--     JOIN `creature_addon_backup_dangling_path` b ON b.`guid` = a.`guid`
--     SET a.`PathId` = b.`PathId`;
--   INSERT INTO `creature_addon` SELECT * FROM `creature_addon_backup_orphans`;
--   INSERT INTO `creature_formations` SELECT * FROM `creature_formations_backup_orphans`;
--   DROP TABLE `creature_addon_backup_dangling_path`,
--              `creature_addon_backup_orphans`,
--              `creature_formations_backup_orphans`;
-- =====================================================================

-- ---------- A: dangling waypoint references ---------------------------
CREATE TABLE IF NOT EXISTS `creature_addon_backup_dangling_path` AS
SELECT a.* FROM `creature_addon` a
LEFT JOIN `waypoint_path` p ON p.`PathId` = a.`PathId`
WHERE a.`PathId` <> 0 AND p.`PathId` IS NULL;

UPDATE `creature_addon` a
LEFT JOIN `waypoint_path` p ON p.`PathId` = a.`PathId`
SET a.`PathId` = 0
WHERE a.`PathId` <> 0 AND p.`PathId` IS NULL;

-- ---------- B: addon rows with no spawn --------------------------------
CREATE TABLE IF NOT EXISTS `creature_addon_backup_orphans` AS
SELECT a.* FROM `creature_addon` a
LEFT JOIN `creature` c ON c.`guid` = a.`guid`
WHERE c.`guid` IS NULL;

DELETE a FROM `creature_addon` a
LEFT JOIN `creature` c ON c.`guid` = a.`guid`
WHERE c.`guid` IS NULL;

-- ---------- C: formations pointing at missing spawns -------------------
CREATE TABLE IF NOT EXISTS `creature_formations_backup_orphans` AS
SELECT f.* FROM `creature_formations` f
  LEFT JOIN `creature` c ON c.`guid` = f.`leaderGUID`
  WHERE c.`guid` IS NULL
UNION
SELECT f.* FROM `creature_formations` f
  LEFT JOIN `creature` c ON c.`guid` = f.`memberGUID`
  WHERE c.`guid` IS NULL;

DELETE f FROM `creature_formations` f
  LEFT JOIN `creature` c ON c.`guid` = f.`leaderGUID`
  WHERE c.`guid` IS NULL;

DELETE f FROM `creature_formations` f
  LEFT JOIN `creature` c ON c.`guid` = f.`memberGUID`
  WHERE c.`guid` IS NULL;

-- Verify: all three should return 0 after applying.
--   SELECT COUNT(*) FROM `creature_addon` a LEFT JOIN `waypoint_path` p
--     ON p.`PathId` = a.`PathId` WHERE a.`PathId` <> 0 AND p.`PathId` IS NULL;
--   SELECT COUNT(*) FROM `creature_addon` a LEFT JOIN `creature` c
--     ON c.`guid` = a.`guid` WHERE c.`guid` IS NULL;
--   SELECT COUNT(*) FROM `creature_formations` f LEFT JOIN `creature` c
--     ON c.`guid` = f.`leaderGUID` WHERE c.`guid` IS NULL;
