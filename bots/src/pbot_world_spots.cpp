/*
 * Companion Bots — world population spots. See pbot_world_spots.h for the rationale.
 */

#include "pbot_world_spots.h"

#include "Containers.h"   // Trinity::Containers::RandomShuffle
#include "DB2Stores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MapManager.h"   // sMapMgr->FindMap — never migrate onto an unloaded continent
#include "Random.h"       // urand

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
    // Sample stride over the creature table. The full table is ~700k rows across every map; we only
    // need "somewhere plausible to stand" per zone, so one row in 37 (~16k rows) is already far more
    // than any bot population will consume, and it keeps the one-time query to a plain scan with no
    // sort — ORDER BY RAND() over the whole table would stall the world thread for seconds.
    // 37 is prime so it does not resonate with any block structure in the guid allocation.
    constexpr uint32 SAMPLE_STRIDE = 37;

    // Cap per zone. Beyond this the extra rows only make the load heavier: a zone contributes at
    // most a handful of bots in any realistic population.
    constexpr size_t MAX_SPOTS_PER_ZONE = 40;

    // Half-width (yards) of the square around a map's origin whose spawn rows are treated as
    // placeholders rather than places. See the filter in EnsureLoaded.
    constexpr float ORIGIN_EXCLUSION = 100.0f;

    // How far a destination zone's band may sit from the bot's level when it moves house. A player
    // moving on does not jump to content far above them, and a zone a little under their level is
    // still worth clearing, so the window is deliberately lopsided.
    constexpr uint32 BAND_TOLERANCE_BELOW = 3;   // zone band up to 3 levels ABOVE the bot
    constexpr uint32 BAND_TOLERANCE_ABOVE = 8;   // zone band up to 8 levels BELOW the bot

    // Where inside that window a bot would rather live: a zone whose band sits this far under its
    // level is content it can actually clear, which is where a player of that level goes.
    constexpr int32 IDEAL_GAP = 2;

    // Candidates this close to the best fit are all treated as equally good, so a whole population
    // migrating at once does not funnel into one zone.
    constexpr int32 FIT_SLACK = 3;

    // key = (mapId << 32) | zoneId
    using ZoneKey = uint64;
    constexpr ZoneKey MakeKey(uint32 mapId, uint32 zoneId) { return (uint64(mapId) << 32) | zoneId; }
    constexpr uint32 MapOf(ZoneKey key) { return uint32(key >> 32); }

    std::unordered_map<ZoneKey, std::vector<PbotWorldSpots::Spot>> _byZone;

    // Spawn points of quest-giving NPCs, per zone. A separate table rather than a flag on the
    // general one because it is preferred, not mixed in: the first live run had six bots in forty
    // holding a quest, simply because a bot dropped in open country rarely wanders into a quest
    // hub. Starting bots where the questgivers stand is where players are anyway.
    std::unordered_map<ZoneKey, std::vector<PbotWorldSpots::Spot>> _hubsByZone;

    bool _loaded = false;
    uint32 _total = 0;
    uint32 _hubTotal = 0;

    // Loads every quest-giver spawn point, unsampled.
    //
    // No guid stride here, unlike the general table: questgivers are a small, sparse subset (a few
    // tens of thousands of rows out of 730k), and sampling them one in 37 would leave most zones
    // with no hub at all — the sampled version covered 165 zones where the full one covers 392.
    void LoadQuestHubs()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT c.map, c.zoneId, c.position_x, c.position_y, c.position_z "
            "FROM creature c JOIN creature_template t ON t.entry = c.id "
            "WHERE c.zoneId > 0 AND (t.npcflag & 2) <> 0");

        if (!result)
        {
            TC_LOG_ERROR("scripts.bots", "pbot: quest hub query returned nothing — bots will be "
                "placed on general creature spawns instead");
            return;
        }

        do
        {
            Field* f = result->Fetch();
            uint32 const mapId  = f[0].GetUInt32();
            uint32 const zoneId = f[1].GetUInt32();

            MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
            if (!mapEntry || !mapEntry->IsContinent())
                continue;

            float const x = f[2].GetFloat();
            float const y = f[3].GetFloat();
            if (std::abs(x) < ORIGIN_EXCLUSION && std::abs(y) < ORIGIN_EXCLUSION)
                continue;

            std::vector<PbotWorldSpots::Spot>& bucket = _hubsByZone[MakeKey(mapId, zoneId)];
            if (bucket.size() >= MAX_SPOTS_PER_ZONE)
                continue;

            PbotWorldSpots::Spot spot;
            spot.MapId  = mapId;
            spot.ZoneId = zoneId;
            spot.Pos.Relocate(x, y, f[4].GetFloat(), 0.0f);
            bucket.push_back(spot);
            ++_hubTotal;
        }
        while (result->NextRow());

        TC_LOG_INFO("scripts.bots", "pbot: loaded {} quest hub spots across {} zones",
            _hubTotal, uint32(_hubsByZone.size()));
    }

    // Fills in Spot::SuggestedLevel for every zone already loaded.
    //
    // The world DB stores no creature level: retail scales creatures, so what a zone is "for" lives
    // in ContentTuning (client data), reached via creature_template_difficulty.ContentTuningID. A
    // zone usually mixes a few tunings, so the one carried by the MOST creatures wins — the level
    // band a character walking around that zone actually meets.
    void LoadZoneLevels()
    {
        QueryResult result = WorldDatabase.PQuery(
            "SELECT c.map, c.zoneId, d.ContentTuningID, COUNT(*) FROM creature c "
            "JOIN creature_template_difficulty d ON d.Entry = c.id AND d.DifficultyID = 0 "
            "WHERE c.zoneId > 0 AND (c.guid % {}) = 0 AND d.ContentTuningID > 0 "
            "GROUP BY c.map, c.zoneId, d.ContentTuningID", SAMPLE_STRIDE);

        if (!result)
        {
            TC_LOG_ERROR("scripts.bots", "pbot: zone level query returned nothing — populated bots "
                "will fall back to the level given on the command line");
            return;
        }

        // zone key -> (best row count so far, level picked from that row)
        std::unordered_map<ZoneKey, std::pair<uint32, uint8>> best;

        do
        {
            Field* f = result->Fetch();
            ZoneKey const key = MakeKey(f[0].GetUInt32(), f[1].GetUInt32());
            uint32 const tuningId = f[2].GetUInt32();
            uint32 const rows = f[3].GetUInt32();

            auto it = best.find(key);
            if (it != best.end() && it->second.first >= rows)
                continue;

            // Ask the engine to resolve the band rather than reading ContentTuning::MinLevel
            // directly. The raw fields are not the effective range — MinLevelType/MaxLevelType and
            // the redirect chain decide that — and taking them at face value put a level-1 bot in
            // an endgame zone (Zaralek Cavern) in the first live run.
            Optional<ContentTuningLevels> const levels = sDB2Manager.GetContentTuningData(tuningId, {});
            if (!levels || levels->MaxLevel <= 0)
                continue;

            // Middle of the band. The low end is where a character arrives and the high end where
            // they leave; the middle is where most of the zone's content sits, and it keeps a bot
            // eligible for quests at both ends of the range.
            int32 const mid = (std::max<int32>(levels->MinLevel, 1) + levels->MaxLevel) / 2;
            best[key] = { rows, uint8(std::clamp<int32>(mid, 1, 255)) };
        }
        while (result->NextRow());

        uint32 tuned = 0;
        auto applyTo = [&best, &tuned](std::unordered_map<ZoneKey, std::vector<PbotWorldSpots::Spot>>& table)
        {
            for (auto& kv : table)
            {
                auto it = best.find(kv.first);
                if (it == best.end())
                    continue;

                for (PbotWorldSpots::Spot& spot : kv.second)
                    spot.SuggestedLevel = it->second.second;
                ++tuned;
            }
        };
        applyTo(_byZone);
        applyTo(_hubsByZone);

        TC_LOG_INFO("scripts.bots", "pbot: derived a level band for {} zone entries across both "
            "spot tables ({} general + {} hub zones)", tuned, uint32(_byZone.size()),
            uint32(_hubsByZone.size()));
    }

    void EnsureLoaded()
    {
        if (_loaded)
            return;
        _loaded = true; // set first: a failed query must not retry on every command

        QueryResult result = WorldDatabase.PQuery(
            "SELECT map, zoneId, position_x, position_y, position_z FROM creature "
            "WHERE zoneId > 0 AND (guid % {}) = 0", SAMPLE_STRIDE);

        if (!result)
        {
            TC_LOG_ERROR("scripts.bots", "pbot: world population spots query returned nothing");
            return;
        }

        uint32 skippedMap = 0;
        uint32 skippedOrigin = 0;
        do
        {
            Field* f = result->Fetch();
            uint32 const mapId  = f[0].GetUInt32();
            uint32 const zoneId = f[1].GetUInt32();

            // Continents only — the engine's own list, not merely "not an instance".
            //
            // IsWorldMap() was the first attempt and it is too generous: it also admits things like
            // Deeprun Tram, garrisons and Argus Invasion Points. A bot placed on Invasion Points
            // woke a boss whose script drove Unit::SetCharm into an ASSERT and killed the server.
            // Those maps are scripted content that assumes a raid arriving deliberately, not a
            // resident. IsContinent() is maintained upstream, so new expansions come along for free.
            MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
            if (!mapEntry || !mapEntry->IsContinent())
            {
                ++skippedMap;
                continue;
            }

            float const x = f[2].GetFloat();
            float const y = f[3].GetFloat();

            // Drop the cluster sitting on the map origin. Those rows are placeholders rather than
            // real placements, and the first live run put two bots at (10, 20) and (16, -3) on the
            // Shadowlands map, standing in nothing. Losing whatever genuine content happens to sit
            // within a few metres of (0,0) costs nothing against 5987 candidate spots.
            if (std::abs(x) < ORIGIN_EXCLUSION && std::abs(y) < ORIGIN_EXCLUSION)
            {
                ++skippedOrigin;
                continue;
            }

            std::vector<PbotWorldSpots::Spot>& bucket = _byZone[MakeKey(mapId, zoneId)];
            if (bucket.size() >= MAX_SPOTS_PER_ZONE)
                continue;

            PbotWorldSpots::Spot spot;
            spot.MapId  = mapId;
            spot.ZoneId = zoneId;
            spot.Pos.Relocate(x, y, f[4].GetFloat(), 0.0f);
            bucket.push_back(spot);
            ++_total;
        }
        while (result->NextRow());

        LoadQuestHubs();
        LoadZoneLevels();

        TC_LOG_INFO("scripts.bots", "pbot: loaded {} world population spots across {} zones "
            "({} rows skipped as non-continent maps, {} as map-origin placeholders)",
            _total, uint32(_byZone.size()), skippedMap, skippedOrigin);
    }

    // Zone keys matching the filter, in random order so successive populate calls do not always
    // start from the same continent.
    std::vector<ZoneKey> MatchingZones(int32 mapFilter)
    {
        // Union of both tables: a zone whose only usable point is a quest hub still counts, and one
        // with only general spawns still counts. Duplicates are removed by the sort below.
        std::vector<ZoneKey> keys;
        keys.reserve(_byZone.size() + _hubsByZone.size());
        for (auto const& kv : _byZone)
            if (mapFilter < 0 || MapOf(kv.first) == uint32(mapFilter))
                keys.push_back(kv.first);
        for (auto const& kv : _hubsByZone)
            if (mapFilter < 0 || MapOf(kv.first) == uint32(mapFilter))
                keys.push_back(kv.first);

        // Sort before shuffling: unordered_map iteration order is unspecified, and a stable base
        // order keeps the shuffle the only source of variation rather than allocator behaviour.
        // The sort also collapses the two tables' overlap.
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        Trinity::Containers::RandomShuffle(keys);
        return keys;
    }
}

namespace PbotWorldSpots
{

std::vector<Spot> Pick(uint32 count, int32 mapFilter, uint32 maxMaps)
{
    EnsureLoaded();

    std::vector<Spot> picked;
    std::vector<ZoneKey> zones = MatchingZones(mapFilter);
    if (zones.empty() || !count)
        return picked;

    // Confine the batch to a handful of maps before spreading it over zones — see DEFAULT_MAX_MAPS
    // for why breadth across continents is the expensive axis. The zone list is already shuffled,
    // so taking the maps in the order they first appear picks a random subset of continents.
    if (maxMaps)
    {
        std::vector<uint32> chosenMaps;
        for (ZoneKey key : zones)
        {
            uint32 const mapId = MapOf(key);
            if (std::find(chosenMaps.begin(), chosenMaps.end(), mapId) == chosenMaps.end())
            {
                if (chosenMaps.size() >= maxMaps)
                    continue;
                chosenMaps.push_back(mapId);
            }
        }

        std::erase_if(zones, [&chosenMaps](ZoneKey key)
        {
            return std::find(chosenMaps.begin(), chosenMaps.end(), MapOf(key)) == chosenMaps.end();
        });

        if (zones.empty())
            return picked;
    }

    picked.reserve(count);

    // Round-robin over zones: bot 0 goes to zone 0, bot 1 to zone 1 ... so the batch spreads over
    // as many zones as it has members before any zone gets a second bot.
    for (uint32 i = 0; i < count; ++i)
    {
        ZoneKey const key = zones[i % zones.size()];

        // Prefer a quest hub in this zone; fall back to a general spawn point where the zone has
        // no questgiver at all. Standing a bot next to the exclamation mark is what turned quest
        // uptake from "six bots in forty" into something a populated world actually looks like.
        auto hubIt = _hubsByZone.find(key);
        std::vector<Spot> const& bucket = (hubIt != _hubsByZone.end() && !hubIt->second.empty())
                                        ? hubIt->second
                                        : _byZone[key];
        if (bucket.empty())
            continue;

        picked.push_back(bucket[urand(0, uint32(bucket.size()) - 1)]);
    }
    return picked;
}

uint32 ZoneCount(int32 mapFilter)
{
    EnsureLoaded();
    return uint32(MatchingZones(mapFilter).size());
}

uint32 SpotCount()
{
    EnsureLoaded();
    return _total;
}

void Preload()
{
    EnsureLoaded();
}

uint8 BandForZone(uint32 mapId, uint32 zoneId)
{
    EnsureLoaded();

    ZoneKey const key = MakeKey(mapId, zoneId);

    // Either table answers — both carry the same band for a given zone.
    auto hubIt = _hubsByZone.find(key);
    if (hubIt != _hubsByZone.end() && !hubIt->second.empty())
        return hubIt->second.front().SuggestedLevel;

    auto it = _byZone.find(key);
    if (it != _byZone.end() && !it->second.empty())
        return it->second.front().SuggestedLevel;

    return 0;
}

bool PickForLevel(uint8 level, Spot& out)
{
    EnsureLoaded();

    // Every zone whose band fits this level AND whose map is already in memory.
    std::vector<Spot const*> candidates;

    auto collect = [&candidates, level](std::unordered_map<ZoneKey, std::vector<Spot>> const& table)
    {
        for (auto const& kv : table)
        {
            if (kv.second.empty())
                continue;

            uint8 const band = kv.second.front().SuggestedLevel;
            if (!band)
                continue;

            int32 const gap = int32(level) - int32(band);
            if (gap < -int32(BAND_TOLERANCE_BELOW) || gap > int32(BAND_TOLERANCE_ABOVE))
                continue;

            // instanceId 0 is the world instance of a continent; a null result means nothing has
            // loaded that map yet, and moving one bot there is not worth paying for it.
            if (!sMapMgr->FindMap(MapOf(kv.first), 0))
                continue;

            for (Spot const& spot : kv.second)
                candidates.push_back(&spot);
        }
    };

    collect(_hubsByZone);   // quest hubs first — a bot that moves house should land near work
    if (candidates.empty())
        collect(_byZone);

    if (candidates.empty())
        return false;

    // Prefer the best fit rather than any fit. The acceptance window is deliberately wide so that
    // a destination exists at all (zone bands are sparse — 15, 17, 22, 25, 27, 32, 45 on the
    // classic continents), but its far edge overlaps the "outgrown" threshold: picking blindly
    // could move a bot to a zone it has ALREADY nearly outgrown, and five minutes later it would
    // move again. Ranking by how well the band fits keeps the wide window without the churn.
    auto fitScore = [level](Spot const* spot)
    {
        // Ideal: a zone whose band sits a couple of levels under the bot — content it can clear.
        return std::abs(int32(level) - int32(spot->SuggestedLevel) - IDEAL_GAP);
    };

    int32 bestScore = fitScore(candidates.front());
    for (Spot const* spot : candidates)
        bestScore = std::min(bestScore, fitScore(spot));

    std::vector<Spot const*> best;
    for (Spot const* spot : candidates)
        if (fitScore(spot) <= bestScore + FIT_SLACK)
            best.push_back(spot);

    out = *best[urand(0, uint32(best.size()) - 1)];
    return true;
}

} // namespace PbotWorldSpots
