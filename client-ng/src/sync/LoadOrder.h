// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Plugin lists across games. A FormID's top byte (or, for .esl files, the FE xxx part) is the plugin's
// place in that game's load order, so the same item has different FormIDs when two players run
// different mods or Creation Club content. Every game sends its plugin names on connect; each receiver
// builds a table from the sender's load-order slots to its own and translates every FormID that
// comes in. Forms from a plugin the receiver doesn't have become 0 (nothing), never a wrong form.
namespace F4MP::LoadOrder
{
    // CoopPluginList payload (after the senderId the server stamps).
    std::vector<uint8_t> BuildMyList(bool isHost);
    void OnList(uint32_t senderId, const uint8_t* data, size_t size);   // main thread

    bool Known(uint32_t senderId);        // that player's list arrived: their FormIDs can be translated
    bool Identical(uint32_t senderId);    // same plugins in the same order: FormIDs are the same
    // Sender's FormID -> ours. Runtime IDs (FF......) and 0 come back unchanged, as does everything
    // from a player whose list hasn't arrived. 0 when the form's plugin isn't in our game.
    // Thread-safe (the network thread translates positions and NPC states).
    uint32_t ToLocal(uint32_t senderId, uint32_t formId);

    // On-screen/log summary of what differs from that player ("" when nothing does).
    std::string Differences(uint32_t senderId);
    size_t Dropped();                      // forms skipped because their plugin is missing here

    void Forget(uint32_t senderId);
    void Reset();
}
