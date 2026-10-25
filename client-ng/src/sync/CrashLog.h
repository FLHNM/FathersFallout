// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <string>

// When the game crashes, the faulting module + offset and the call stack go to the end of
// FathersFallout.log (so the launcher's SHARE LOG carries them). Chains to any crash handler
// installed before us (Buffout 4 etc.).
namespace F4MP::CrashLog
{
    void Install();
    // Puts our unhandled-exception filter back in front if something replaced it since Install.
    void Reassert();
    // The game's log is rewritten on every start: the session before (the one that crashed, when a
    // player relaunches to send it) is kept as FathersFallout-previous.log. Call before F4SE::Init.
    void KeepPreviousLog();
    std::wstring LogPath();
}
