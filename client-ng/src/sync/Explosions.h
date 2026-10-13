// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

namespace Network { struct CoopExplosionMsg; }

// Grenades, mines, exploding cars: every explosion that goes off near our player is announced and
// the others set off the same explosion (form, place) in their own world, so they see it, hear it
// and take its damage there. Explosions that are a consequence of a replay (the car our replay just
// blew up) are not sent back. Main thread only.
namespace F4MP::Explosions
{
    void Tick();   // ~10/s while connected: looks for new explosions near our player
    void OnReceived(const Network::CoopExplosionMsg& m);
    void Reset();  // disconnected
}
