// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace RE { class Actor; }

// Implemented in main.cpp: the remote-player body bookkeeping and the host role.
namespace F4MP
{
    bool IsHost();
    std::vector<std::pair<uint32_t, uint32_t>> GetBodies();  // (playerId, body FormID)
    uint32_t GetPlayerIdForBody(uint32_t formId);           // 0 if not a body
}

// Co-op sync on top of the position/NPC relay: equipment, combat, items and quests.
// Model: one client is the HOST and is authoritative for NPCs, containers and quests.
// Everything is keyed by FormID, so every player must load a copy of the host's save.
// All functions run on the game's main thread (scheduled via the F4SE task interface).
namespace F4MP::Coop
{
    struct Settings
    {
        bool equipment = true;
        bool combat = true;
        bool items = true;
        bool quests = true;
        bool questCatchUp = true;    // joiners: on joining, move my quests up to the host's progress
        bool shareMyQuests = false;  // joiners: ask the host to apply my quest stages (own saves differ)
        bool worldTime = true;   // follow the host's clock and weather
        bool combatAnims = true; // show other players' shots, swings, aiming and reloads
        bool building = true;    // settlement building, doors and switches
        bool friendlyFire = true; // other players' hits hurt me ([Sync] FriendlyFire in F4MP.ini)
        bool hitTrace = false;   // [Debug] BodyTrace: log every hit by/on a player, dump the appearance we send
    };

    struct Stats
    {
        uint32_t sent = 0;
        uint32_t applied = 0;
        uint32_t questStagesApplied = 0;
        uint32_t containerOps = 0;
        uint32_t damageForwarded = 0;
        uint32_t trackedQuests = 0;
    };

    Settings& GetSettings();
    bool IsLoadOrderMismatch();   // client: plugin list differs from the host's (ID-based sync paused)
    size_t MismatchedPlayers();   // host: connected clients with a different plugin list
    const Stats& GetStats();

    void RegisterSinks();   // once, at kGameDataReady
    void Tick();            // ~10/s while connected
    void OnDisconnected();  // forget all per-session state
    void OnGameLoaded();    // kPostLoadGame: hits sent before the load are dropped for a moment

    // Mirror side of NPC sync: copy the owner's health/death onto a mirrored NPC and forward
    // any damage our player dealt to it to the owner. Called from NpcsSync.
    void ApplyNpcVitals(RE::Actor* actor, uint32_t ownerId, uint32_t formId, float ownerHealth, bool ownerDead);

    // A mirrored NPC's weapon was just swapped to its owner's (the model rebuilt holstered while the game
    // still counts it as drawn): true once, when it should be drawn again. Called from NpcsSync.
    bool TakeNpcRedraw(uint32_t localFormId);

    // Another player's health as they last reported it (any thread). False if not heard yet.
    bool GetPlayerVitals(uint32_t playerId, float& health, float& maxHealth, bool& dead);

    // Host side: current Health of an actor, for NpcStateMsg.
    float GetHealth(RE::Actor* actor);

    // A body has the equipment its player last sent (or none was sent yet). Drawing the weapon
    // waits for this: a draw cut short by the weapon's 3D rebuild left the body half-drawn.
    bool EquipmentSettled(uint32_t playerId, uint32_t bodyId);
}
