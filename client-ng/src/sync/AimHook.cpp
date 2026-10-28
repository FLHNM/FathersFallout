// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/AimHook.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace F4MP::AimHook
{
    namespace
    {
        struct Slot
        {
            std::atomic<const RE::Actor*> actor{ nullptr };
            std::atomic<float> heading{ 0.0f };
            std::atomic<float> pitch{ 0.0f };
            std::atomic<float> bend{ 0.0f };   // the spine's current bend (eases toward pitch, 0 with the gun away)
        };
        std::array<Slot, 32> g_slots;
        std::atomic<int> g_logged{ 0 };
        unsigned g_patched = 0;
        std::atomic<bool> g_trace{ false };

        // Projectile::AddInitialVelocity (vtable slot 0xE7) and UpdateImpl (0xCF), per projectile class.
        using AddVel_t = void (*)(RE::Projectile*);
        using Update_t = void (*)(RE::Projectile*, float);
        constexpr std::size_t kAddVelocitySlot = 0xE7;
        constexpr std::size_t kUpdateSlot = 0xCF;
        struct Orig { std::uintptr_t vtbl; AddVel_t fn; Update_t update; };
        std::array<Orig, 8> g_orig{};

        // Trace only: shots to report after their first update (where their ray hit).
        std::mutex g_watchLock;
        std::vector<std::pair<RE::Projectile*, int>> g_watch;   // shot, updates seen

        void UpdateHook(RE::Projectile* a_proj, float a_delta)
        {
            const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_proj);
            for (auto& o : g_orig) if (o.vtbl == vtbl) { o.update(a_proj, a_delta); break; }
            if (!g_trace.load(std::memory_order_relaxed)) return;
            std::lock_guard l(g_watchLock);
            auto it = std::find_if(g_watch.begin(), g_watch.end(), [&](auto& w) { return w.first == a_proj; });
            if (it == g_watch.end()) return;
            const int n = ++it->second;
            if (a_proj->distanceMoved <= 0.0f && n < 3) return;   // the ray may be cast in a later update
            g_watch.erase(it);
            auto shooter = a_proj->shooter.get();
            REX::INFO("[AimHook] shot by {:#x} (update {}): velocity ({:.0f} {:.0f} {:.0f}), ray {:.0f}, {} impact(s)", shooter ? shooter->GetFormID() : 0, n,
                a_proj->velocity.x, a_proj->velocity.y, a_proj->velocity.z, a_proj->distanceMoved, a_proj->impacts.size());
            for (std::uint32_t i = 0; i < a_proj->impacts.size() && i < 3; ++i) {
                const auto& im = a_proj->impacts[i];
                auto col = im.collidee.get();
                REX::INFO("[AimHook]   impact at ({:.0f} {:.0f} {:.0f}) on {:#x} layer {}", im.location.x, im.location.y, im.location.z,
                    col ? col->GetFormID() : 0, static_cast<int>(im.collisionLayer));
            }
        }

        Slot* Find(const RE::TESObjectREFR* a)
        {
            for (auto& s : g_slots) if (a && static_cast<const RE::TESObjectREFR*>(s.actor.load(std::memory_order_acquire)) == a) return &s;
            return nullptr;
        }

        void AddVelocityHook(RE::Projectile* a_proj)
        {
            const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_proj);
            for (auto& o : g_orig) if (o.vtbl == vtbl) { o.fn(a_proj); break; }
            auto shooter = a_proj->shooter.get();
            Slot* s = shooter ? Find(shooter.get()) : nullptr;
            if (s) {
                // A remote player's shot leaves the way its player aimed: heading like the body, up/down
                // like its player - not at whoever the body's AI aims at.
                // The engine builds the velocity from these in the first update and casts the ray then.
                const float h = s->heading.load(std::memory_order_relaxed);
                const float p = s->pitch.load(std::memory_order_relaxed);
                a_proj->movementDirection = RE::NiPoint3{ std::sin(h) * std::cos(p), std::cos(h) * std::cos(p), -std::sin(p) };
                a_proj->data.angle.x = p;
                a_proj->data.angle.z = h;
                a_proj->desiredTarget.reset();
                // Its blast is the owner's, replayed where it really went off (Explosions.cpp). The body's own
                // molotov/grenade/missile blew up a second time where the body aimed.
                a_proj->explosion = nullptr;
            }
            if (g_trace.load(std::memory_order_relaxed) && (s || (shooter && shooter.get() == RE::PlayerCharacter::GetSingleton())) &&
                g_logged.fetch_add(1, std::memory_order_relaxed) < 400) {
                std::lock_guard l(g_watchLock);
                if (g_watch.size() > 64) g_watch.erase(g_watch.begin());   // never followed up: drop
                g_watch.push_back({ a_proj, 0 });
            }
        }

        // ---- The body's upper half bent like its player looks up/down (the gun goes with the arms) ----
        // The AI aims the body's gun at its own target whatever the graph or look-at says,
        // so the spine bones are turned after the animation pose, every frame.
        using UpdAnim_t = void (*)(RE::TESObjectREFR*, float);
        constexpr std::size_t kUpdateAnimationSlot = 0x9F;   // TESObjectREFR::UpdateAnimation
        UpdAnim_t g_origUpdAnim = nullptr;
        std::atomic<int> g_spineMode{ 2 };          // 0 off, 1 column vectors (W = P*L), 2 row vectors
        std::atomic<float> g_spineScale{ 1.0f };
        std::atomic<int> g_spineLogged{ 0 };

        // Rotation by a_angle around the unit axis a (column-vector convention).
        RE::NiMatrix3 AxisAngle(const RE::NiPoint3& a, float a_angle)
        {
            const float c = std::cos(a_angle), s = std::sin(a_angle), t = 1.0f - c;
            RE::NiMatrix3 m;
            m.entry[0] = { t * a.x * a.x + c, t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y, 0.0f };
            m.entry[1] = { t * a.x * a.y + s * a.z, t * a.y * a.y + c, t * a.y * a.z - s * a.x, 0.0f };
            m.entry[2] = { t * a.x * a.z - s * a.y, t * a.y * a.z + s * a.x, t * a.z * a.z + c, 0.0f };
            return m;
        }

        // Turn a bone by R (a world-space rotation), keeping its parent where it is.
        void Bend(RE::NiAVObject* a_node, const RE::NiMatrix3& R, int a_mode)
        {
            if (!a_node || !a_node->parent) return;
            const RE::NiMatrix3& P = a_node->parent->world.rotate;
            if (a_mode == 1) a_node->local.rotate = P.Transpose() * R * P * a_node->local.rotate;
            else a_node->local.rotate = a_node->local.rotate * P * R.Transpose() * P.Transpose();
        }

        std::atomic<std::uint32_t> g_deltaProbe{ 0 };   // test aid: log this ref's animation time steps

        void UpdateAnimationHook(RE::TESObjectREFR* a_ref, float a_delta)
        {
            if (const auto probe = g_deltaProbe.load(std::memory_order_relaxed); probe && a_ref && a_ref->GetFormID() == probe) {
                static std::atomic<int> s_n{ 0 };
                if (s_n.fetch_add(1, std::memory_order_relaxed) % 30 == 0) REX::INFO("[AimHook] {:#x} animation step {:.4f} s", probe, a_delta);
            }
            g_origUpdAnim(a_ref, a_delta);
            const int mode = g_spineMode.load(std::memory_order_relaxed);
            if (!mode) return;
            Slot* s = Find(a_ref);
            if (!s) return;
            auto* root = a_ref->Get3D();
            if (!root) return;
            static const RE::BSFixedString kBones[] = { "SPINE1", "SPINE2", "Chest" };
            static constexpr float kShare[] = { 0.3f, 0.3f, 0.4f };
            // Only with the weapon out: looking at the ground while walking must not bow the body.
            auto* actor = a_ref->As<RE::Actor>();
            const bool drawn = actor && actor->GetWeaponMagicDrawn();
            const float target = drawn ? s->pitch.load(std::memory_order_relaxed) * g_spineScale.load(std::memory_order_relaxed) : 0.0f;
            const float k = std::clamp(a_delta * 12.0f, 0.0f, 1.0f);   // eases in ~0.1 s: no snap per network update
            const float p = s->bend.load(std::memory_order_relaxed) + (target - s->bend.load(std::memory_order_relaxed)) * k;
            s->bend.store(p, std::memory_order_relaxed);
            if (std::abs(p) < 0.002f) return;
            // The model's own facing: right = forward x up.
            // NiMatrix3 here maps row vectors (testing: a body facing +X has row 1 = (1 0 0)), so forward is row 1.
            const RE::NiPoint3 fwd{ root->world.rotate.entry[1].x, root->world.rotate.entry[1].y, 0.0f };
            const float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y);
            if (len < 0.01f) return;
            const RE::NiPoint3 right{ fwd.y / len, -fwd.x / len, 0.0f };
            RE::NiAVObject* first = nullptr;
            int found = 0;
            for (int i = 0; i < 3; ++i) {
                auto* bone = root->GetObjectByName(kBones[i]);
                if (!bone) continue;
                found++;
                if (!first) first = bone;
                Bend(bone, AxisAngle(right, -p * kShare[i]), mode);   // + pitch = down: forward tips down
            }
            if (first) {
                RE::NiUpdateData ud{};
                first->UpdateDownwardPass(ud, 0);   // arms, hands and the gun follow this frame
            }
            if (g_spineLogged.fetch_add(1, std::memory_order_relaxed) < 2) {
                const auto& R = root->world.rotate;
                REX::INFO("[AimHook] body {:#x}: {} spine bones bent by {:.2f} rad (mode {}) | heading {:.2f} | root rows ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f}) ({:.2f} {:.2f} {:.2f})",
                    a_ref->GetFormID(), found, p, mode, a_ref->data.angle.z, R.entry[0].x, R.entry[0].y, R.entry[0].z, R.entry[1].x, R.entry[1].y,
                    R.entry[1].z, R.entry[2].x, R.entry[2].y, R.entry[2].z);
            }
        }

        bool PatchSlot(std::uintptr_t* a_entry, std::uintptr_t a_fn)
        {
            DWORD old = 0;
            if (!VirtualProtect(a_entry, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &old)) return false;
            *a_entry = a_fn;
            VirtualProtect(a_entry, sizeof(std::uintptr_t), old, &old);
            return true;
        }

        void PatchClass(const char* a_name, REL::ID a_vtableId)
        {
            REL::Relocation<std::uintptr_t> vt{ a_vtableId };
            auto** vtbl = reinterpret_cast<std::uintptr_t**>(vt.address());
            for (auto& o : g_orig) {
                if (o.vtbl == reinterpret_cast<std::uintptr_t>(vtbl)) return;   // already
                if (o.vtbl) continue;
                // Originals first: another thread may call through the slots the moment they are patched.
                o.fn = reinterpret_cast<AddVel_t>(vtbl[kAddVelocitySlot]);
                o.update = reinterpret_cast<Update_t>(vtbl[kUpdateSlot]);
                o.vtbl = reinterpret_cast<std::uintptr_t>(vtbl);
                if (!PatchSlot(reinterpret_cast<std::uintptr_t*>(&vtbl[kUpdateSlot]), reinterpret_cast<std::uintptr_t>(&UpdateHook)) ||
                    !PatchSlot(reinterpret_cast<std::uintptr_t*>(&vtbl[kAddVelocitySlot]), reinterpret_cast<std::uintptr_t>(&AddVelocityHook))) {
                    REX::WARN("[AimHook] {}: could not patch", a_name);
                    return;
                }
                g_patched++;
                REX::INFO("[AimHook] {}: bodies' shots aimed like their players (vtable {:#x})", a_name, reinterpret_cast<std::uintptr_t>(vtbl));
                return;
            }
        }
    }

    void Install()
    {
        if (g_patched) return;
        PatchClass("MissileProjectile", RE::VTABLE::MissileProjectile[0]);
        PatchClass("BeamProjectile", RE::VTABLE::BeamProjectile[0]);
        PatchClass("ArrowProjectile", RE::VTABLE::ArrowProjectile[0]);
        PatchClass("GrenadeProjectile", RE::VTABLE::GrenadeProjectile[0]);
        PatchClass("FlameProjectile", RE::VTABLE::FlameProjectile[0]);
        PatchClass("ConeProjectile", RE::VTABLE::ConeProjectile[0]);
    }

    void InstallSpine(RE::Actor* a_anyBody)
    {
        if (g_origUpdAnim || !a_anyBody) return;
        // A body's own vtable (Character; the local player has another one, untouched).
        auto** vtbl = *reinterpret_cast<std::uintptr_t***>(a_anyBody);
        g_origUpdAnim = reinterpret_cast<UpdAnim_t>(vtbl[kUpdateAnimationSlot]);
        if (PatchSlot(reinterpret_cast<std::uintptr_t*>(&vtbl[kUpdateAnimationSlot]), reinterpret_cast<std::uintptr_t>(&UpdateAnimationHook)))
            REX::INFO("[AimHook] body animation update hooked (vtable {:#x}): bodies bend with their player's look pitch",
                reinterpret_cast<std::uintptr_t>(vtbl));
    }

    void SetTrace(bool a_on) { g_trace.store(a_on, std::memory_order_relaxed); }
    void ProbeDelta(std::uint32_t a_formId) { g_deltaProbe.store(a_formId, std::memory_order_relaxed); }

    void SetSpine(int a_mode, float a_scale)
    {
        g_spineMode.store(a_mode, std::memory_order_relaxed);
        g_spineScale.store(a_scale, std::memory_order_relaxed);
        g_spineLogged.store(0, std::memory_order_relaxed);
    }

    void SetBodyAim(RE::Actor* a_body, float a_heading, float a_pitch)
    {
        if (!a_body) return;
        Slot* s = Find(a_body);
        if (!s) {
            for (auto& f : g_slots) {
                const RE::Actor* expected = nullptr;
                if (f.actor.compare_exchange_strong(expected, a_body)) { s = &f; break; }
            }
        }
        if (!s) return;
        s->heading.store(a_heading, std::memory_order_relaxed);
        s->pitch.store(a_pitch, std::memory_order_relaxed);
    }

    void Forget(RE::Actor* a_body)
    {
        if (Slot* s = Find(a_body)) s->actor.store(nullptr, std::memory_order_release);
    }

    unsigned PatchedClasses() { return g_patched; }
}
