// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Animation graph events the game sends to the LOCAL player (attacks, reloads, aiming...).
// Captured by hooking the PlayerCharacter graph-event vfunc; the co-op layer forwards the
// interesting ones so other games can replay them on this player's body.
namespace F4MP::PlayerEvents
{
    void Install();                          // once, after the player exists (kGameDataReady or later)
    std::vector<std::string> Drain();        // events since the last call (main thread)
    void SetLogAll(bool a_on);               // diagnostics: log every event name (BodyTrace)
}
