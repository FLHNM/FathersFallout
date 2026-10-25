// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <vector>

namespace RE { class Actor; }

// Pip-Boy lights (and power armor headlamps) of other players. A body is an NPC, and the game only
// lights the real player (PlayerCharacter::ShowPipboyLight), so each lit body gets a light reference
// of the same record that follows it every tick.
namespace F4MP::BodyLights
{
    // color: 0xRRGGBB of that player's Pip-Boy (its HUD color, like their own light), 0 = the record's own.
    struct Want { uint32_t playerId; RE::Actor* body; bool on; uint32_t color; };

    // The local Pip-Boy color (Fallout4Prefs [Pipboy] fPipboyEffectColor*) as 0xRRGGBB; never 0.
    uint32_t LocalColor();

    // Main thread, every body sync tick: every body there is now (lights of the others are removed).
    void Sync(const std::vector<Want>& a_bodies);
    void Clear();
}
