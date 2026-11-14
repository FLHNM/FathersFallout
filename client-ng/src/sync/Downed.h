// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include "network/NetworkClient.h"

// Down, not dead (Update 29). While playing together the blow that would kill you puts you DOWN instead: 1 health,
// can't fight, and a friend who crouches beside you for 3 seconds gets you back up. A window offers the way out:
//   campaign - RESPAWN AT HOST (guests) or LOAD LAST SAVE (the game's own death);
//   Wasteland Wars - GIVE UP: the Wars death (loot bag, respawn at your bed / place / a start).
// Nobody comes in time: the same happens by itself. In Wars only your crew (or faction) can revive you, and only
// when one of them is online; alone you die at once, as before.
namespace F4MP::Downed
{
    void Install();                         // kGameDataReady: the window (F4SE Menu Framework)
    void Tick();                            // main thread, ~10/s (Coop::Tick)
    bool HoldsDeath();                      // the player's health should stop at 1 now (campaign co-op, setting on)
    bool Revivable();                       // Wars: a crewmate is online - go down instead of dying at once
    bool IsDown();                          // we are down right now
    void OnDowned(const Network::CoopDownedMsg& a_msg);
    void OnRevive(const Network::CoopReviveMsg& a_msg);
    void SetGoToHost(bool (*a_fn)(bool));   // main.cpp's Teleport to host
    void Reset();                           // disconnected / a game loaded
    void RenderMenu();                      // F1 menu: the same choices while down
    void Bridge(const std::string& a_what); // tests: f4mp:down (go down), f4mp:down revive
}
