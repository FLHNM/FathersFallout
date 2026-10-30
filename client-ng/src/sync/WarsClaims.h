// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <vector>

// Wasteland Wars: captured places. The server tells this player how a capture goes (hold the cleared place's
// settlement workbench, no enemies near) and who holds each workbench; ours is ours to build with, the others'
// (and nobody's) can't be used (see WarsClaims.cpp).
namespace F4MP::Wars::Claims
{
    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload);   // WarsCapture / WarsClaim
    void Tick();     // main thread, ~1/s
    void Reset();    // save loaded / disconnected
    uint32_t MyPlace();   // the map marker of a place we (our crew) hold, 0 = none: where we get up after dying
}
