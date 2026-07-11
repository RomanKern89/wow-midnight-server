-- =====================================================================
-- 01_quest_earthen_intro_fix.sql
-- Fix: Earthen allied-race intro ("Awakening", Isle of Dorn) dead-locks
--      after the first quest because of a phantom chain link.
--
-- Chain:  79200 "Who am I?" -> 79201 "The Analysis Interface"
--                          -> 83328 "The Analysis Interface" (Foreman Uzjax)
--
-- Quest 79201 is a deprecated DUPLICATE (same title as 83328) that has NO
-- quest-starter and NO quest-ender anywhere in the DB, yet 83328 requires it
-- to be completed (PrevQuestID = 79201). Since nobody can give or turn in
-- 79201, the whole intro dead-locks and the player sees "no quest givers".
--
-- Fix: bypass the phantom so the chain flows 79200 -> 83328 directly.
-- Apply live with no restart:  worldserver console >  reload quest_template
-- =====================================================================

UPDATE `quest_template_addon` SET `NextQuestID` = 83328 WHERE `ID` = 79200;
UPDATE `quest_template_addon` SET `PrevQuestID` = 79200 WHERE `ID` = 83328;

-- Revert:
-- UPDATE `quest_template_addon` SET `NextQuestID` = 79201 WHERE `ID` = 79200;
-- UPDATE `quest_template_addon` SET `PrevQuestID` = 79201 WHERE `ID` = 83328;
