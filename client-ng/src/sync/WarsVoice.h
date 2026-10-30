// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Wasteland Wars voice: push-to-talk, two ways.
// - Nearby (proximity): heard by players around you, quieter with distance and from the side they're on; only in the
//   same space (the same interior, or outdoors in the same worldspace).
// - Radio: on your radio's frequency, heard by everyone tuned to it anywhere in the Commonwealth, with a radio's
//   sound (band-limited, a little crunch, squelch at both ends).
// The radio (F1 > Wasteland Wars) also carries the A-Life chatter, can be switched off, and is tuned 88.0-108.0 MHz.
// The mic opens only while a talk key is held (and a few seconds after), 16 kHz mono, Opus ~16 kbit/s.
namespace F4MP::Wars::Voice
{
    void Init(const std::string& a_iniPath);   // settings ([Voice] in FathersFallout.ini)
    void OnConnected();                        // the radio's settings go to the server
    void OnPacket(const std::vector<uint8_t>& a_payload);   // main thread: someone's voice
    void Frame();                              // main thread, every frame: who/where I am, can I talk now
    void RenderMenu();                         // the F1 section
    void RenderHud();                          // "TALKING" and who's speaking, on the HUD

    // The radio: A-Life chatter is shown only when it's on and chatter is on.
    void Bridge(const std::string& a_what);    // test: f4mp:wars voice [hear | radio | tune <MHz>]
    void RadioLine(const std::string& a_who, const std::string& a_text);   // A-Life chatter heard (main thread)
    bool RadioOn();
    void TuneTo(int a_tenthsMHz);              // the radio to a frequency (a crew's own), saved and sent
    bool ChatterOn();
}
