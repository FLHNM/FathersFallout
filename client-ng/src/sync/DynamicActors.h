// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>

namespace RE { class Actor; }

// Runtime-created actors (FormID >= 0xFF000000: random encounters, scripted ambushes, leveled
// spawns created at runtime) have IDs that differ per machine. The game that owns an area
// (sync/Authority) announces each one with a spawnable source record; other games create a local
// copy, map (owner, owner's ID) -> local ID, and the regular NPC sync then drives it. In areas
// another player owns, a game disables its own runtime actors nobody announced, so they don't
// duplicate the owner's. Main thread only.
namespace F4MP::DynamicActors
{
    constexpr uint32_t kRuntimeIdStart = 0xFF000000;

    // Owner: announce a runtime actor we run (throttled per actor internally).
    void HostAnnounce(RE::Actor* actor);

    // Another player announced a runtime actor their game runs.
    void OnAnnounce(uint32_t ownerId, uint32_t ownerRefId, uint32_t spawnFormId, float x, float y, float z, float angleZ);

    // Our local copy of an owner's runtime actor, or nullptr if not spawned (yet).
    RE::Actor* Resolve(uint32_t ownerId, uint32_t ownerRefId);

    // Whether this local runtime actor is our copy of another player's.
    bool IsMirrorCopy(uint32_t localFormId);
    // Our copy of another player's runtime actor -> (that player, their FormID for it).
    bool OwnerOf(uint32_t localFormId, uint32_t& ownerId, uint32_t& ownerRefId);

    // Spawn queue, cleanup and duplicate suppression. ~10/s, every game.
    void ClientTick();

    bool& SuppressLocalSpawns();   // menu toggle (default on)
    uint32_t MappedCount();
    uint32_t SuppressedCount();
    void Reset();
}
