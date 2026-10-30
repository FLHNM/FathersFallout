// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <string>

// Wasteland Wars death: you keep your character, not your gear. In a Wars world the player's health stops at 1 (the game's
// death would end the session and load a save); going down = everything you carry goes into a loot bag where you
// fell, and you get up at your crew's captured place (or a starting spot) with a fresh starting kit.
namespace F4MP::Wars::Death
{
    void Install();                            // plugin load, Wars mode only: the player's health stops at 1
    void Tick();                               // main thread, ~1/s
    bool Pending();                            // a killing blow was stopped: the death runs on the next tick
    void Bridge(const std::string& a_what);    // test: f4mp:wars die
}
