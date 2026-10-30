// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <vector>

// Wasteland Wars: the A-Life camps around this player, as the server has them (see WarsCamps.cpp).
namespace F4MP::Wars::Camps
{
    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload);   // WarsCamp / WarsCampGone
    void Tick(bool a_outside);   // main thread, ~1/s
    void Reset();                // save loaded / disconnected: our things go
    void Log();
}
