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
#include "sync/Downed.h"
#include "sync/Activity.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
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
            // Update 29: campaign co-op too, while down-not-dead is on and someone else is playing (sync/Downed).
            if (d < 0.0f && a_self == RE::PlayerCharacter::GetSingleton() && (Wars::Live() || Downed::HoldsDeath())) {
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
        bool g_bagRetried = false;               // this death's bag was placed a second time

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

        bool IsBed(RE::TESObjectREFR* a_ref)
        {
            auto* base = a_ref ? a_ref->GetObjectReference() : nullptr;
            const char* name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
            if (!base || !base->As<RE::TESFurniture>() || !name) return false;
            return std::strstr(name, "Bed") || std::strstr(name, "bed") || std::strstr(name, "Mattress") ||
                   std::strstr(name, "Sleeping") || std::strstr(name, "Cot");
        }

        // Update 29: lying down in a bed makes it your respawn point (players got up at their claim or a random
        // start whatever bed they had slept in - "always Abernathy Farm").
        void WatchBed(RE::PlayerCharacter* a_pl)
        {
            auto* seat = Activity::SeatOf(a_pl);
            if (!IsBed(seat) || seat->GetFormID() == Wars::Bed()) return;
            Wars::SetBed(seat->GetFormID());
            Hud("This bed is your respawn point now.");
            REX::INFO("[Wars] respawn bed {:#x}", seat->GetFormID());
        }

        // A far respawn bed: after the fast travel to the nearest start, a short step to the bed's spot, then onto it.
        struct Chase { uint32_t ref = 0; RE::NiPoint3 at{}; Clock::time_point since{}; bool stepped = false; };
        Chase g_chase;

        void ChaseBed(RE::PlayerCharacter* a_pl, Clock::time_point a_now)
        {
            if (!g_chase.ref) return;
            if (a_now - g_chase.since > std::chrono::seconds(90)) {
                REX::WARN("[Wars] respawn bed {:#x}: didn't get there", g_chase.ref);
                g_chase = {};
                return;
            }
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->GetMenuOpen("LoadingMenu")) return;
            if (a_now - g_chase.since < std::chrono::seconds(3)) return;   // the fast travel's load first
            auto* f = RE::TESForm::GetFormByID(g_chase.ref);
            auto* bed = f ? f->As<RE::TESObjectREFR>() : nullptr;
            if (bed && bed->Get3D()) {
                Console(std::format("player.moveto {:08X}", g_chase.ref));
                REX::INFO("[Wars] respawned at bed {:#x}", g_chase.ref);
                Hud("You got up at your bed.");
                g_chase = {};
                return;
            }
            if (g_chase.stepped) return;
            const auto me = a_pl->GetPosition();
            const float dx = g_chase.at.x - me.x, dy = g_chase.at.y - me.y;
            // A setpos across the map hangs the game: only a short step from the nearest start.
            if (dx * dx + dy * dy > 16000.0f * 16000.0f) {
                REX::WARN("[Wars] respawn bed {:#x} is {:.0f} u from the nearest start: staying there", g_chase.ref, std::sqrt(dx * dx + dy * dy));
                Hud("Your bed is too far from any start point: you got up nearby instead.");
                g_chase = {};
                return;
            }
            g_chase.stepped = true;
            Console(std::format("player.setpos x {:.1f}", g_chase.at.x));
            Console(std::format("player.setpos y {:.1f}", g_chase.at.y));
            Console(std::format("player.setpos z {:.1f}", g_chase.at.z + 60.0f));
        }

        // Our loot bag, until we pick it up or it's long gone: where to find it (players never found the small sack).
        struct Bag { uint32_t ref = 0; RE::NiPoint3 at{}; uint32_t space = 0; Clock::time_point since{}, nextHint{}; };
        Bag g_bag;

        uint32_t SpaceOf(RE::TESObjectREFR* a_ref)
        {
            auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
            if (!cell) return 0;
            if (cell->IsInterior()) return cell->GetFormID();
            return cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
        }

        uint32_t ItemsIn(RE::TESObjectREFR* a_ref)
        {
            auto* inv = a_ref ? a_ref->inventoryList : nullptr;
            if (!inv) return 0;
            uint32_t n = 0;
            for (auto& item : inv->data)
                for (auto* st = item.stackData.get(); st; st = st->nextStack.get()) n += st->count;
            return n;
        }

        void GuideToBag(RE::PlayerCharacter* a_pl)
        {
            if (!g_bag.ref) return;
            const auto now = Clock::now();
            auto* f = RE::TESForm::GetFormByID(g_bag.ref);
            auto* bag = f ? f->As<RE::TESObjectREFR>() : nullptr;
            // Emptied (picked up), gone, or an hour later: done.
            const bool emptied = bag && bag->Get3D() && ItemsIn(bag) == 0;
            if (emptied || now - g_bag.since > std::chrono::hours(1)) {
                if (emptied) REX::INFO("[Wars] loot bag {:#x} emptied", g_bag.ref);
                g_bag = {};
                return;
            }
            if (now < g_bag.nextHint) return;
            g_bag.nextHint = now + std::chrono::seconds(20);
            if (SpaceOf(a_pl) != g_bag.space) return;   // another worldspace / interior: no sensible direction
            const auto me = a_pl->GetPosition();
            const float dx = g_bag.at.x - me.x, dy = g_bag.at.y - me.y;
            const float meters = std::sqrt(dx * dx + dy * dy) * 0.0142875f;
            if (meters < 4.0f) return;
            static const char* kDirs[] = { "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west" };
            float deg = std::atan2(dx, dy) * 57.29578f;   // 0 = north (+y), 90 = east (+x)
            if (deg < 0.0f) deg += 360.0f;
            Hud(std::format("Your loot bag: {:.0f} m {}", meters, kDirs[static_cast<int>((deg + 22.5f) / 45.0f) % 8]));
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
        if (g_stage == Stage::Alive) {
            WatchBed(pl);
            GuideToBag(pl);
        }
        switch (g_stage) {
        case Stage::Alive:
            if (!Down(pl)) return;
            // A crewmate is online: down first, they can revive us (sync/Downed); it calls DieNow if nobody comes.
            if (!g_test && g_pending && (Downed::IsDown() || Downed::Revivable())) return;
            if (auto* hp = HealthAV())
                REX::INFO("[Wars] down: test {} blow {} life state {} health {:.1f}", g_test, g_pending.load(), static_cast<int>(pl->lifeState), pl->GetActorValue(*hp));
            g_test = false;
            g_pending = false;
            g_fellAt = pl->GetPosition();
            g_before.clear();
            ForEachRuntimeRefNear(g_fellAt, 400.0f, [&](RE::TESObjectREFR* r) { g_before.insert(r->GetFormID()); });
            Console(std::format("player.placeatme {:08X} 1", kLootBag));
            g_bagRetried = false;
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
            if (!bag && now - g_at < std::chrono::seconds(5)) {
                // Update 29: one more try half way (a bag that never came lost nothing, but nothing was in it either).
                if (now - g_at > std::chrono::milliseconds(2500) && !g_bagRetried) {
                    g_bagRetried = true;
                    Console(std::format("player.placeatme {:08X} 1", kLootBag));
                    REX::WARN("[Wars] loot bag didn't appear: placing it again");
                }
                return;
            }
            if (bag) {
                g_bag = { bag->GetFormID(), bag->GetPosition(), SpaceOf(bag), now, now + std::chrono::seconds(15) };
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
            // Our bed first (Update 29), else our crew's place, else a start. A bed is a reference, not a map
            // marker: moveto. Not when it's gone (scrapped, the save can't find it).
            uint32_t at = 0;
            const uint32_t bedId = Wars::Bed();
            auto* bedForm = bedId ? RE::TESForm::GetFormByID(bedId) : nullptr;
            auto* bed = bedForm ? bedForm->As<RE::TESObjectREFR>() : nullptr;
            RE::NiPoint3 bedAt{};
            uint32_t bedSpace = 0;
            const bool bedKnown = Wars::BedPlace(bedAt, bedSpace);
            auto* commonwealth = RE::TESForm::GetFormByID(0x0000003C);   // Commonwealth worldspace
            const bool bedOutside = bedKnown && commonwealth && bedSpace == 0x3C;
            if (bed && !bed->IsDisabled() && !bed->IsDeleted()) {
                at = bedId;
                Console(std::format("player.moveto {:08X}", bedId));
            } else if (!bed && bedOutside) {
                // A far bed: its reference isn't in memory. Fast travel to the nearest start, then step over to
                // the bed's spot and onto it once its cell is loaded (Stage::Moving).
                at = Wars::NearestStart(bedAt);
                if (!at || !FastTravel(at)) Console(std::format("player.moveto {:08X}", at ? at : RespawnPoint()));
                g_chase = { bedId, bedAt, now, false };
                REX::INFO("[Wars] respawn bed {:#x} is far ({:.0f} {:.0f}): via start {:#x}", bedId, bedAt.x, bedAt.y, at);
            } else {
                if (bedId && bed) {   // scrapped / packed up
                    REX::WARN("[Wars] respawn bed {:#x} is gone (disabled {}, deleted {})", bedId, bed->IsDisabled(), bed->IsDeleted());
                    Wars::SetBed(0);
                } else if (bedId) {
                    REX::WARN("[Wars] respawn bed {:#x} isn't loaded and isn't outdoors in the Commonwealth: starting elsewhere", bedId);
                }
                at = RespawnPoint();
                // The game's own fast travel, not a moveto: a moveto into Red Rocket hung the loading screen for good
                //.
                if (!FastTravel(at)) Console(std::format("player.moveto {:08X}", at));
            }
            Wars::GiveKit();
            g_stage = Stage::Moving;
            g_at = now;
            Hud(bag ? std::string("You died. Everything you carried is in a bag where you fell - go back for it before someone else does.") +
                          (at == bedId && bedId ? " You got up at your bed." : "")
                    : std::string("You died."));
            REX::INFO("[Wars] respawned at {:#x}", at);
            return;
        }
        case Stage::Moving:
            ChaseBed(pl, now);
            if (now - g_at > std::chrono::seconds(5) && !g_chase.ref) { g_stage = Stage::Alive; g_pending = false; }   // no second death while loading in
            return;
        }
    }

    bool Pending() { return g_pending; }
    void ClearPending() { g_pending = false; }
    void DieNow() { g_test = true; }

    void Install()
    {
        if (g_origClamp) return;   // Update 29: campaign too (down, not dead); the hook only acts when one of them asks
        REL::Relocation<std::uintptr_t> vt{ RE::VTABLE::PlayerCharacter[0] };
        auto** vtbl = reinterpret_cast<std::uintptr_t**>(vt.address());
        auto* entry = reinterpret_cast<std::uintptr_t*>(&vtbl[kClampSlot]);
        g_origClamp = reinterpret_cast<Clamp_t>(*entry);
        DWORD old = 0;
        if (!VirtualProtect(entry, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &old)) { g_origClamp = nullptr; return; }
        *entry = reinterpret_cast<std::uintptr_t>(&ClampHook);
        VirtualProtect(entry, sizeof(std::uintptr_t), old, &old);
        REX::INFO("[Death] player health can stop at 1 (vtable {:#x} slot {:#x})", reinterpret_cast<std::uintptr_t>(vtbl), kClampSlot);
    }

    void Bridge(const std::string& a_what)
    {
        if (a_what == "die") {
            g_test = true;
            REX::INFO("[Wars] test: dying on the next tick");
        }
    }
}
