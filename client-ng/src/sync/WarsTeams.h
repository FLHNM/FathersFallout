// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Wasteland Wars sides, as the server keeps them (see server/src/Wars/Teams.h): solo, a crew or one of the big
// four factions. The choice window (it opens by itself until this player has picked), the F1 section, name tags'
// tag and colour, no hits between crewmates, the crew's radio frequency, and the faction's NPCs on our side.
namespace F4MP::Wars::Teams
{
    void Init();      // plugin load: registers the choice window
    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload);   // WarsTeamInfo / WarsCrew / WarsNotice
    void Tick();      // main thread, ~1/s
    void Reset();     // disconnected / save loaded
    void RenderMenu();   // the F1 section

    // A player's name tag in Wars: "[TAG] " (empty = none) and its colour (ABGR). False = plain (solo / unknown).
    bool TagOf(uint32_t a_player, std::string& a_tag, uint32_t& a_abgr);
    // Their hits on us don't count: same crew, or the same faction.
    bool Friendly(uint32_t a_player);
}
