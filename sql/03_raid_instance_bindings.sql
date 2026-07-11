-- =====================================================================
-- 03_raid_instance_bindings.sql
-- Bind 5 legacy raids to their compiled instance script so the map gives
-- proper lockout, DungeonEncounter journal completion and boss-state
-- persistence (clearable/tracked).
--
-- NOTE: these instance scripts are stubs (SetBossNumber + encounter data
-- only) — they do NOT add boss combat AI. Full boss mechanics require
-- boss_*.cpp that does not exist for these raids in TrinityCore. This
-- binding is purely for the tracking/lockout value.
--
-- Requires a worldserver restart to take effect.
-- =====================================================================

UPDATE `instance_template` SET `script` = 'instance_dragon_soul'            WHERE `map` = 967; -- Dragon Soul
UPDATE `instance_template` SET `script` = 'instance_end_time'               WHERE `map` = 938; -- End Time
UPDATE `instance_template` SET `script` = 'instance_hour_of_twilight'       WHERE `map` = 940; -- Hour of Twilight
UPDATE `instance_template` SET `script` = 'instance_well_of_eternity'       WHERE `map` = 939; -- Well of Eternity
UPDATE `instance_template` SET `script` = 'instance_throne_of_the_four_winds' WHERE `map` = 754; -- Throne of the Four Winds

-- Revert: UPDATE `instance_template` SET `script` = '' WHERE `map` IN (967,938,940,939,754);
