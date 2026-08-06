-- =====================================================================
-- 06 — Remove stray creature spawns (NPCs standing where they do not belong)
--
-- A default world database contains a handful of creatures spawned in the
-- wrong place entirely: raid bosses in starter cities, event props with no
-- event attached, and Blizzard's own QA test creatures. They are an artifact
-- of sniffed data — a client saw them somewhere during a scripted moment and
-- the capture recorded them as permanent residents.
--
-- They matter beyond being odd scenery. A level-90 boss standing at the
-- Stormwind gates is sudden death for anything levelling past it, and the
-- companion bots in this repo were dying to exactly that.
--
-- WHAT IS REMOVED (30 rows), and what stood around each one — the company an
-- NPC keeps identifies its true home more reliably than any external site,
-- and these legacy entries 404 on Wowhead because they are not in live retail
-- data at all:
--
--   Prince Sarsarun (45214)        Uldum's Herald of Al'Akir, at the Stormwind gates
--   Kai'ju Gahz'rilla (40961)      a raid boss, same spot
--   Necropolis Acolyte (16368) x6  Scourge Invasion props, NOT event-gated,
--   Skeletal Soldier (16422) x3      and spawned nowhere else in the world
--   Clayton's Test Creature (25738) "Quality Assured", flagged as a BOSS
--   Armageddon Target (25735)      beside it, in a developer test cluster near
--                                    Stormwind complete with an NPC named Rygarius
--   Darren's Test NPC 2 (55243)    a test NPC, in Stormheim
--   Grobbulus (29373)              a Naxxramas boss, among 102 "High Overlord's
--                                    Raider" in a Battle for Azeroth war camp
--   Sapphiron (29991)              a Naxxramas boss, among Fjord Hawks in Howling Fjord
--   Lord Marrowgar (37958) x6      an Icecrown boss, among Shattered Sun Marksmen
--                                    on the Isle of Quel'Danas
--   Rotface (38549) x6             an Icecrown boss, on the Magisters' Terrace
--   King Haldor (30782) x2         a Northrend NPC, in Pandaria
--
-- WHAT IS DELIBERATELY NOT TOUCHED:
--   * classification 6 — invisible script helpers ("bunnies", kill-credit
--     proxies, checkpoints). There are 45567 of them and the world needs
--     every one; they are not visible to players.
--   * "Ribbon Pole Debug Target" and friends — real Midsummer content that
--     merely has a technical name.
--   * The 22 remaining boss-classification spawns, all of which belong:
--     Dragonflight rares, Valdrakken training dummies, script helpers.
--
-- HOW TO FIND MORE, if your database differs: boss-classification spawns are
-- few enough to read by name (about 40 world-wide), and creatures spawned
-- both inside an instance and out in the open are almost always wrong.
--
-- REQUIRES A WORLDSERVER RESTART — spawns are loaded at startup.
--
-- REVERT: INSERT INTO creature SELECT * FROM creature_strays_backup_06;
-- =====================================================================

-- Back up the exact rows first, so this is reversible.
CREATE TABLE IF NOT EXISTS creature_strays_backup_06 AS
SELECT * FROM creature
WHERE id IN (45214, 40961, 25738, 25735, 55243, 29373, 29991, 37958, 38549, 30782)
   OR (map = 0 AND id IN (16368, 16422)
       AND SQRT(POW(position_x + 8954, 2) + POW(position_y - 521, 2)) < 150);

-- Addons are keyed by guid and would be left orphaned.
DELETE FROM creature_addon WHERE guid IN (SELECT guid FROM creature_strays_backup_06);
DELETE FROM creature       WHERE guid IN (SELECT guid FROM creature_strays_backup_06);

-- --- verify --------------------------------------------------------------
-- SELECT COUNT(*) FROM creature WHERE guid IN (SELECT guid FROM creature_strays_backup_06);
--   -> 0
--
-- SELECT ct.Name, c.map, ROUND(c.position_x), ROUND(c.position_y)
-- FROM creature c JOIN creature_template ct ON ct.entry = c.id
-- WHERE ct.Classification = 3 ORDER BY c.map;
--   -> only creatures that belong where they stand
