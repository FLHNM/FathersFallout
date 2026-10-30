// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

namespace Network { struct CoopNpcSayMsg; }

// Conversations everyone sees and hears: when an NPC our game runs speaks a line (talking to our player, a scene,
// a greeting, a bark), the others' copy of that NPC says it too - its own voice, lips, subtitle and head movement
// from their engine (ObjectReference.Say on the line's topic). Lines with a script fragment (stages, items,
// scenes) are not said again there: running them twice would hand out rewards twice. Main thread.
namespace F4MP::Dialogue
{
    void Tick();   // ~10/s while connected: lines our NPCs started saying
    void OnSay(const Network::CoopNpcSayMsg& m);
    void Reset();
}
