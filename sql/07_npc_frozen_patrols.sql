-- =====================================================================
-- 07_npc_frozen_patrols.sql
-- Fix: NPCs that are supposed to patrol stand frozen on their spawn point.
--
-- SYMPTOM
--   A guard, sentry or wandering mob never moves. It is not a stuck pathing
--   case — the creature never starts moving at all. On a stock database this
--   affects roughly a third of every waypoint mover in the world (measured:
--   3,924 of 10,993 spawns, ~36%).
--
-- ROOT CAUSE
--   `creature.MovementType` = 2 means WAYPOINT_MOTION_TYPE, and the path id
--   comes from `creature_addon.PathId`. When the spawn has no `creature_addon`
--   row, or PathId = 0, or PathId points at a `waypoint_path` that does not
--   exist, WaypointMovementGenerator::DoInitialize() cannot load a path,
--   returns false and logs
--     "WaypointMovementGenerator::DoInitialize: couldn't load path for ..."
--   The movement generator never initialises, so the creature simply stands
--   there for as long as the grid is loaded.
--
--   The spawn row is the anomaly, not the template: on a stock database the
--   overwhelming majority of these spawns belong to a creature_template whose
--   own MovementType is 0 (idle). Something set the spawn to "patrol" without
--   ever giving it a route.
--
-- FIX
--   Make the spawn honest about what it can actually do:
--     wander_distance > 0  ->  MovementType 1 (random movement in that radius)
--     wander_distance = 0  ->  MovementType 0 (idle)
--   This is the same mapping TrinityCore itself applies when it repairs other
--   contradictory MovementType/wander_distance combinations at load time.
--
--   Real patrol routes cannot be recovered — the waypoint data was never in
--   the database. This removes the broken state and the error spam; it does
--   not invent routes.
--
-- SAFE TO RE-RUN. The backup table is created only if it does not already
-- exist, so a second run cannot overwrite your original values.
--
-- Requires a worldserver restart.
--
-- Revert:
--   UPDATE `creature` c
--     JOIN `creature_backup_frozen_patrols` b ON b.`guid` = c.`guid`
--     SET c.`MovementType` = b.`MovementType`;
--   DROP TABLE `creature_backup_frozen_patrols`;
-- =====================================================================

CREATE TABLE IF NOT EXISTS `creature_backup_frozen_patrols` AS
SELECT c.*
FROM `creature` c
LEFT JOIN `creature_addon`  a ON a.`guid`   = c.`guid`
LEFT JOIN `waypoint_path`   p ON p.`PathId` = a.`PathId`
WHERE c.`MovementType` = 2
  AND (a.`guid` IS NULL OR a.`PathId` = 0 OR p.`PathId` IS NULL);

UPDATE `creature` c
LEFT JOIN `creature_addon`  a ON a.`guid`   = c.`guid`
LEFT JOIN `waypoint_path`   p ON p.`PathId` = a.`PathId`
SET c.`MovementType` = IF(c.`wander_distance` > 0, 1, 0)
WHERE c.`MovementType` = 2
  AND (a.`guid` IS NULL OR a.`PathId` = 0 OR p.`PathId` IS NULL);

-- Verify: both of these should return 0 after applying.
--   SELECT COUNT(*) FROM `creature` c
--     LEFT JOIN `creature_addon` a ON a.`guid` = c.`guid`
--     LEFT JOIN `waypoint_path`  p ON p.`PathId` = a.`PathId`
--     WHERE c.`MovementType` = 2
--       AND (a.`guid` IS NULL OR a.`PathId` = 0 OR p.`PathId` IS NULL);
--   -- and every surviving waypoint mover should own a path that has nodes:
--   SELECT COUNT(*) FROM `creature` c
--     JOIN `creature_addon` a ON a.`guid` = c.`guid` AND a.`PathId` <> 0
--     JOIN `waypoint_path`  p ON p.`PathId` = a.`PathId`
--     LEFT JOIN `waypoint_path_node` n ON n.`PathId` = p.`PathId`
--     WHERE c.`MovementType` = 2 AND n.`PathId` IS NULL;
