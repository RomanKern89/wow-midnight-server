/*
 * Companion Bots — world population spots (TrinityCore master, retail 12.0.7).
 *
 * Where do you put a bot that is supposed to look like a player living in the world?
 *
 * Not a hand-written table of coordinates: it would cover a handful of zones, rot the moment the
 * world DB changes, and say nothing about the continents added after it was written. The world DB
 * already contains a complete, per-zone, guaranteed-walkable answer — the `creature` table. Every
 * row in it is a position the world designers chose, on the ground, inside a zone that is actually
 * used. Sampling it gives population points for every zone of every continent for free, including
 * ones this code has never heard of.
 *
 * The spots are grouped by zone and handed out round-robin so that asking for 60 bots spreads them
 * over ~60 different zones instead of dropping them all in whichever zone happens to have the most
 * creature spawns (Elwynn and Durotar would otherwise swallow the whole batch).
 */

#ifndef TRINITYCORE_PBOT_WORLD_SPOTS_H
#define TRINITYCORE_PBOT_WORLD_SPOTS_H

#include "Define.h"
#include "Position.h"

#include <vector>

namespace PbotWorldSpots
{
    struct Spot
    {
        uint32   MapId  = 0;
        uint32   ZoneId = 0;
        Position Pos;

        // What level a character belongs at here, derived from the content tuning of the creatures
        // that live in this zone. 0 when the zone's creatures carry no tuning data.
        //
        // This is not decoration. A level-15 bot dropped in a level-60 zone cannot take a single
        // quest there (CanTakeQuest enforces the level requirement), cannot fight anything, and
        // simply dies over and over — so "spread bots over the world" without matching the level
        // to the zone produces a world full of corpses rather than a populated one.
        uint8    SuggestedLevel = 0;
    };

    // How many distinct maps one populate call will spread over by default.
    //
    // Not an arbitrary number: spreading 40 bots over every world map killed the server. Bots are
    // cheap (20 of them in a battleground cost no measurable tick time); MAPS are not. Every
    // continent a bot stands on pulls in that map's grids, vmaps and mmaps, and eleven continents
    // at once peaked at 26 GB and got the process OOM-killed. Population density per continent is
    // free, breadth across continents is not.
    // Raised from 3 to 5 once the cost was measured rather than guessed: with the population
    // confined to real continents, 60 bots over 3 maps sat at 17-20 GB of a 50 GB machine, where
    // the earlier unbounded version had peaked at 26 GB of 28 and died. Five continents is the
    // ceiling this hardware carries with room to spare, not the most it can be pushed to.
    constexpr uint32 DEFAULT_MAX_MAPS = 5;

    // Returns up to `count` spots, spread as evenly as the data allows across distinct zones, but
    // confined to at most maxMaps distinct maps. mapFilter < 0 selects every world map; otherwise
    // only that map. May return fewer than asked (or nothing at all) if the sample holds no
    // matching zone — the caller reports that.
    std::vector<Spot> Pick(uint32 count, int32 mapFilter, uint32 maxMaps);

    // How many distinct zones the loaded sample can offer for that filter. Lets a command tell the
    // operator "spread over N zones" and distinguishes "no spots" from "no bots spawned".
    uint32 ZoneCount(int32 mapFilter);

    // Rows kept after filtering, for diagnostics. Triggers the one-time load like Pick does.
    uint32 SpotCount();

    // The level band derived for a zone, or 0 if that zone carries no tuning data. Lets a bot ask
    // "am I too high level for where I am standing?" without remembering where it was placed.
    uint8 BandForZone(uint32 mapId, uint32 zoneId);

    // Picks a spot whose zone band suits `level`, for a bot that has outgrown its zone.
    //
    // Restricted to maps that are ALREADY LOADED on purpose: a migration that pulls in a fresh
    // continent would cost gigabytes for one bot, and loading maps is the one thing that has
    // repeatedly taken this server down. Returns false when nothing suitable is loaded — the bot
    // then simply stays where it is, which is a far better failure than an out-of-memory kill.
    bool PickForLevel(uint8 level, Spot& out);

    // Runs the one-time load now. Called from world startup on purpose: the two queries behind it
    // full-scan a 730k-row table (~1.8s together, since neither zoneId nor MOD(guid,N) is
    // indexable), and doing that lazily on the first populate command would freeze the world
    // thread — every online player hitching for two seconds — instead of costing two seconds of
    // boot nobody is waiting through.
    void Preload();
}

#endif // TRINITYCORE_PBOT_WORLD_SPOTS_H
