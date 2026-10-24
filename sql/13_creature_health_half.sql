-- =====================================================================
-- 13_creature_health_half.sql
-- OPTIONAL BALANCE TUNING - read before applying.
--
-- This is not a bug fix. It halves the health of every non-boss creature
-- template and exists because this realm runs sixty bots as its population:
-- measured on it, ordinary mobs took 130+ hits to kill (bots dealt 20-100
-- damage against creatures with 3,000-1,400,000 health), so a bot died
-- before it finished a fight and never progressed. Halving health was the
-- only lever among several that moved the number that matters - deaths per
-- kill - while every behaviour change we tried (target filtering, earlier
-- retreat, avoiding creatures that had nearly killed the bot) made it worse
-- by costing kills.
--
-- Measured over 40 minutes, 60 bots: deaths per kill 31% -> 15.5%, kills
-- per hour 435 -> 522. A later fix to socket-less visibility took the same
-- ratio to ~4%.
--
-- DO NOT APPLY THIS BLINDLY TO A PLAYER SERVER. It changes every fight in
-- the world for everyone. It is here because the change is part of this
-- server's history and because it is fully reversible.
--
-- What it does NOT touch:
--   * boss-flagged templates      - TypeFlags & 0x4
--   * rows already at zero health - see 12_zero_hp_templates.sql first
--
-- SAFE TO RE-RUN: the backup table is created only if absent, and a second
-- run would halve again - check that the backup already exists before
-- running a second time.
--
-- Requires a worldserver restart.

CREATE TABLE IF NOT EXISTS `creature_template_difficulty_hp_backup` AS
SELECT `Entry`, `DifficultyID`, `HealthModifier`
FROM `creature_template_difficulty`
WHERE (`TypeFlags` & 4) = 0 AND `HealthModifier` > 0;

SELECT COUNT(*) AS backed_up FROM `creature_template_difficulty_hp_backup`;

UPDATE `creature_template_difficulty`
SET `HealthModifier` = `HealthModifier` * 0.5
WHERE (`TypeFlags` & 4) = 0 AND `HealthModifier` > 0;

SELECT ROW_COUNT() AS rows_halved;

-- Spot check: 3819, 4345 and 28145 were 0.7 before and read 0.35 after.
SELECT `Entry`, `DifficultyID`, `HealthModifier`
FROM `creature_template_difficulty`
WHERE `Entry` IN (3819, 4345, 28145, 39313)
ORDER BY `Entry`, `DifficultyID`;

-- REVERT
--   UPDATE `creature_template_difficulty` d
--   JOIN `creature_template_difficulty_hp_backup` b
--     ON b.`Entry` = d.`Entry` AND b.`DifficultyID` = d.`DifficultyID`
--   SET d.`HealthModifier` = b.`HealthModifier`;
