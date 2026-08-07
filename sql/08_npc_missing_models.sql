-- =====================================================================
-- 08_npc_missing_models.sql
-- Fix: creatures that are spawned in the world but can never appear in it.
--
-- SYMPTOM
--   The worldserver log repeats
--     "Creature (Entry: N) has no model defined in table `creature_template`,
--      can't load."
--   every time one of those grids loads. The creature does not exist in the
--   world at all — it cannot be seen, targeted, or killed.
--
-- ROOT CAUSE
--   A creature needs at least one row in `creature_template_model`. Some
--   entries have none. The engine refuses to spawn them.
--
-- WHY IT MATTERS
--   Most of the affected entries are invisible utility NPCs — kill-credit
--   counters, quest-objective bunnies, "[DNT]" spawner stubs. They are meant
--   to be invisible, but they still have to EXIST: a quest objective that
--   counts kills of a credit NPC can never complete if that NPC cannot spawn.
--   On a stock database this was 889 distinct entries across 1,908 spawns.
--
-- FIX
--   Give every spawned entry that has no model at all the canonical invisible
--   display id 11686 — the model TrinityCore's own invisible stalkers use, and
--   already referenced by thousands of trigger templates in a stock database.
--   The creature then loads and behaves normally while staying invisible.
--
--   Scope is deliberately limited to entries that are actually SPAWNED. An
--   entry nobody placed in the world does not need a model, and touching it
--   would only widen the blast radius.
--
-- IF ONE OF THESE IS ACTUALLY A VISIBLE NPC
--   It will now be present but invisible, which is still strictly better than
--   absent. Give it its real display id to finish the job:
--     UPDATE `creature_template_model` SET `CreatureDisplayID` = <real id>
--       WHERE `CreatureID` = <entry> AND `CreatureDisplayID` = 11686;
--   On the database this was written against, exactly one affected entry was
--   referenced as a quest giver or ender, and it was a kill-credit counter.
--
-- SAFE TO RE-RUN.
--
-- Requires a worldserver restart.
--
-- Revert:
--   DELETE m FROM `creature_template_model` m
--     JOIN `creature_template_model_backup_added` b ON b.`CreatureID` = m.`CreatureID`
--     WHERE m.`Idx` = 0 AND m.`CreatureDisplayID` = 11686 AND m.`VerifiedBuild` = 0;
--   DROP TABLE `creature_template_model_backup_added`;
-- =====================================================================

-- Record exactly which entries this file adds a model for, so the revert
-- above can never touch a model that was already there.
CREATE TABLE IF NOT EXISTS `creature_template_model_backup_added` AS
SELECT DISTINCT c.`id` AS `CreatureID`
FROM `creature` c
LEFT JOIN `creature_template_model` m ON m.`CreatureID` = c.`id`
WHERE m.`CreatureID` IS NULL;

INSERT IGNORE INTO `creature_template_model`
    (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT x.`CreatureID`, 0, 11686, 1, 1, 0
FROM (
    SELECT DISTINCT c.`id` AS `CreatureID`
    FROM `creature` c
    LEFT JOIN `creature_template_model` m ON m.`CreatureID` = c.`id`
    WHERE m.`CreatureID` IS NULL
) x;

-- Verify: should return 0 after applying.
--   SELECT COUNT(DISTINCT c.`id`) FROM `creature` c
--     LEFT JOIN `creature_template_model` m ON m.`CreatureID` = c.`id`
--     WHERE m.`CreatureID` IS NULL;
