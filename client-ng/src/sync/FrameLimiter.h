// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

// Hard 60 FPS cap enforced by the plugin on every client. Fallout 4 ties physics and
// parts of game logic to frame time, so a client above 60 FPS runs fast and drifts
// out of sync with everyone else. Called once per rendered frame.
namespace F4MP::FrameLimiter
{
    constexpr int kTargetFps = 60;

    void OnFrame();
    float GetMeasuredFps();
}
