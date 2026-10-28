// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>

namespace RE { class Actor; class TESObjectREFR; }
namespace Network {
    struct CoopSeatMsg;
    struct CoopBuildPlacedMsg;
    struct CoopBuildMovedMsg;
    struct CoopBuildRemovedMsg;
    struct CoopRefActivatedMsg;
    struct CoopPropMovedMsg;
    struct CoopWorldReqMsg;
    struct CoopWorldSnapshotMsg;
    struct CoopRefsHiddenMsg;
    struct CoopWirePlacedMsg;
    struct CoopWireRemovedMsg;
    struct CoopWorkerAssignedMsg;
}

// Settlement building together. Objects built in workshop mode while connected are announced;
// every other game places a copy at the same spot, linked to its own workbench, so anyone can
// move or scrap it and everyone sees that too. Objects are named (builder's id, FormID in the
// builder's game). Builds that already existed (each player's own save) are left alone.
// Also shares opening doors and flipping switches. Main thread only.
namespace F4MP::Workshop
{
    void Tick();   // ~10/s while connected: scans built objects while the workshop menu is open
    void OnPlaced(const Network::CoopBuildPlacedMsg& m);
    void OnMoved(const Network::CoopBuildMovedMsg& m);
    void OnRemoved(const Network::CoopBuildRemovedMsg& m);
    void OnActivated(const Network::CoopRefActivatedMsg& m);
    void OnLocalActivate(RE::TESObjectREFR* target);   // our player activated something
    void OnPropMoved(const Network::CoopPropMovedMsg& m); // someone carries/dropped a loose prop
    void HoldRemoteProps();   // every frame: keep props other players hold in their hands
    void Reset();  // disconnected (shared builds stay linked: they belong to the save)
    // The host's world: a joiner sees the host's settlements (the joiner asks, the host answers).
    void OnWorldReq(const Network::CoopWorldReqMsg& m);
    void OnWorldSnapshot(const Network::CoopWorldSnapshotMsg& m);
    void OnRefsHidden(const Network::CoopRefsHiddenMsg& m);
    void OnGameLoaded();
    // Furniture: our player's seat is sent (FurnitureTick), the sender's body sits on ours.
    void FurnitureTick();
    void OnFurniture(const Network::CoopSeatMsg& m);
    bool BodySeated(uint32_t playerId);   // the body is on furniture: don't move or animate it
    // Furniture named for the network: a shared build (owner key, its FormID there) or a plugin
    // reference (0, FormID). False for furniture only this game has. LocalSeat: back to our FormID (0 = none).
    bool SeatKey(RE::TESObjectREFR* furniture, uint32_t& owner, uint32_t& ref);
    uint32_t LocalSeat(uint32_t owner, uint32_t ref);
    // Any shared build (a chest built at a settlement, ...) by its network name, and back to our FormID (0 = none).
    bool BuildKey(RE::TESObjectREFR* ref, uint32_t& owner, uint32_t& key);
    uint32_t LocalBuild(uint32_t owner, uint32_t key);
    // Hide one of our references while connected (an NPC the host's world doesn't have here).
    void HideRef(RE::TESObjectREFR* ref);   // kPostLoadGame: show what a save made while connected had hidden
    void RegisterSerialization();   // once, at plugin load: build links in the co-save
    // Power wires and settlers' jobs, made in workshop mode (scanned with the builds): the same here as in the
    // builder's game. A wire is made like the workshop does (ends connected, the game's shape copied, rebuilt).
    void OnWirePlaced(const Network::CoopWirePlacedMsg& m);
    void OnWireRemoved(const Network::CoopWireRemovedMsg& m);
    void OnWorkerAssigned(const Network::CoopWorkerAssignedMsg& m);
    // Test (f4mp:ws): a wire between two references, shaped straight between their origins (0 = not made); a job.
    uint32_t Wire(RE::TESObjectREFR* a, RE::TESObjectREFR* b, uint32_t splineBase = 0);
    bool AssignWorker(RE::Actor* npc, RE::TESObjectREFR* workObject);
    // The scripts attached to a reference besides plain ObjectReference ("" = none), comma-separated.
    std::string ScriptsOn(RE::TESObjectREFR* ref);
}
