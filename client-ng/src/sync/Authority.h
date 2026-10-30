// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>

namespace RE { class TESObjectREFR; class NiPoint3; }

// Who simulates what. Every game knows every player's position, "space" (interior cell or
// exterior worldspace) and loaded radius, so each one computes the same owner for any spot with
// no negotiation: the host if the host has that spot loaded, otherwise the lowest-numbered player
// who does. The owner runs that area's NPCs and broadcasts them; everyone else mirrors. This lets
// players be far apart: each game only simulates what it already has loaded.
namespace F4MP::Authority
{
    // Interior cell FormID, or worldspace FormID outdoors; 0 if unknown.
    uint32_t SpaceOf(const RE::TESObjectREFR* ref);
    uint32_t LocalSpace();          // main thread
    uint32_t LocalSpaceAtomic();    // any thread (last value seen on the main thread)

    void SetRemoteRadius(uint32_t playerId, float radius);
    void SetHost(uint32_t playerId);
    uint32_t HostId();

    // Snapshot every player's position/space/radius. Call once per main-thread tick.
    void Refresh();

    uint32_t MyId();
    bool HostAreaKnown();
    bool IsPaused(uint32_t playerId);   // in a pausing menu (or just out of one): runs nothing for now   // we are the host, or the host's position has arrived
    uint32_t OwnerOf(uint32_t space, const RE::NiPoint3& pos);
    // Whether our game runs this reference. Runtime actors (random encounters...) stay with the game
    // that made them wherever the players go: our copies of another player's are never ours.
    bool IOwn(const RE::TESObjectREFR* ref);
    // Who has the spot loaded, ignoring the rule above (sticky per reference to avoid edge flicker).
    bool IOwnArea(const RE::TESObjectREFR* ref);

    void Reset();
}
