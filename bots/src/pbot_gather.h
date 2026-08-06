/*
 * Companion Bots — Phase 5 resource gathering (TrinityCore master, retail 12.0.7).
 *
 * Herb and ore nodes are GameObjects of type CHEST whose lock demands a gathering skill. A player
 * walks up and right-clicks; a bot has no client to right-click with, so it does the same thing the
 * click would have done server-side: GameObject::Use, then take what the resulting loot contains.
 *
 * Two things make this safe to run on every idle tick:
 *   - the node search is gated behind a cooldown, so a bot standing in a herb-free area is not
 *     paying for a grid visit several times a second;
 *   - a node is only ever approached if the bot's skill actually satisfies the lock, checked
 *     against the same Lock DB2 record the engine checks, so bots never walk to something they
 *     cannot open and then stand there.
 */

#ifndef TRINITYCORE_PBOT_GATHER_H
#define TRINITYCORE_PBOT_GATHER_H

#include "Define.h"
#include "ObjectGuid.h"

class GameObject;
class Player;

namespace PbotGather
{
    // How far a bot notices a node.
    //
    // Was 30y, and that was the whole reason the world had no raw materials: measured, sixty bots
    // gathered SIX nodes in forty minutes, and with nothing coming out of the ground there was
    // nothing to smelt, nothing to craft and nothing but looted gear to sell. A player does not
    // find veins by walking over them — tracking puts them on the minimap a hundred yards out —
    // so a bot that only notices what it nearly trips on gathers by accident, not on purpose.
    constexpr float SEARCH_RANGE = 80.0f;

    // Close enough to harvest. The engine's own interaction distance is smaller than this, so the
    // bot walks in until it is comfortably inside it.
    constexpr float INTERACT_RANGE = 4.0f;

    // Gap between node searches when the last one found nothing.
    //
    // Lengthened alongside the wider radius. A search costs roughly its area in grid cells, so the
    // two changes together leave the per-bot cost close to where it was while looking over seven
    // times as much ground — the point is to find more nodes, not to spend more of the world tick.
    constexpr uint32 SEARCH_COOLDOWN_MS = 10000;

    // How long to keep waiting for a node's loot to materialise after using it. Harvesting is not
    // instantaneous — GameObject::Use starts the opening/cast, and the loot only exists once that
    // resolves — so a single-tick "use it and read the loot" would walk away empty-handed every
    // time. Also the give-up bound if the loot never appears (someone else took the node).
    constexpr uint32 HARVEST_WAIT_MS = 6000;

    // Teaches the bot herbalism and mining at a value appropriate to its level. Called once at
    // spawn — a bot that cannot gather anything is not meaningfully "living in the world", and
    // there is no trainer interaction for it to do this the long way.
    void GrantGatheringSkills(Player* bot);

    // Nearest node within SEARCH_RANGE that this bot's skills can actually open, or nullptr.
    GameObject* FindNode(Player* bot);

    // One tick of the gather behaviour.
    //
    // Returns true when the bot is busy gathering (walking to a node or harvesting it), meaning the
    // caller should not also make it wander. Returns false when there is nothing to gather, leaving
    // the caller to do whatever it does otherwise.
    //
    // nodeGuid/cooldownMs/harvestWaitMs are the caller-owned state for this behaviour, stored per
    // bot in PbotAI. harvestWaitMs is non-zero exactly while the bot is standing at a node it has
    // used and is waiting for the loot to appear.
    bool Tick(Player* bot, ObjectGuid& nodeGuid, uint32& cooldownMs, uint32& harvestWaitMs, uint32 diff);
}

#endif // TRINITYCORE_PBOT_GATHER_H
