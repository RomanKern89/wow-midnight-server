-- =====================================================================
-- 12_zero_hp_templates.sql
-- Fix: creature difficulty rows that leave a spawned creature with no
--      health at all.
--
-- The engine computes a creature's health as
--     ceil(baseHealth * HealthModifier)
-- (Creature::GetMaxHealthByLevel, then UpdateLevelDependantStats). When the
-- modifier is 0 the base is 0, SetMaxHealth clamps it to 1, and the creature
-- spawns alive with a full bar of nothing: it dies to the first hit of any
-- kind and any "health percent" logic reads 0.
--
-- Two different groups carry a zero, and only one of them is fixable here:
--
--   A. The entry has a healthy row on another difficulty. 9 rows over 8
--      entries. The value is copied from that row - nothing is invented.
--      Example: 216736 Delandros Shimmermoon has 0.15 on difficulties
--      1/2/12/14 and 0 on 0; 202604 Quest Bunny 1 likewise.
--
--   B. The entry has no healthy row anywhere. 54 rows over 42 entries.
--      These are named in the report at the end rather than guessed at:
--      they are Scourge, Maw, totem and [DNT] objects whose intended health
--      cannot be derived from the data. Note that the modifier is not a
--      small multiplier in this database - values run to 5e7, so it encodes
--      absolute health per expansion, and any invented value would be
--      wrong by orders of magnitude.
--
-- The 86 rows whose modifier is small but non-zero (5e-14 .. 9.6e-7) are
-- deliberately NOT touched: they are 1-hit objects by design (Ihgaluk Roach,
-- Missile Target, Specimen Tank). Raising them would make quest objects stop
-- dying on contact and give scenery a health bar.
--
-- SAFE TO RE-RUN: the backup table is created only if absent, and the UPDATE
-- only touches rows that are still exactly 0.
--
-- Requires a worldserver restart.

CREATE TABLE IF NOT EXISTS `creature_template_difficulty_zero_hp_backup` AS
SELECT d.*
FROM `creature_template_difficulty` d
WHERE d.`HealthModifier` = 0;

-- How many rows were zero before the fix.
SELECT COUNT(*) AS zero_rows_before FROM `creature_template_difficulty_zero_hp_backup`;

-- A. Copy the value from the same entry's healthy difficulty.
UPDATE `creature_template_difficulty` d
JOIN (
    SELECT `Entry`, MIN(`HealthModifier`) AS hp
    FROM `creature_template_difficulty`
    WHERE `HealthModifier` >= 0.000001
    GROUP BY `Entry`
) s ON s.`Entry` = d.`Entry`
SET d.`HealthModifier` = s.hp
WHERE d.`HealthModifier` = 0;

-- What is left is group B - each entry needs a decision, not a formula.
SELECT d.`Entry`, ct.`name`, d.`DifficultyID`, d.`ContentTuningID`,
       (SELECT COUNT(*) FROM `creature` c WHERE c.`id` = d.`Entry`) AS spawns
FROM `creature_template_difficulty` d
LEFT JOIN `creature_template` ct ON ct.`entry` = d.`Entry`
WHERE d.`HealthModifier` = 0
ORDER BY spawns DESC, d.`Entry`;

-- REVERT
--   UPDATE `creature_template_difficulty` d
--   JOIN `creature_template_difficulty_zero_hp_backup` b
--     ON b.`Entry` = d.`Entry` AND b.`DifficultyID` = d.`DifficultyID`
--   SET d.`HealthModifier` = b.`HealthModifier`
--   WHERE d.`HealthModifier` <> b.`HealthModifier`;
