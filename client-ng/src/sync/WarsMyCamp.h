// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include "network/NetworkClient.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Wasteland Wars portable camp (Update 29), a little like Fallout 76's C.A.M.P.: set up a camp kit wherever you stand
// (F1 menu) - a sleeping bag (your respawn bed), a cooking fire, a weapons workbench and a stash footlocker - and pack it
// up again to move it (the stash's contents come back to you). One camp per player. The other players see your camp
// (their copy has no stash). A real build-anywhere settlement needs a plugin with Location records: not possible here.
namespace F4MP::Wars::MyCamp
{
    void Tick();                                          // main thread, ~1/s (Wars only)
    bool Deployed();
    void Deploy();                                        // here, facing where we look (moves an existing camp)
    void PackUp();
    void RenderMenu();                                    // F1 menu (Wars)
    void OnRemote(const Network::WarsPlayerCampMsg& a_m);
    void Reset();                                         // disconnected: others' camps go

    // Co-save (inside the Wars record): our camp's references and place.
    std::vector<uint8_t> Save();
    void Load(const uint8_t* a_data, size_t a_size, const std::function<uint32_t(uint32_t)>& a_resolve);
    void Revert();
    void Bridge(const std::string& a_what);               // f4mp:wars mycamp deploy|pack
}
