// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

// Sync distances follow what the game actually has loaded. Exterior cells are 4096 units
// (~58.5 m); the game keeps uGridsToLoad x uGridsToLoad cells loaded around the player, so
// (uGridsToLoad / 2) cells in every direction are always loaded. Outside that, actors don't run,
// so there is nothing to sync and a body teleported there is unloaded at once.
namespace F4MP::Range
{
    constexpr float kCellSize = 4096.0f;
    constexpr float kNearNpc = 4000.0f;   // NPCs closer than this update at full rate

    inline unsigned GridsToLoad()
    {
        static unsigned grids = 0;
        if (!grids) {
            auto* s = RE::GetINISetting("uGridsToLoad:General");
            grids = s && s->GetType() == RE::Setting::SETTING_TYPE::kUInt ? s->GetUInt() : 5u;
            if (grids < 5) grids = 5;
            REX::INFO("[FathersFallout] uGridsToLoad {} -> sync radius {:.0f} units (~{:.0f} m)", grids,
                (grids / 2) * kCellSize, (grids / 2) * kCellSize * 0.01428f);
        }
        return grids;
    }

    // Radius that is always loaded around the local player.
    inline float Loaded() { return static_cast<float>(GridsToLoad() / 2) * kCellSize; }
    inline float BodySpawn() { return Loaded() * 0.9f; }
    inline float BodyDespawn() { return Loaded(); }
}
