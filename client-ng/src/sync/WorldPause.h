// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

// A shared world must not stop because one player looks at their Pip-Boy. The game's own menus have to pause
// it, so instead a paused game says
// so (PlayerPositionMsg kPosPaused): the others take over its NPCs meanwhile (Authority), the host's clock
// catches up afterwards and a guest's Wait/Sleep moves everyone's clock (CoopSync).
namespace F4MP::WorldPause
{
    void Install();          // after the game data is ready (menu events)
    void OnFrame();          // every frame, paused or not
    bool LocalPaused();      // any thread: our game is stopped in a menu right now (not a loading screen)
    // Real seconds the game stood still in menus since the last call (not loading screens or Wait/Sleep).
    double TakePausedSeconds();
    // A Wait/Sleep just ended here: how many game hours it skipped (0 if none since the last call).
    float TakeWaitedHours();
    // VATS at normal speed while playing together (a_on), the game's own slow-motion otherwise.
    void SetRealTimeVats(bool a_on);
}
