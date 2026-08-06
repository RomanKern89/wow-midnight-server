/*
 * Companion Bots — crafting professions (TrinityCore master, retail 12.0.7).
 *
 * Bots already gather: they mine ore and pick herbs, and that raw material now reaches a market that
 * works. What nobody in this world does is TURN it into anything. A server whose every character is
 * a gatherer and none a maker has an economy with a supply side and no industry — the ore piles up,
 * the auction house fills with reagents, and nothing is ever built from them.
 *
 * So each bot takes one craft, works at it, and sells what it makes. The engine does the hard parts:
 * casting a trade-skill spell consumes the reagents, creates the item, and raises the skill, exactly
 * as it does when a player clicks the recipe. This module only decides who learns what, when they
 * have the materials, and when they bother.
 *
 * The craft is derived from the bot's guid rather than assigned in a table, so the population spreads
 * itself across every profession the client data knows about — including ones added later.
 */

#ifndef TRINITYCORE_PBOT_PROFESSION_H
#define TRINITYCORE_PBOT_PROFESSION_H

#include "Define.h"

#include <string>
#include <unordered_set>

class Player;

namespace PbotProfession
{
    // How often a bot thinks about crafting. Slow: a craft is a few seconds of work and a skill
    // point, not something to do every tick, and the reagent scan walks the bags.
    constexpr uint32 CRAFT_INTERVAL_MS = 45000;

    // Recipes are learned in batches as skill rises. Learning every recipe the profession will ever
    // have on the first tick would hand a novice the whole book.
    constexpr uint16 LEARN_AHEAD = 25;

    // Which craft this bot practises, derived from its guid. Stable for the life of the character.
    uint32 CraftFor(Player* bot);

    // Grants the craft and its per-expansion child lines, once. Safe to call repeatedly.
    void GrantCraft(Player* bot);

    // Teaches every recipe of the bot's craft that its current skill can attempt. Called as skill
    // rises, so the book grows with the crafter.
    uint32 LearnRecipes(Player* bot);

    // How long to leave a bot alone once it has set off for a workbench. A forge found by the local
    // search is within a hundred yards, so this is a walk of seconds, not a journey.
    constexpr uint32 WORKBENCH_WALK_MS = 25000;

    enum class CraftOutcome : uint8
    {
        Idle,                   // nothing to make, or nothing to make it with
        Made,                   // one item created
        WalkingToWorkbench      // has the materials, needed a forge, and is on its way to one
    };

    // Makes one thing, if the bot knows a recipe it has the materials for. Skill-ups are the
    // engine's business, not ours.
    //
    // A bot that cannot craft where it stands walks to the nearest workbench instead of shrugging:
    // measured, every single refusal was SPELL_FAILED_REQUIRES_SPELL_FOCUS — the recipe needed a
    // forge, an anvil or a fire, and there are 8771 anvils and 944 forges in the world that the
    // bots were walking straight past.
    CraftOutcome Craft(Player* bot);

    // Which workbench this bot needs right now: the spell-focus id of a recipe it HAS the materials
    // for and cannot cast where it stands. 0 when it needs nothing, or nothing it lacks.
    //
    // The town errand asks, because a hundred-yard look around is a city radius and bots dig ore in
    // the field: measured, the ore finally flowed (1610 nodes in forty minutes) and the crafting
    // still refused with "requires spell focus", because between the vein and the bar there is a
    // journey to a forge — the same journey the bot already makes for repairs and for the market.
    uint32 NeededFocus(Player* bot);

    // Every item id consumed by a recipe this bot knows.
    //
    // The auction module asks so it does not sell a crafter's own raw material out from under it.
    // Without this a blacksmith lists the ore it just mined, and then stands at the anvil with a
    // hundred recipes and nothing to smelt — the gathering, the processing and the crafting are one
    // chain, and selling the first link breaks the other two.
    std::unordered_set<uint32> KnownReagents(Player* bot);

    // One line for the diagnostic: craft, skill, recipes known, and whether anything is makeable now.
    std::string Describe(Player* bot);
}

#endif // TRINITYCORE_PBOT_PROFESSION_H
