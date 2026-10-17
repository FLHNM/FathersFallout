// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RE { class Actor; }

// Remote player bodies are moved with SetPosition, so the engine sees no movement and writes
// Speed = 0 into their animation graph every frame: the body slides in its idle pose. This hooks
// the graph's pre-update (on the live Character vtable, found from the first body) and writes the
// remote player's Speed/Direction AFTER the engine's own values, right before the graph samples them.
namespace F4MP::GraphOverride
{
    void Install(RE::Actor* a_anyBody);   // once; needs a live Character to find the vtable
    bool Installed();

    // No "E) Talk" prompt and no activation on bodies (hooks the TESNPC vtable, once).
    void InstallNoTalk(RE::Actor* a_anyBody);

    // The graph manager's bound channels copy game values (movement speed, direction...) into
    // the graph every update, over anything we set. Unbind Speed/Direction on a body (once per
    // actor; logs the channel names it finds).
    void DetachMovementChannels(RE::Actor* a_body);

    // Main thread, every sync tick. Bodies not set here are left to the engine.
    void Set(RE::Actor* a_body, float a_speed, float a_direction);
    // Aim up/down (radians, + = down) written into the body graph's Pitch (its bound channel is
    // unbound like Speed's). Call after Set for the same body.
    void SetPitch(RE::Actor* a_body, float a_pitch);
    void SetPitchScale(float a_scale);
    void EngineAim(float& a_pitch, float& a_heading);   // diagnostics: the engine's own aim values on a body
    void KeepOnly(const std::vector<RE::Actor*>& a_live);   // drop bodies that no longer exist
    void ClearAll();

    // Send a graph event to a body as F4MP (the engine's own stop events to bodies are dropped).
    bool SendEvent(RE::Actor* a_body, const char* a_event);
    unsigned long long BlockedCount();
    // True once after the engine sent the body an event that may have reset its locomotion state.
    bool TakeResync(RE::Actor* a_body);
    // Its player's weapon state: the engine's own draw/holster events against it are dropped.
    void SetWantDrawn(RE::Actor* a_body, bool a_drawn);
    // Our equipment change is about to holster/redraw the body's weapon: let it through for a moment.
    void AllowWeaponSwap(RE::Actor* a_body);

    // NPCs (not bodies): combat and idle events (attacks, reloads, throws, idles) the engine sent them, as
    // (FormID, event). Collected only while capture is on; any thread, drained on the main thread.
    void CaptureNpcEvents(bool a_on);
    std::vector<std::pair<std::uint32_t, std::string>> DrainNpcEvents();
    // Idle animations (smoking, cooking, a dog scratching...) are collected too. An NPC mirrored
    // from another player's game plays only the idles its owner sends, not ones its own AI picks.
    bool IsNpcIdleEvent(const char* a_event);
    void SetMirror(std::uint32_t a_formId, bool a_on);
    // Whether a mirrored NPC's owner has its weapon out: its own AI's draw/holster against that is dropped.
    void SetMirrorDrawn(std::uint32_t a_formId, bool a_drawn);
    void ClearMirrors();
    // Let a mirrored NPC's idle events through for a while (an idle its owner's copy plays: sync/Activity).
    void AllowIdles(std::uint32_t a_formId, int a_ms);
    // Events the engine sends while F4MP itself plays something (an idle) count as ours.
    void BeginOurs();
    void EndOurs();

    // Footsteps: the graph's own Foot... events, seen for player bodies and mirrored NPCs. They tell whether
    // a walk/run really plays (a body gliding with still feet, or walking in place, stayed that way until
    // its player jumped or crouched). Milliseconds on the steady clock; 0 = none yet.
    long long LastStepMs(RE::Actor* a_actor);

    unsigned long long CallCount();      // diagnostics: how often the hook ran / overrode
    unsigned long long OverrideCount();
}
