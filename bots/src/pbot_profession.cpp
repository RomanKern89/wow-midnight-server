/*
 * Companion Bots — crafting profession implementation. See pbot_profession.h.
 */

#include "pbot_profession.h"

#include "pbot_belongings.h" // do not craft more of what already fills the bags
#include "pbot_bags.h"   // a tailor short of room should sew itself a bag first

#include "pbot_mount.h"   // a bot rides to the anvil; it cannot craft until it gets off

#include "Cell.h"
#include "CellImpl.h"
#include "DB2Stores.h"
#include "DB2Structure.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "GameTime.h"
#include "Log.h"

#include <unordered_map>
#include "MotionMaster.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellDefines.h"    // CastSpellExtraArgs, TRIGGERED_IGNORE_CAST_TIME
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace
{
    // What a refusal actually means, in words. The raw enum number is unreadable in a log, and the
    // one that mattered - 123 - was not a fault at all: it means the recipe needs a forge or cooking
    // fire, and the bot simply had not reached one yet.
    char const* RefusalName(SpellCastResult result)
    {
        switch (result)
        {
            case SPELL_FAILED_REQUIRES_SPELL_FOCUS: return "needs a workbench (spell focus)";
            case SPELL_FAILED_REAGENTS:             return "missing reagents";
            case SPELL_FAILED_NOT_READY:            return "not ready yet";
            case SPELL_FAILED_SPELL_IN_PROGRESS:    return "another cast in progress";
            case SPELL_FAILED_ITEM_NOT_READY:       return "item not ready";
            default:                                return "engine refused";
        }
    }

    // The crafts a bot may take. Parent skill lines only — the per-expansion children are derived
    // from the client data below, the same way the gathering skills are, so an expansion added later
    // works without editing this list.
    //
    // Gathering is deliberately absent: bots already have herbalism and mining, and those feed these.
    constexpr uint32 CRAFTS[] =
    {
        SKILL_ALCHEMY,
        SKILL_BLACKSMITHING,
        SKILL_TAILORING,
        SKILL_LEATHERWORKING,
        SKILL_ENGINEERING,
        SKILL_JEWELCRAFTING,
        SKILL_INSCRIPTION,
        SKILL_ENCHANTING,
        SKILL_COOKING,
    };

    constexpr uint16 CRAFT_SKILL_MAX = 950;

    // Recipes the engine refused to teach — spells that are in the skill-line data but not in this
    // client build. Shared by every bot on purpose: a spell that does not exist does not exist for
    // any of them, and re-discovering that sixty times over is exactly the waste this ends.
    std::unordered_set<uint32> _unlearnable;

    // Skill a bot of this level would plausibly have reached. Mirrors the gathering module's shape:
    // a level-40 character with a profession is not a novice at it, and starting everyone at 1 would
    // mean nobody ever crafts anything above the first tier.
    uint16 SkillValueForLevel(uint8 level)
    {
        return std::min<uint16>(CRAFT_SKILL_MAX, uint16(level) * 5);
    }

    bool CreatesAnItem(SpellInfo const* spell)
    {
        for (SpellEffectInfo const& effect : spell->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_CREATE_ITEM) || effect.IsEffect(SPELL_EFFECT_CREATE_LOOT)
               )
                return true;

        return false;
    }

    // Does this recipe produce a plain bag?
    //
    // Worth knowing because a bot short of room should make the bag BEFORE it makes anything else:
    // crafting needs two free slots to put the result in, so a tailor with full bags eventually
    // cannot practise its trade at all — and the one recipe that would fix that is the one it never
    // gets around to. Specialised containers (herb, mining, enchanting) are excluded for the same
    // reason PbotBags excludes them: they hold only their own kind of goods, so they add no room
    // for a quest reward.
    bool MakesABag(SpellInfo const* spell)
    {
        for (SpellEffectInfo const& effect : spell->GetEffects())
        {
            if (!effect.IsEffect(SPELL_EFFECT_CREATE_ITEM))
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(effect.ItemType);
            if (proto && proto->GetClass() == ITEM_CLASS_CONTAINER
                && proto->GetSubClass() == ITEM_SUBCLASS_CONTAINER)
                return true;
        }

        return false;
    }

    // Does the bot hold everything this recipe consumes?
    bool HasReagentsFor(Player* bot, SpellInfo const* spell)
    {
        bool wantsSomething = false;

        for (size_t i = 0; i < spell->Reagent.size(); ++i)
        {
            if (spell->Reagent[i] <= 0 || spell->ReagentCount[i] <= 0)
                continue;

            wantsSomething = true;
            if (!bot->HasItemCount(uint32(spell->Reagent[i]), uint32(spell->ReagentCount[i])))
                return false;
        }

        // A recipe with no reagents at all is not a craft — it is a trainer entry, a proc, or some
        // other ability that happens to live on the skill line. Making those would be free items.
        return wantsSomething;
    }

    constexpr uint32 WORKBENCH_POINT_ID = 0xC0F;   // distinct from upkeep/gather/quest/travel ids
    constexpr float WORKBENCH_SEARCH_RANGE = 100.0f;
    constexpr float WORKBENCH_STAND_RANGE = 4.0f;  // inside any focus radius, which is 10y by default

    // Nearest object that provides the focus this recipe wants.
    //
    // A game object can offer up to four different focus types — one anvil doubles as several
    // workbenches — so all four are compared rather than only the first.
    class WorkbenchCheck
    {
    public:
        WorkbenchCheck(Player const* bot, uint32 focus, GameObject*& nearest)
            : _bot(bot), _focus(focus), _nearest(nearest), _bestDistance(WORKBENCH_SEARCH_RANGE) { }

        bool operator()(GameObject* go)
        {
            GameObjectTemplate const* info = go->GetGOInfo();
            if (!info || info->type != GAMEOBJECT_TYPE_SPELL_FOCUS)
                return false;

            if (info->spellFocus.spellFocusType != _focus && info->spellFocus.spellFocusType2 != _focus
                && info->spellFocus.spellFocusType3 != _focus && info->spellFocus.spellFocusType4 != _focus)
                return false;

            float const distance = _bot->GetDistance(go);
            if (distance >= _bestDistance)
                return false;

            _bestDistance = distance;
            _nearest = go;
            return false;   // never a match — keeping the nearest is the whole point
        }

    private:
        Player const* _bot;
        uint32 _focus;
        GameObject*& _nearest;
        float _bestDistance;
    };

    bool WalkToWorkbench(Player* bot, uint32 focus)
    {
        GameObject* nearest = nullptr;
        GameObject* unused = nullptr;
        WorkbenchCheck check(bot, focus, nearest);
        Trinity::GameObjectSearcher<WorkbenchCheck> searcher(bot, unused, check);
        Cell::VisitAllObjects(bot, searcher, WORKBENCH_SEARCH_RANGE);

        if (!nearest)
            return false;

        // Already standing at it and still refused — then the focus was not the problem after all,
        // and walking the same four yards again would be a loop rather than a fix.
        if (bot->GetDistance(nearest) <= WORKBENCH_STAND_RANGE)
            return false;

        float const angle = nearest->GetAbsoluteAngle(bot);
        float x = nearest->GetPositionX() + std::cos(angle) * WORKBENCH_STAND_RANGE;
        float y = nearest->GetPositionY() + std::sin(angle) * WORKBENCH_STAND_RANGE;
        float z = nearest->GetPositionZ();
        bot->UpdateAllowedPositionZ(x, y, z);
        bot->GetMotionMaster()->MovePoint(WORKBENCH_POINT_ID, x, y, z);

        TC_LOG_INFO("scripts.bots", "pbot craft: {} walked to a workbench for focus {} ({:.0f}y away)",
            bot->GetName(), focus, bot->GetDistance(nearest));
        return true;
    }
}

uint32 PbotProfession::CraftFor(Player* bot)
{
    if (!bot)
        return 0;

    // Spread by guid, so the population covers every craft instead of all picking the same one, and
    // so a given bot always practises the same trade across restarts.
    return CRAFTS[bot->GetGUID().GetCounter() % std::size(CRAFTS)];
}

void PbotProfession::GrantCraft(Player* bot)
{
    if (!bot)
        return;

    uint32 const craft = CraftFor(bot);
    if (!craft)
        return;

    uint16 const value = SkillValueForLevel(bot->GetLevel());

    auto grant = [bot, value](uint32 skillId)
    {
        if (!skillId)
            return;

        // Raise it, do not merely add it. Some crafts arrive with the character: cooking is a
        // default skill, so a bot that drew cooking already "had" it — at value 1 — and the old
        // has-it-already guard skipped the grant, leaving a level-40 cook a permanent novice who
        // could learn nothing beyond the first recipe. Skill earned above what we would hand out
        // is left alone.
        if (bot->HasSkill(skillId) && bot->GetSkillValue(skillId) >= value)
            return;

        bot->SetSkill(skillId, 1, value, CRAFT_SKILL_MAX);
    };

    grant(craft);

    // Retail splits each profession into a skill line per expansion, and a recipe demands the line
    // for ITS expansion. Granting only the parent leaves a bot unable to make anything modern —
    // the identical trap the gathering skills had to solve.
    if (std::vector<SkillLineEntry const*> const* children = sDB2Manager.GetSkillLinesForParentSkill(craft))
        for (SkillLineEntry const* child : *children)
            grant(child->ID);
}

uint32 PbotProfession::LearnRecipes(Player* bot)
{
    if (!bot)
        return 0;

    uint32 const craft = CraftFor(bot);
    if (!craft || !bot->HasSkill(craft))
        return 0;

    uint32 learned = 0;

    auto learnFrom = [bot, &learned](uint32 skillId)
    {
        std::vector<SkillLineAbilityEntry const*> const* abilities =
            sDB2Manager.GetSkillLineAbilitiesBySkill(skillId);
        if (!abilities)
            return;

        uint16 const skill = bot->GetSkillValue(skillId);

        for (SkillLineAbilityEntry const* ability : *abilities)
        {
            if (!ability->Spell || bot->HasSpell(uint32(ability->Spell))
                || _unlearnable.count(uint32(ability->Spell)))
                continue;

            // Only what this crafter could actually attempt, plus a little headroom so skill gained
            // between passes is not wasted waiting for the next one.
            if (ability->MinSkillLineRank > int16(skill + LEARN_AHEAD))
                continue;

            SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(ability->Spell), DIFFICULTY_NONE);
            if (!spell || !CreatesAnItem(spell))
                continue;

            bot->LearnSpell(uint32(ability->Spell), /*dependent*/ false, int32(skillId));

            // Count it only if it STUCK — the same discipline the quest accept path had to learn.
            //
            // Player::AddSpell rejects a spell the client build does not have and says so; the bot
            // then arrives at this loop again next pass, tries the identical rejected recipe, and
            // "learns" it again forever. Measured over five hours: 12,696 "learned N recipes" lines
            // and 55,830 "Player::AddSpell: Spell is invalid" errors beside them, against THIRTEEN
            // quest lines from sixty bots. The population spent its entire day re-learning recipes
            // that never existed.
            //
            // Remembering the refusals is what ends it: without the set, this loop would still walk
            // the same dead abilities every pass, merely more quietly.
            if (bot->HasSpell(uint32(ability->Spell)))
                ++learned;
            else
                _unlearnable.insert(uint32(ability->Spell));
        }
    };

    auto learnLine = [bot, &learnFrom](uint32 parentSkill)
    {
        if (!bot->HasSkill(parentSkill))
            return;

        learnFrom(parentSkill);
        if (std::vector<SkillLineEntry const*> const* children = sDB2Manager.GetSkillLinesForParentSkill(parentSkill))
            for (SkillLineEntry const* child : *children)
                if (bot->HasSkill(child->ID))
                    learnFrom(child->ID);
    };

    learnLine(craft);

    // And the PROCESSING recipes, which hang off the GATHERING skills rather than the craft.
    //
    // This is the step between digging something up and making something of it: smelting turns ore
    // into bars, and a blacksmith with a bag of ore and no smelting knows a hundred recipes it can
    // never start. Measured before this: 639 recipes learned across the population and FOUR items
    // made — the bots knew what to build and had nothing to build it from.
    //
    // Every miner can smelt, so this is granted to all of them regardless of which craft they took:
    // an alchemist who mines still smelts the ore, and bars sell.
    learnLine(SKILL_MINING);
    learnLine(SKILL_HERBALISM);

    if (learned)
        TC_LOG_INFO("scripts.bots", "pbot craft: {} learned {} new recipes for skill {}",
            bot->GetName(), learned, craft);

    return learned;
}

PbotProfession::CraftOutcome PbotProfession::Craft(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return CraftOutcome::Idle;

    uint32 const craft = CraftFor(bot);
    if (!craft || !bot->HasSkill(craft))
        return CraftOutcome::Idle;

    uint32 attempted = 0;
    uint32 refusedSpell = 0;
    uint32 wantedFocus = 0;
    SpellCastResult refusal = SPELL_CAST_OK;

    // Walk what the bot knows and make the first thing it has materials for — except that a bot
    // short of room makes a BAG first if it knows how.
    //
    // The ordering is not a nicety. Crafting needs two free slots for the result, so a tailor with
    // full bags stops being able to practise its trade at all, and the single recipe that would
    // undo that is the one it never reaches. It is also simply what a tailor is for: a bot that
    // gathers cloth all day and then buys a bag with coin is a strange sort of tailor.
    std::vector<uint32> order;
    order.reserve(bot->GetSpellMap().size());

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
        if (playerSpell.state != PLAYERSPELL_REMOVED && !playerSpell.disabled)
            order.push_back(spellId);

    if (PbotBags::WantsABag(bot))
    {
        std::stable_sort(order.begin(), order.end(), [](uint32 a, uint32 b)
        {
            SpellInfo const* sa = sSpellMgr->GetSpellInfo(a, DIFFICULTY_NONE);
            SpellInfo const* sb = sSpellMgr->GetSpellInfo(b, DIFFICULTY_NONE);
            return (sa && MakesABag(sa)) > (sb && MakesABag(sb));
        });
    }

    // Casting is the whole craft: the engine consumes the reagents, creates the item and raises the
    // skill through Player::UpdateCraftSkill, exactly as when a player clicks a recipe. Doing any of
    // that by hand here would be reimplementing the profession system beside the real one.
    for (uint32 spellId : order)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spell || !CreatesAnItem(spell) || !HasReagentsFor(bot, spell))
            continue;

        // A recipe that needs a profession TOOL - a smith's hammer, a mining pick - is refused with
        // SPELL_FAILED_TOTEM_CATEGORY (152), a name that reads like a totem problem and sent the
        // investigation looking at shamans. The engine's own test is HasItemTotemCategory over each
        // declared category, so ask the same question here and skip the recipe: without the tool the
        // refusal can never change, and every craft interval spent on it is wasted.
        bool hasTools = true;
        for (int32 totemCategory : spell->TotemCategory)
            if (totemCategory && !bot->HasItemTotemCategory(uint32(totemCategory)))
            {
                hasTools = false;
                break;
            }
        if (!hasTools)
            continue;

        // Somewhere to put the result. A bot with jammed bags would burn the reagents for nothing.
        if (bot->GetFreeInventorySlotCount() < 2)
            return CraftOutcome::Idle;

        // And do not ADD to a hoard. A bot short of room may only make the one thing that fixes
        // that — a bag — and must otherwise go and sell before it brews anything else.
        //
        // Measured: an alchemist filled all four bags with 107 copies of a single potion and then
        // stood at a questgiver unable to accept a reward. Its trade was not the problem; making
        // more of the thing that had already filled the bags was.
        if (!MakesABag(spell) && PbotBelongings::NeedsRoom(bot))
            return CraftOutcome::Idle;

        // Get off the horse. Nobody smiths from the saddle, and the engine agrees: the refusal is
        // SPELL_FAILED_NOT_MOUNTED, which despite its name means "you are mounted". This is the
        // third wall in a row and each one was hidden behind the last — the bot rides to the anvil
        // and would have sat there refusing its own recipes forever.
        //
        // Only here, where a recipe with materials is actually about to be cast: dismounting on
        // every craft tick would strand travelling bots on foot for nothing.
        PbotMount::Dismount(bot);

        // Instant, but still paid for. TRIGGERED_IGNORE_CAST_TIME removes the cast bar and NOTHING
        // else: it is deliberately excluded from TRIGGERED_IS_TRIGGERED_MASK, so CheckCast runs in
        // full — the forge or cooking fire is still required, the reagents are still taken, the
        // skill still goes up.
        //
        // A plain cast does not work for a bot. Measured: 62 casts, not one reagent consumed, skill
        // still 1 — a trade spell takes seconds and a bot is walking somewhere for every one of
        // them, so it interrupted itself every single time.
        ++attempted;

        SpellCastResult const result =
            bot->CastSpell(bot, spellId, CastSpellExtraArgs(TRIGGERED_IGNORE_CAST_TIME));

        // A refusal is not the end of the pass. Most recipes need a workbench the bot is nowhere
        // near, and the next one down the list may need nothing at all — so try it, rather than
        // giving up for another interval because the first thing looked at happened to need a forge.
        if (result != SPELL_CAST_OK)
        {
            refusal = result;
            refusedSpell = spellId;
            if (result == SPELL_FAILED_REQUIRES_SPELL_FOCUS && !wantedFocus)
                wantedFocus = spell->RequiresSpellFocus;
            continue;
        }

        TC_LOG_INFO("scripts.bots", "pbot craft: {} made something with spell {} (skill {} now {})",
            bot->GetName(), spellId, craft, uint32(bot->GetSkillValue(craft)));

        // A bag it just sewed belongs on its back, not in the pack taking up the room it adds.
        // Collect before installing — moving items about from inside a walk over those same
        // containers is the mistake this codebase has made more than once.
        if (MakesABag(spell))
        {
            std::vector<Item*> loose;
            bot->ForEachItem(ItemSearchLocation::Inventory, [&loose](Item* item)
            {
                if (item->IsInBag() || item->GetSlot() >= INVENTORY_SLOT_ITEM_START)
                    loose.push_back(item);
                return ItemSearchCallbackResult::Continue;
            });

            for (Item* item : loose)
                if (PbotBags::Install(bot, item))
                    break;
        }
        return CraftOutcome::Made;
    }

    if (!attempted)
        return CraftOutcome::Idle;   // nothing in the bags to work with — that is a supply problem

    // Materials in hand and every recipe refused. If what stood in the way was a workbench, go to
    // one; that is the whole difference between a bot that owns a profession and a bot that
    // practises it.
    if (wantedFocus && WalkToWorkbench(bot, wantedFocus))
        return CraftOutcome::WalkingToWorkbench;

    // Say WHY nothing was made. Silence here is what cost the earlier measurements: an attempt and
    // a success looked identical in the log, so 62 refusals read as 62 crafts.
    //
    // Once every ten minutes per bot, not every craft interval: "waiting for a workbench" is a state,
    // and repeating it every minute is how a real message becomes noise. Measured 2026-10-21: five
    // identical lines per bot inside one workbench trip, all of them the same wait.
    static std::unordered_map<ObjectGuid, uint32> lastRefusalLog;
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32& lastLog = lastRefusalLog[bot->GetGUID()];
    if (!lastLog || nowMs - lastLog >= 600000)
    {
        lastLog = nowMs;
        TC_LOG_INFO("scripts.bots", "pbot craft: {} has materials for {} recipes, none made - {} "
            "(last spell {} -> result {})", bot->GetName(), attempted, RefusalName(refusal),
            refusedSpell, uint32(refusal));
    }

    return CraftOutcome::Idle;
}

std::vector<uint32> PbotProfession::NeededToolCategories(Player* bot)
{
    std::vector<uint32> categories;
    if (!bot)
        return categories;

    // Same walk as the craft loop: only spells that create an item are recipes, and only the ones
    // the bot actually knows can be made at all.
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled)
            continue;

        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spell || !CreatesAnItem(spell))
            continue;

        for (int32 category : spell->TotemCategory)
        {
            if (!category || bot->HasItemTotemCategory(uint32(category)))
                continue;

            if (std::find(categories.begin(), categories.end(), uint32(category)) == categories.end())
                categories.push_back(uint32(category));
        }
    }

    return categories;
}

uint32 PbotProfession::NeededFocus(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return 0;

    uint32 const craft = CraftFor(bot);
    if (!craft || !bot->HasSkill(craft))
        return 0;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled)
            continue;

        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spell || !spell->RequiresSpellFocus || !CreatesAnItem(spell))
            continue;

        if (HasReagentsFor(bot, spell))
            return spell->RequiresSpellFocus;
    }

    return 0;
}

std::unordered_set<uint32> PbotProfession::KnownReagents(Player* bot)
{
    std::unordered_set<uint32> reagents;
    if (!bot)
        return reagents;

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled)
            continue;

        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spell || !CreatesAnItem(spell))
            continue;

        for (size_t i = 0; i < spell->Reagent.size(); ++i)
            if (spell->Reagent[i] > 0 && spell->ReagentCount[i] > 0)
                reagents.insert(uint32(spell->Reagent[i]));
    }

    return reagents;
}

std::string PbotProfession::Describe(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return "not in world";

    uint32 const craft = CraftFor(bot);
    if (!craft)
        return "no craft";

    uint32 known = 0;
    uint32 makeable = 0;
    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled)
            continue;

        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spell || !CreatesAnItem(spell))
            continue;

        ++known;
        if (HasReagentsFor(bot, spell))
            ++makeable;
    }

    return Trinity::StringFormat("craft {} at skill {}, {} recipes known, {} makeable now",
        craft, uint32(bot->GetSkillValue(craft)), known, makeable);
}
