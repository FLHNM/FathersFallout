// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>

namespace RE { class Actor; }

// Where a remote player's body shoots. The body's AI aimed its gun at the nearest player whatever
// its player aimed at, so its replayed shots hit the other player
// and never went up or down. Every projectile's initial velocity (Projectile::AddInitialVelocity,
// per projectile class) passes through here: a body's shot is turned to its player's own heading
// and look pitch; everyone else's is left alone. It must happen there, before the projectile's
// first update: a bullet casts its whole ray in that update and never moves afterwards.
namespace F4MP::AimHook
{
    void Install();   // once, at game data ready
    void SetTrace(bool a_on);
    void ProbeDelta(std::uint32_t a_formId);   // test aid: log that actor's animation time steps   // [Debug] AimTrace: log where shots (bodies' and ours) hit
    void InstallSpine(RE::Actor* a_anyBody);   // once, from the first body (its class's vtable)
    void SetSpine(int a_mode, float a_scale);   // bridge f4mp:spine <0 off|1|2> <scale>
    // Main thread, every sync tick: the body's facing (rad, 0 = +Y) and look pitch (rad, + = down).
    void SetBodyAim(RE::Actor* a_body, float a_heading, float a_pitch);
    void Forget(RE::Actor* a_body);
    unsigned PatchedClasses();
}
