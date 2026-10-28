// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Proximity voice chat (campaign co-op), the same model as Wasteland Wars' nearby voice without its radio: push-to-talk,
// heard by players around you, quieter with distance and from the side they're on, only in the same space (the same
// interior, or outdoors in the same worldspace). The mic opens only while the talk key is held (and a few seconds after),
// 16 kHz mono, Opus ~16 kbit/s. Settings in [Voice] of FathersFallout.ini, F1 > Voice chat.
namespace F4MP::Voice
{
    void Init(const std::string& a_iniPath);   // once, at plugin load: settings, the mic thread
    void OnPacket(const std::vector<uint8_t>& a_payload);   // main thread: someone's voice (CoopVoice)
    void Frame();                              // main thread, every frame: where I am, can I talk now
    void RenderMenu();                         // the F1 section
    void RenderHud();                          // "TALKING" and who's speaking, on the HUD
    void Bridge(const std::string& a_what);    // test: f4mp:voice [hear]
}
