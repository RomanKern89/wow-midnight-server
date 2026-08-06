/*
 * Fake-player companion bots — identity data (TrinityCore master, retail 12.0.7).
 *
 * MVP scope per DESIGN_PHASE3.md SS8.1 / SS8.7: this "gear" file carries NO item-granting
 * code — starter gear/spells/action bars come for free from Player::Create() (Player.cpp
 * walks PlayerInfo::item). All this module owns is the race/class mapping table and the
 * player-style name pool + collision-avoiding picker that PbotMgr::SpawnBot consumes.
 *
 * Consumed by:
 *   - PbotMgr: ClassId / PickRaceForTeam / IsValidCombo / PickName during spawn.
 *   - cs_pbot.cpp: ParseClass / PickRandomClass / ClassTokenList (command input),
 *     ClassName / ClassNameForClassId (list output).
 *
 * Phase 4B replaced the fixed 2-race x 5-class MVP table with all 13 classes and a race choice
 * derived from the engine's own creation data at runtime — see PickRaceForTeam below.
 */
#pragma once

#include "pbot_mgr.h" // PbotClass
#include <cstdint>
#include <string>
#include <string_view>

namespace PbotIdentity
{
    // --- Class token / display-name / engine-class-id mapping (Phase 4B: all 13 classes) ---

    // Parses a user command token into a PbotClass. Case-insensitive, and accepts the common
    // shorthands players actually type ("dk", "dh", "lock", "pala", "рог", "друид", ...).
    // Returns false (leaving out untouched) for anything unrecognised.
    bool ParseClass(std::string_view token, PbotClass& out);

    // Space-separated list of the canonical class tokens, for command usage messages.
    char const* ClassTokenList();

    // Picks a random class — used by "any"/"random" spawns so an owner can fill a squad without
    // naming every class.
    PbotClass PickRandomClass();

    // Display name for a PbotClass, e.g. PbotClass::Warrior -> "Warrior".
    char const* ClassName(PbotClass classId);

    // Engine class id for a PbotClass, e.g. PbotClass::Warrior -> CLASS_WARRIOR (1).
    // This is what gets written into WorldPackets::Character::CharacterCreateInfo::Class.
    uint8 ClassId(PbotClass classId);

    // Display name derived from a live Unit class id (Player::GetClass()), for the list
    // command which reads the class off the resolved Player rather than the registry.
    // Falls back to "Unknown" for any class id outside our 5.
    char const* ClassNameForClassId(uint8 unitClassId);

    // --- Race selection per owner faction (Phase 4B) ---

    // Picks a random race that (a) belongs to the owner's faction and (b) can actually be that
    // class on THIS realm. Returns 0 when no race qualifies, which the caller must treat as a
    // spawn failure.
    //
    // The candidate set is derived from the engine's own data at runtime — sChrRacesStore for
    // existence, Player::TeamForRace for faction, and ObjectMgr::GetPlayerInfo(race, class) for
    // creatability — never from a table hardcoded here. That is what makes this correct across all
    // 31 playable races including allied races, Dracthyr and the Earthen/Midnight additions: the
    // exact same lookup Player::Create performs decides what we offer, so an impossible pairing is
    // unrepresentable rather than merely unlikely.
    uint8 PickRaceForTeam(uint32 team, uint8 classId);

    // Single-combination form of the same engine-data check, used by PbotMgr as a defensive guard
    // before handing race+class to Player::Create.
    bool IsValidCombo(uint8 raceId, uint8 classId);

    // --- Name pool ---

    // Picks a plausible player-style name that (a) is not already taken by any existing
    // character (checked via sCharacterCache) and (b) passes ObjectMgr::CheckPlayerName for
    // this realm's strictness/DB2 rules. Returns an empty string only if the pool and its
    // single-letter-suffix fallbacks are all exhausted or rejected — caller must treat an
    // empty result as a spawn failure.
    std::string PickName();
}
