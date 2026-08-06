/*
 * Companion Bots — individual character (TrinityCore master, retail 12.0.7).
 *
 * Every bot has been running the identical algorithm with the identical constants, so sixty bots
 * were sixty copies of one player. Watch two of them for a minute and the repetition gives the
 * whole thing away — real players in the same zone behave visibly differently: one clears every
 * quest in order, one grinds the same pack for an hour, one runs off to look at something, one
 * sits down at the first scratch, one never stops talking.
 *
 * Traits are DERIVED FROM THE BOT'S GUID, not stored and not random per call. That matters:
 *   - the same bot has the same character every tick, for its whole life, with no table to load,
 *     no column to migrate and no state to get out of sync;
 *   - two bots created a second apart get different characters, because their guids differ;
 *   - it survives a restart for free, since the guid does.
 */

#ifndef TRINITYCORE_PBOT_PERSONALITY_H
#define TRINITYCORE_PBOT_PERSONALITY_H

#include "Define.h"
#include "ObjectGuid.h"

namespace PbotPersonality
{
    // Every trait is 0..100. They are read as multipliers on the shared constants rather than as
    // replacements for them, so the tuned baselines still hold and personality only spreads bots
    // around it.
    struct Traits
    {
        uint8 Aggression   = 50;   // how far it looks for a fight, how readily it takes one
        uint8 Caution      = 50;   // when it sits down to recover, how early it disengages
        uint8 Diligence    = 50;   // quests and objectives vs. grinding whatever is in front of it
        uint8 Sociability  = 50;   // how much it talks and whether it seeks company
        uint8 Wanderlust   = 50;   // how far it will travel for no particular reason
        uint8 Greed        = 50;   // how much it cares about loot and gathering nodes

        char const* Archetype = "средний";   // human-readable label, for logs and diagnostics
    };

    // The character of this bot. Stable for a given guid; cheap enough to call every tick.
    Traits const& Of(ObjectGuid guid);

    // Scales a baseline by a trait: trait 0 gives `low` x baseline, 100 gives `high` x baseline.
    float Scale(uint8 trait, float baseline, float low, float high);
}

#endif // TRINITYCORE_PBOT_PERSONALITY_H
