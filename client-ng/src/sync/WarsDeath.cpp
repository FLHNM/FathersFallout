// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Wasteland Wars death, Rust-style. Fallout 4's own death is a game over (the death screen, then a save loads),
// which a world that keeps going can't have. In a Wars world the player's health stops at 1 (the player's
// CheckClampDamageModifier, vtable slot 0x131): the blow that would have killed is the death. (The game's own "down"
// - essential - left the player in a T-pose that no console command fixed, and refused saves: only a last resort.)
// Then: a loot bag (the game's own LootBag container) is placed where they fell and everything they
// carry goes into it (the Pip-Boy comes back), then they get up healed at their crew's captured place (or one of the
// starting spots) with a starting kit. The bag stays where it is: go back for it.

#include "pch.h"
#include "sync/WarsDeath.h"
#include "sync/Wars.h"
#include "sync/WarsClaims.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <format>
#include <unordered_set>

namespace F4MP::Wars::Death
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kLootBag = 0x0000000E;    // LootBag: the game's own bag for a body's things
        constexpr uint32_t kPipBoy = 0x00021B3B;

        std::atomic<bool> g_pending{ false };   // the hook stopped a killing blow

        using Clamp_t = float (*)(RE::Actor*, RE::ActorValueInfo&, float);
        Clamp_t g_origClamp = nullptr;
        constexpr std::size_t kClampSlot = 0x131;

        float ClampHook(RE::Actor* a_self, RE::ActorValueInfo& a_info, float a_delta)
        {
            float d = g_origClamp(a_self, a_info, a_delta);
            if (d < 0.0f && Wars::Live() && a_self == RE::PlayerCharacter::GetSingleton()) {
                auto* avs = RE::ActorValue::GetSingleton();
                if (avs && &a_info == avs->health) {
                    const float cur = a_self->GetActorValue(a_info);
                    if (cur + d < 1.0f) {
                        if (!g_pending) REX::INFO("[Wars] killing blow: {:.1f} damage at {:.1f} health", -d, cur);
                        d = (std::min)(0.0f, 1.0f - cur);
                        g_pending = true;
                    }
                }
            }
            return d;
        }

        enum class Stage { Alive, Bagging, Moving };
        Stage g_stage = Stage::Alive;
        Clock::time_point g_at{};
        RE::NiPoint3 g_fellAt{};
        std::unordered_set<uint32_t> g_before;   // runtime refs near us before the bag was placed
        bool g_test = false;                     // f4mp:wars die

        void Console(const std::string& a_cmd) { RE::Console::ExecuteCommand(a_cmd.c_str()); }

        void Hud(const std::string& a_text)
        {
            REX::INFO("[Wars] notice: {}", a_text);
            RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false, false);
        }

        RE::ActorValueInfo* HealthAV()
        {
            auto* avs = RE::ActorValue::GetSingleton();
            return avs ? avs->health : nullptr;
        }

        template <class F>
        void ForEachRuntimeRefNear(const RE::NiPoint3& a_at, float a_radius, F&& a_fn)
        {
            auto* tes = RE::TES::GetSingleton();
            if (!tes) return;
            tes->ForEachReferenceInRange(a_at, a_radius, [&](RE::TESObjectREFR* r) {
                if (r && r != RE::PlayerCharacter::GetSingleton() && r->GetFormID() >= 0xFF000000u) a_fn(r);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        bool Down(RE::PlayerCharacter* a_pl)
        {
            if (g_test || g_pending) return true;
            const auto life = static_cast<RE::ACTOR_LIFE_STATE>(a_pl->lifeState);
            if (life == RE::ACTOR_LIFE_STATE::kEssentialDown || life == RE::ACTOR_LIFE_STATE::kBleedout) return true;
            auto* hp = HealthAV();
            return hp && a_pl->GetActorValue(*hp) <= 0.0f;
        }

        bool FastTravel(uint32_t a_marker)
        {
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            auto* f = RE::TESForm::GetFormByID(a_marker);
            auto* ref = f ? f->As<RE::TESObjectREFR>() : nullptr;
            if (!vm || !ref) return false;
            return vm->DispatchStaticCall("Game", "FastTravel", nullptr, ref);
        }

        uint32_t RespawnPoint()
        {
            if (const uint32_t place = Claims::MyPlace()) return place;   // our crew's place
            return Wars::RandomStart();
        }
    }

    void Tick()
    {
        if (!Wars::Live()) return;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl || !pl->parentCell) return;
        // The game's death would end the session: in Wars the player goes down instead.
        if (!pl->boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential)) {
            pl->boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
            // The game clears it again about once a second: this ran 3126 times in one session's log.
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; REX::INFO("[Wars] the player is essential (down, not dead)"); }
        }
        const auto now = Clock::now();
        switch (g_stage) {
        case Stage::Alive:
            if (!Down(pl)) return;
            if (auto* hp = HealthAV())
                REX::INFO("[Wars] down: test {} blow {} life state {} health {:.1f}", g_test, g_pending.load(), static_cast<int>(pl->lifeState), pl->GetActorValue(*hp));
            g_test = false;
            g_pending = false;
            g_fellAt = pl->GetPosition();
            g_before.clear();
            ForEachRuntimeRefNear(g_fellAt, 400.0f, [&](RE::TESObjectREFR* r) { g_before.insert(r->GetFormID()); });
            Console(std::format("player.placeatme {:08X} 1", kLootBag));
            g_stage = Stage::Bagging;
            g_at = now;
            REX::INFO("[Wars] down at {:.0f} {:.0f} {:.0f}: loot bag", g_fellAt.x, g_fellAt.y, g_fellAt.z);
            return;
        case Stage::Bagging: {
            // The bag: the new LootBag next to us.
            RE::TESObjectREFR* bag = nullptr;
            ForEachRuntimeRefNear(g_fellAt, 400.0f, [&](RE::TESObjectREFR* r) {
                if (!bag && !g_before.count(r->GetFormID()) && r->GetObjectReference() && r->GetObjectReference()->GetFormID() == kLootBag) bag = r;
            });
            if (!bag && now - g_at < std::chrono::seconds(5)) return;
            if (bag) {
                Console(std::format("player.removeallitems {:08X}", bag->GetFormID()));
                Console(std::format("{:08X}.removeitem {:08X} 1", bag->GetFormID(), kPipBoy));   // the Pip-Boy stays yours
                REX::INFO("[Wars] gear into loot bag {:#x}", bag->GetFormID());
            } else {
                REX::WARN("[Wars] no loot bag appeared: gear kept");
            }
            // Up again, somewhere safe, with a starting kit.
            if (auto* hp = HealthAV()) pl->RestoreActorValue(*hp, 100000.0f);
            if (pl->lifeState != static_cast<std::uint32_t>(RE::ACTOR_LIFE_STATE::kAlive)) {
                Console("player.resurrect");   // the game put us down after all (a fall, a scripted kill)
                REX::WARN("[Wars] the game had put the player down (life state {}): resurrected", static_cast<int>(pl->lifeState));
            }
            const uint32_t at = RespawnPoint();
            // The game's own fast travel, not a moveto: a moveto into Red Rocket hung the loading screen for good
            //.
            if (!FastTravel(at)) Console(std::format("player.moveto {:08X}", at));
            Wars::GiveKit();
            g_stage = Stage::Moving;
            g_at = now;
            Hud(bag ? "You died. Everything you carried is in a bag where you fell - go back for it before someone else does."
                    : "You died.");
            REX::INFO("[Wars] respawned at {:#x}", at);
            return;
        }
        case Stage::Moving:
            if (now - g_at > std::chrono::seconds(5)) { g_stage = Stage::Alive; g_pending = false; }   // no second death while loading in
            return;
        }
    }

    bool Pending() { return g_pending; }

    void Install()
    {
        if (!Wars::Mode() || g_origClamp) return;
        REL::Relocation<std::uintptr_t> vt{ RE::VTABLE::PlayerCharacter[0] };
        auto** vtbl = reinterpret_cast<std::uintptr_t**>(vt.address());
        auto* entry = reinterpret_cast<std::uintptr_t*>(&vtbl[kClampSlot]);
        g_origClamp = reinterpret_cast<Clamp_t>(*entry);
        DWORD old = 0;
        if (!VirtualProtect(entry, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &old)) { g_origClamp = nullptr; return; }
        *entry = reinterpret_cast<std::uintptr_t>(&ClampHook);
        VirtualProtect(entry, sizeof(std::uintptr_t), old, &old);
        REX::INFO("[Wars] player health stops at 1 (vtable {:#x} slot {:#x})", reinterpret_cast<std::uintptr_t>(vtbl), kClampSlot);
    }

    void Bridge(const std::string& a_what)
    {
        if (a_what == "die") {
            g_test = true;
            REX::INFO("[Wars] test: dying on the next tick");
        }
    }
}
