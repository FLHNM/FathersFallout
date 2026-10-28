// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>

namespace RE { class Actor; class TESObjectREFR; class TESIdleForm; }
namespace Network { struct CoopNpcActivityMsg; struct CoopPowerArmorMsg; }

// What actors are busy with, beyond walking about: furniture (chairs, beds, campfires, workbenches),
// idles (smoking, leaning, eating) and power armor.
// - NPCs: the game that runs one sends its seat and idle (CoopNpcActivity); copies elsewhere are put
//   on the same seat / play the same idle. A copy used to stand on the seat doing nothing (an NPC
//   cooking at a campfire on the host stood through the seat on the guest's screen).
// - Player bodies: put on their player's seat (CoopFurniture) and into a frame of their own when their
//   player wears power armor (CoopPowerArmor). Main thread only.
namespace F4MP::Activity
{
    void InstallHooks();   // at plugin load: idles the game starts (what an NPC plays is sent from them)
    void Tick();   // ~10/s while connected
    void OnNpcActivity(const Network::CoopNpcActivityMsg& m);
    void OnPowerArmor(const Network::CoopPowerArmorMsg& m);
    void Reset();  // disconnected / loading a save

    // A mirrored NPC (local FormID) sits/lies/works on furniture as its owner's copy does: leave its
    // position and walk animation alone.
    bool MirrorSeated(uint32_t a_localFormId);
    // A player's body is in power armor here (its gait/locomotion events are the frame's own).
    bool BodyInPowerArmor(uint32_t a_playerId);

    // Seat an actor at once on a furniture marker (true when it's on it). Unseat: get up at once.
    bool Seat(RE::Actor* a_actor, RE::TESObjectREFR* a_furniture, uint32_t a_marker);
    void Unseat(RE::Actor* a_actor);
    RE::TESObjectREFR* SeatOf(RE::Actor* a_actor, uint32_t* a_marker = nullptr);

    // Bridge test helpers (f4mp:act ...): log what an actor is doing; seat it; play an idle; power armor.
    void Describe(RE::Actor* a_actor);
    bool PlayIdle(RE::Actor* a_actor, RE::TESIdleForm* a_idle);
    bool EnterPowerArmor(RE::Actor* a_actor, uint32_t a_frameBase, const uint32_t* a_pieces, int a_count);
    void ExitPowerArmor(RE::Actor* a_actor);
}
