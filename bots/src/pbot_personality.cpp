/*
 * Companion Bots — personality implementation. See pbot_personality.h for why traits are derived
 * from the guid rather than stored.
 */

#include "pbot_personality.h"

#include <array>
#include <unordered_map>

namespace
{
    // Cheap, well-mixed integer hash (splitmix64 finalizer). Sequential guids differ in every bit
    // of the output, which matters because bots are created one after another — a weaker mix would
    // hand consecutive bots nearly identical characters.
    uint64 Mix(uint64 value)
    {
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

    uint8 TraitFrom(uint64 seed, uint32 index)
    {
        return uint8(Mix(seed + index * 0x100000001B3ull) % 101);
    }

    // A label for whichever trait stands out most. Purely for logs and the diagnostic command, but
    // it is what makes "are they actually different?" answerable at a glance instead of by staring
    // at six numbers per bot.
    char const* NameFor(PbotPersonality::Traits const& t)
    {
        struct Candidate { uint8 value; char const* name; };
        std::array<Candidate, 6> const ranked =
        {{
            { t.Aggression,  "боевой"    },
            { t.Caution,     "осторожный"},
            { t.Diligence,   "прилежный" },
            { t.Sociability, "общительный"},
            { t.Wanderlust,  "бродяга"   },
            { t.Greed,       "жадный"    }
        }};

        Candidate best = ranked[0];
        for (Candidate const& candidate : ranked)
            if (candidate.value > best.value)
                best = candidate;

        // Nobody is defined by a trait they only have an average amount of.
        return best.value >= 70 ? best.name : "обычный";
    }
}

namespace PbotPersonality
{

Traits const& Of(ObjectGuid guid)
{
    // Cached per guid: the derivation is cheap but this runs several times per bot per tick, and a
    // reference return keeps every call site free of copies.
    static std::unordered_map<ObjectGuid, Traits> cache;

    auto it = cache.find(guid);
    if (it != cache.end())
        return it->second;

    uint64 const seed = guid.GetCounter();

    Traits traits;
    traits.Aggression  = TraitFrom(seed, 1);
    traits.Caution     = TraitFrom(seed, 2);
    traits.Diligence   = TraitFrom(seed, 3);
    traits.Sociability = TraitFrom(seed, 4);
    traits.Wanderlust  = TraitFrom(seed, 5);
    traits.Greed       = TraitFrom(seed, 6);
    traits.Archetype   = NameFor(traits);

    return cache.emplace(guid, traits).first->second;
}

float Scale(uint8 trait, float baseline, float low, float high)
{
    float const fraction = float(trait) / 100.0f;
    return baseline * (low + (high - low) * fraction);
}

} // namespace PbotPersonality
