// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Wasteland Wars A-Life squads the server hands to this game (sync/WarsSquads.cpp).
namespace F4MP::Wars::Squads
{
    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload);   // WarsSquadOnline / Offline / Goal
    void Tick();      // main thread, ~1/s: presence, spawns, reports
    void Reset();     // save loaded / disconnected
    size_t Count();
    void Log();   // bridge f4mp:wars squads
    void AiProbe();
    void SetJog(bool a_on);   // bridge f4mp:wars jog 0|1: squads on the move jog (default) or walk
}
