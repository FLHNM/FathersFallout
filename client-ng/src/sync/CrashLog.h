// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

// When the game crashes, the faulting module + offset and the call stack go to the end of
// FathersFallout.log (so the launcher's SHARE LOG carries them). Chains to any crash handler
// installed before us (Buffout 4 etc.).
namespace F4MP::CrashLog
{
    void Install();
}
