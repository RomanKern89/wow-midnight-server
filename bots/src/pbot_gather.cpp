/*
 * Companion Bots — Phase 5 resource gathering implementation.
 * See pbot_gather.h for why the search is cooldown-gated and lock-checked.
 */

#include "pbot_gather.h"

#include "Cell.h"
#include "CellImpl.h"
#include "DB2Stores.h"      // sLockStore, sDB2Manager.GetSkillLinesForParentSkill
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Loot.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{
    // MovePoint id for "walking to a node", distinct from the wander id so the two behaviours can
    // be told apart in a movement-inform callback if one is ever added.
    constexpr uint32 GATHER_POINT_ID = 0xC01;

    // Stop this far short of the node instead of pathing to its exact position. A node's own
    // coordinates sit inside its model — ore veins are lumps of rock — so asking for a path INTO it
    // produced "MoveSplineInitArgs::Validate: _checkPathLengths() failed" and the bot simply never
    // moved. Observed live: one bot idled 20y from its node for three minutes.
    constexpr float APPROACH_OFFSET = 2.0f;

    // Walks the bot to a reachable spot beside the node.
    void MoveToNode(Player* bot, GameObject* node)
    {
        // Approach from whichever side the bot is already on, so it never crosses the node.
        float const angle = node->GetAbsoluteAngle(bot);
        float x = node->GetPositionX() + std::cos(angle) * APPROACH_OFFSET;
        float y = node->GetPositionY() + std::sin(angle) * APPROACH_OFFSET;
        float z = node->GetPositionZ();

        // Put the destination on the actual surface; a point hanging in the air or buried in the
        // terrain is the other way to get an invalid path.
        bot->UpdateAllowedPositionZ(x, y, z);

        bot->GetMotionMaster()->MovePoint(GATHER_POINT_ID, x, y, z);
    }

    // Classic-tier gathering skill ceiling. Modern retail splits gathering into per-expansion skill
    // lines; this grants the base line, which is what the classic-zone nodes our bots start among
    // actually require. Extending to the expansion lines is a data question, not a code one.
    constexpr uint16 GATHER_SKILL_MAX = 300;

    // Skill value per character level, capped at GATHER_SKILL_MAX — roughly the curve a player who
    // levelled the profession alongside their character would have.
    uint16 SkillValueForLevel(uint8 level)
    {
        return uint16(std::min<uint32>(uint32(level) * 5u, GATHER_SKILL_MAX));
    }

    // Does this bot's skill satisfy the lock on this gameobject?
    //
    // Mirrors the engine's own lock evaluation: walk the lock's cases, and for every case that
    // demands a skill, check the bot has enough of it. A node with no skill requirement at all
    // (a plain quest chest) is deliberately NOT treated as gatherable — bots should harvest
    // professions, not loot the world's quest containers.
    // Fills `why` with what the lock actually demands, for the diagnostic. Only ever called for a
    // node the bot could not open, and only for the first such node per search.
    void DescribeLock(GameObject const* go, std::string& why)
    {
        GameObjectTemplate const* info = go->GetGOInfo();
        uint32 const lockId = info ? info->GetLockId() : 0;
        LockEntry const* lock = lockId ? sLockStore.LookupEntry(lockId) : nullptr;

        why = Trinity::StringFormat("go {} lock {}", go->GetEntry(), lockId);
        if (!lock)
        {
            why += " (no lock entry)";
            return;
        }

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (!lock->Type[i])
                continue;

            why += Trinity::StringFormat(" [type {} index {} needs {} -> skill {}]",
                uint32(lock->Type[i]), uint32(lock->Index[i]), uint32(lock->Skill[i]),
                lock->Type[i] == LOCK_KEY_SKILL
                    ? uint32(SkillByLockType(LockType(lock->Index[i]))) : 0u);
        }
    }

    bool CanOpenLock(Player const* bot, GameObject const* go)
    {
        GameObjectTemplate const* info = go->GetGOInfo();
        if (!info)
            return false;

        uint32 const lockId = info->GetLockId();
        if (!lockId)
            return false;

        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return false;

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;

            SkillType const skill = SkillByLockType(LockType(lock->Index[i]));
            if (skill == SKILL_NONE)
                continue;

            if (bot->GetSkillValue(skill) >= lock->Skill[i])
                return true;   // one satisfied skill case is enough, same as the engine
        }

        // No skill case the bot can satisfy — either the lock wants a key/quest item, or it wants
        // more skill than the bot has. Either way this is not a node for us.
        return false;
    }

    class GatherableNodeCheck
    {
    public:
        GatherableNodeCheck(Player const* bot, float range, uint32& chestsSeen, std::string& firstRefusal)
            : _bot(bot), _range(range), _chestsSeen(chestsSeen), _firstRefusal(firstRefusal) { }

        bool operator()(GameObject* go) const
        {
            if (!go || !go->isSpawned())
                return false;

            // Modern herb and ore nodes are GAMEOBJECT_TYPE_GATHERING_NODE — a type retail added
            // and this filter did not know about. There are 25537 of them spawned in this world,
            // and the bots had never once been allowed to look at one.
            //
            // This, not the search radius and not the skill value, is why nothing was ever gathered:
            // the only things the old chest-only filter could see were footlockers and quest
            // containers, which it then correctly refused. Measured, 653 refusals in minutes and
            // every lock among them wanted "open", "treasure" or "open kneeling" — never mining or
            // herbalism. Classic-era chest nodes are kept because some still exist; the lock check
            // below is what tells a vein from a footlocker either way.
            GameobjectTypes const type = go->GetGoType();
            if (type != GAMEOBJECT_TYPE_GATHERING_NODE && type != GAMEOBJECT_TYPE_CHEST)
                return false;

            if (!_bot->IsWithinDist(go, _range))
                return false;

            // Counted BEFORE the lock check, so a fruitless search can say which of the two things
            // went wrong: no nodes anywhere near the bot, or nodes it is not allowed to open.
            // Widening the radius from 30y to 80y changed nothing (6 harvests became 2), so the
            // question is no longer "how far can it see" but "what is it looking at".
            ++_chestsSeen;

            if (CanOpenLock(_bot, go))
                return true;

            if (_firstRefusal.empty())
                DescribeLock(go, _firstRefusal);

            return false;
        }

    private:
        Player const* _bot;
        float _range;
        uint32& _chestsSeen;
        std::string& _firstRefusal;
    };
}

void PbotGather::GrantGatheringSkills(Player* bot)
{
    if (!bot)
        return;

    uint16 const value = SkillValueForLevel(bot->GetLevel());

    auto grant = [bot, value](uint32 skillId)
    {
        if (!skillId || bot->HasSkill(skillId))
            return;

        // step 1 = the first skill tier; the engine derives the rest from the skill line itself.
        bot->SetSkill(skillId, 1, value, GATHER_SKILL_MAX);
    };

    for (uint32 parentSkill : { uint32(SKILL_HERBALISM), uint32(SKILL_MINING) })
    {
        grant(parentSkill);

        // Modern retail splits gathering into a separate skill line per expansion ("Dragon Isles
        // Mining" and friends), and a node's lock demands the line for ITS expansion — the classic
        // parent skill alone opens almost nothing outside vanilla zones. Granting every child line
        // the client data declares keeps this correct for expansions that do not exist yet, the
        // same way the race table is derived rather than listed.
        if (std::vector<SkillLineEntry const*> const* children = sDB2Manager.GetSkillLinesForParentSkill(parentSkill))
            for (SkillLineEntry const* child : *children)
                if (child)
                    grant(child->ID);
    }
}

GameObject* PbotGather::FindNode(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return nullptr;

    GameObject* found = nullptr;
    uint32 chestsSeen = 0;
    std::string firstRefusal;
    GatherableNodeCheck check(bot, SEARCH_RANGE, chestsSeen, firstRefusal);
    Trinity::GameObjectLastSearcher<GatherableNodeCheck> searcher(bot, found, check);
    Cell::VisitAllObjects(bot, searcher, SEARCH_RANGE);

    // Only the informative case is worth a line: nodes were RIGHT THERE and the bot could not open
    // any of them. Silence means it simply had nothing in range, which is the other answer.
    //
    // Measured: 653 of these in minutes, from bots with herbalism and mining at 225 — so the wall
    // is the lock check, not the radius and not the skill value. The lock's own cases are printed
    // because guessing at client data is how the last three walls each cost a soak.
    if (!found && chestsSeen)
        TC_LOG_INFO("scripts.bots", "PbotGather: bot {} saw {} chests within {:.0f}y and could open "
            "none (herbalism {}, mining {}); first refusal: {}", bot->GetName(), chestsSeen,
            SEARCH_RANGE, uint32(bot->GetSkillValue(SKILL_HERBALISM)),
            uint32(bot->GetSkillValue(SKILL_MINING)), firstRefusal);

    return found;
}

bool PbotGather::Tick(Player* bot, ObjectGuid& nodeGuid, uint32& cooldownMs, uint32& harvestWaitMs, uint32 diff)
{
    if (!bot || !bot->IsAlive() || bot->IsInCombat())
    {
        nodeGuid.Clear();
        harvestWaitMs = 0;
        return false;
    }

    if (cooldownMs > diff)
        cooldownMs -= diff;
    else
        cooldownMs = 0;

    // Already heading for a node: keep going until we arrive, it vanishes, or someone else takes it.
    if (!nodeGuid.IsEmpty())
    {
        GameObject* node = ObjectAccessor::GetGameObject(*bot, nodeGuid);
        if (!node || !node->isSpawned())
        {
            nodeGuid.Clear();
            harvestWaitMs = 0;
            return false;
        }

        if (bot->IsWithinDist(node, INTERACT_RANGE))
        {
            // First tick at the node: start the harvest. Use() is the same entry point a
            // right-click reaches, so loot rules, lock checks and skill-up all run exactly as they
            // would for a player — including the opening delay, which is why we then wait rather
            // than expecting loot on this same tick.
            if (!harvestWaitMs)
            {
                node->Use(bot);
                harvestWaitMs = HARVEST_WAIT_MS;
                return true;
            }

            // Waiting for the loot to materialise. A real client would send one loot-item request
            // per slot once the loot window opened; with no client we take the items ourselves.
            if (Loot* loot = node->GetLootForPlayer(bot))
            {
                if (!loot->items.empty())
                {
                    // Iterate backwards: StoreLootItem removes the slot it took, which would shift
                    // every later index if we walked forwards.
                    for (uint32 slot = uint32(loot->items.size()); slot-- > 0; )
                        bot->StoreLootItem(node->GetGUID(), uint8(slot), loot);

                    TC_LOG_INFO("scripts.bots", "PbotGather: bot {} harvested node {}.",
                        bot->GetName(), node->GetEntry());

                    nodeGuid.Clear();
                    harvestWaitMs = 0;
                    cooldownMs = SEARCH_COOLDOWN_MS;
                    return true;
                }
            }

            // Still nothing. Give up once the wait is spent — the node may have been taken by
            // someone else, or the bot's skill was not enough after all.
            if (harvestWaitMs > diff)
            {
                harvestWaitMs -= diff;
            }
            else
            {
                harvestWaitMs = 0;
                nodeGuid.Clear();
                cooldownMs = SEARCH_COOLDOWN_MS;
            }
            return true;
        }

        // Still walking. Re-issue the move only if the motion generator has fallen idle, otherwise
        // we would restart the path every tick and never arrive.
        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
            MoveToNode(bot, node);

        return true;
    }

    if (cooldownMs)
        return false;

    GameObject* node = FindNode(bot);
    if (!node)
    {
        cooldownMs = SEARCH_COOLDOWN_MS;
        return false;
    }

    nodeGuid = node->GetGUID();
    MoveToNode(bot, node);
    return true;
}
