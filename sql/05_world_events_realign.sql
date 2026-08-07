-- =====================================================================
-- 05 — Realign holiday events with the real-world calendar
--
-- TrinityCore stores a holiday as an anchor date plus a fixed `occurence`
-- in MINUTES. Two consequences follow, and both were visible on a live
-- server: anything the real calendar defines by RULE (Darkmoon Faire is
-- the first Sunday of the month; Noblegarden follows Easter; the Lunar
-- Festival follows the lunar new year) drifts away from its rule, and
-- even fixed-date holidays slide about a day every four years because
-- 525600 minutes is 365 days flat, with no leap quarter.
--
-- A default TDB anchors these in 2022-2023. By August 2026 the yearly
-- holidays were 2-4 days early and the Darkmoon Faire was two weeks out.
--
-- This file re-anchors each holiday to its NEXT genuine occurrence, so
-- every later year lands on the right date too.
--
-- REQUIRES A WORLDSERVER RESTART — `game_event` is read at startup.
--
-- REVERT: restore the table you backed up in the first statement:
--   INSERT INTO game_event SELECT * FROM game_event_backup_YYYYMMDD;
-- =====================================================================

-- Back up first. Rename the target if you run this more than once.
CREATE TABLE IF NOT EXISTS game_event_backup_05 AS SELECT * FROM game_event;

-- --- fixed-date holidays -------------------------------------------------
UPDATE game_event SET start_time = '2027-06-21 02:00:00' WHERE eventEntry = 1;   -- Midsummer Fire Festival, Jun 21 - Jul 5
UPDATE game_event SET start_time = '2026-12-16 06:00:00' WHERE eventEntry = 2;   -- Winter Veil, Dec 16 - Jan 2
UPDATE game_event SET start_time = '2026-12-31 06:00:00' WHERE eventEntry = 6;   -- New Year's Eve
UPDATE game_event SET start_time = '2027-02-05 00:01:00' WHERE eventEntry = 8;   -- Love is in the Air, Feb 5 - 19
UPDATE game_event SET start_time = '2027-05-01 00:01:00' WHERE eventEntry = 10;  -- Children's Week, May 1 - 7
UPDATE game_event SET start_time = '2026-09-06 22:01:00' WHERE eventEntry = 11;  -- Harvest Festival
UPDATE game_event SET start_time = '2026-10-18 00:01:00' WHERE eventEntry = 12;  -- Hallow's End, Oct 18 - Nov 1
UPDATE game_event SET start_time = '2026-09-20 22:01:00' WHERE eventEntry = 24;  -- Brewfest, Sep 20 - Oct 6
UPDATE game_event SET start_time = '2026-11-22 00:01:00' WHERE eventEntry = 26;  -- Pilgrim's Bounty

-- --- holidays that move with another calendar ----------------------------
-- These have no fixed date at all, so the anchor is the correct date for
-- the coming year and will need setting again after it.
UPDATE game_event SET start_time = '2027-02-06 00:01:00' WHERE eventEntry = 7;   -- Lunar Festival — lunar new year 2027
UPDATE game_event SET start_time = '2027-03-29 00:01:00' WHERE eventEntry = 9;   -- Noblegarden — the week after Easter 2027

-- --- Darkmoon Faire ------------------------------------------------------
-- Its real rule is "first Sunday of the month, seven days", which this
-- schema cannot express: `occurence` is a fixed number of minutes, and 30
-- days is not a month. Anchoring on the next first-Sunday keeps it within
-- a couple of days for roughly half a year; after that, re-anchor.
UPDATE game_event SET start_time = '2026-09-06 00:01:00' WHERE eventEntry = 3;   -- the Faire itself
UPDATE game_event SET start_time = '2026-09-01 00:01:00' WHERE eventEntry = 23;  -- construction, a few days before

-- --- verify --------------------------------------------------------------
-- What the ENGINE considers active is NOT "now is between start and end".
-- It is: minutes since the anchor, modulo the recurrence, below the length.
-- Checking it the naive way reports every holiday as running at once.
--
-- SELECT eventEntry, description
-- FROM game_event
-- WHERE occurence > 0 AND length > 0
--   AND start_time <= NOW() AND (end_time IS NULL OR end_time >= NOW())
--   AND MOD(TIMESTAMPDIFF(MINUTE, start_time, NOW()), occurence) < length;
--
-- On an ordinary day that should list only the short recurring events —
-- the arena run, the night cycle, a boss rotation, brew of the month —
-- and no seasonal holiday.
