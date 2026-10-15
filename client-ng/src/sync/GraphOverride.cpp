// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include <chrono>
#include "sync/GraphOverride.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace F4MP::GraphOverride
{
    namespace
    {
        using Holder = RE::IAnimationGraphManagerHolder;
        using PreUpdate_t = void (*)(const Holder*, const RE::BSTSmartPointer<RE::BSAnimationGraphManager>&);
        constexpr std::size_t kPreUpdateSlot = 0x16;   // IAnimationGraphManagerHolder::PreUpdateAnimationGraphManager

        // Lock-free: the hook may run on animation worker threads.
        struct Slot
        {
            std::atomic<const Holder*> holder{ nullptr };
            std::atomic<float> speed{ 0.0f };
            std::atomic<float> direction{ 0.0f };
            std::atomic<float> pitch{ 0.0f };      // radians, + = down
            std::atomic<bool> resync{ false };   // engine changed the graph's state: resend ours
            std::atomic<int> wantDrawn{ -1 };      // its player's weapon: 1 out, 0 away, -1 unknown
            std::atomic<long long> swapUntil{ 0 }; // ms clock: our own equip change may holster/draw
        };

        long long NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        std::array<Slot, 32> g_slots;
        std::atomic<unsigned long long> g_weaponBlocked{ 0 };

        PreUpdate_t g_original = nullptr;
        // Graph Pitch per radian of look pitch ([Bridge] f4mp:pitchscale).
        std::atomic<float> g_pitchScale{ -57.2958f };   // testing: player graph Pitch = -57.3 x angle.x
        std::atomic<float> g_engineAimPitch{ 0.0f }, g_engineAimHeading{ 0.0f };   // what the engine wrote (trace)

        // Graph events sent to bodies: logged (diagnostics); the engine's own move/sprint stops
        // are dropped, since the body's AI stands still while its player is running.
        using Notify_t = bool (*)(Holder*, const RE::BSFixedString&);
        constexpr std::size_t kNotifySlot = 0x01;   // IAnimationGraphManagerHolder::NotifyAnimationGraphImpl
        Notify_t g_originalNotify = nullptr;
        thread_local bool t_ours = false;           // set while F4MP itself sends an event
        std::atomic<unsigned long long> g_blocked{ 0 };
        std::atomic<unsigned long long> g_calls{ 0 };
        std::atomic<unsigned long long> g_overrides{ 0 };

        // Combat events the engine sends to NPCs (not bodies): drained by the NPC sync, which
        // forwards those of NPCs this game runs so their copies attack on the other screens too.
        std::atomic<bool> g_captureNpc{ false };
        std::mutex g_npcLock;
        std::vector<std::pair<std::uint32_t, std::string>> g_npcEvents;

        bool IsNpcCombatEvent(const char* a_ev)
        {
            static const char* kPrefixes[] = { "attack", "meleeattack", "powerattack", "bash", "reload", "throw", "grenade" };
            for (const char* p : kPrefixes)
                if (_strnicmp(a_ev, p, std::strlen(p)) == 0) return true;
            return false;
        }

        // NPCs mirrored from another player's game (their FormIDs): their own AI's idles are dropped.
        std::mutex g_mirrorLock;
        std::unordered_set<std::uint32_t> g_mirrors;
        std::unordered_map<std::uint32_t, bool> g_mirrorDrawn;   // whether its owner's copy has the weapon out
        std::atomic<unsigned long long> g_mirrorWeaponBlocked{ 0 };
        std::atomic<unsigned long long> g_idlesBlocked{ 0 };

        bool IsMirror(std::uint32_t a_fid)
        {
            std::lock_guard l(g_mirrorLock);
            return g_mirrors.contains(a_fid);
        }

        // -1: not a mirror or not known yet; else 1/0 = its owner's copy has the weapon out / away.
        int MirrorWantsDrawn(std::uint32_t a_fid)
        {
            std::lock_guard l(g_mirrorLock);
            auto it = g_mirrorDrawn.find(a_fid);
            return it == g_mirrorDrawn.end() ? -1 : (it->second ? 1 : 0);
        }

        void Hook(const Holder* a_self, const RE::BSTSmartPointer<RE::BSAnimationGraphManager>& a_mgr)
        {
            g_original(a_self, a_mgr);   // the engine's own update first (this is where Speed goes to 0)
            g_calls.fetch_add(1, std::memory_order_relaxed);
            for (auto& s : g_slots) {
                if (s.holder.load(std::memory_order_acquire) != a_self) continue;
                static const RE::BSFixedString kSpeed{ "Speed" };
                static const RE::BSFixedString kSpeedSmoothed{ "SpeedSmoothed" };
                static const RE::BSFixedString kDirection{ "Direction" };
                static const RE::BSFixedString kDirectionDegrees{ "DirectionDegrees" };
                auto* holder = const_cast<Holder*>(a_self);
                const float speed = s.speed.load(std::memory_order_relaxed);
                const float dir = s.direction.load(std::memory_order_relaxed);   // fraction of a turn, 0..1
                holder->SetGraphVariableFloat(kSpeed, speed);
                holder->SetGraphVariableFloat(kSpeedSmoothed, speed);
                holder->SetGraphVariableFloat(kDirection, dir);
                holder->SetGraphVariableFloat(kDirectionDegrees, (dir > 0.5f ? dir - 1.0f : dir) * 360.0f);
                static const RE::BSFixedString kPitch{ "Pitch" };
                const float pitch = s.pitch.load(std::memory_order_relaxed);
                holder->SetGraphVariableFloat(kPitch, pitch * g_pitchScale.load(std::memory_order_relaxed));
                if ((g_calls.load(std::memory_order_relaxed) & 127) == 0) {
                    static const RE::BSFixedString kAimPitch{ "AimPitchCurrent" };
                    static const RE::BSFixedString kAimHeading{ "AimHeadingCurrent" };
                    float ep = -999.0f, eh = -999.0f;   // -999 = no such variable on this graph
                    holder->GetGraphVariableImplFloat(kAimPitch, ep);
                    holder->GetGraphVariableImplFloat(kAimHeading, eh);
                    g_engineAimPitch.store(ep, std::memory_order_relaxed);
                    g_engineAimHeading.store(eh, std::memory_order_relaxed);
                }
                g_overrides.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        const Holder* HolderOf(RE::Actor* a_actor) { return a_actor ? static_cast<const Holder*>(a_actor) : nullptr; }

        Slot* SlotOf(const Holder* a_h)
        {
            for (auto& s : g_slots) if (s.holder.load(std::memory_order_acquire) == a_h) return &s;
            return nullptr;
        }

        bool IsBody(const Holder* a_h) { return SlotOf(a_h) != nullptr; }

        bool NotifyHook(Holder* a_self, const RE::BSFixedString& a_event)
        {
            if (Slot* slot = t_ours ? nullptr : SlotOf(a_self)) {
                const char* ev = a_event.c_str();
                {
                    // Per event name, so one noisy event (sneakStop ~30/s) can't use up the log.
                    static std::mutex s_lock;
                    static std::unordered_map<std::string, int> s_count;
                    std::lock_guard l(s_lock);
                    const int n = ++s_count[ev ? ev : "?"];
                    if (n <= 30 || n % 200 == 0) REX::INFO("[GraphOverride] engine event to body: {} (#{})", ev ? ev : "?", n);
                }
                // The body's own AI keeps sending these (sneakStop ~30/s while its player sneaks).
                if (ev && (_stricmp(ev, "moveStop") == 0 || _stricmp(ev, "SprintStop") == 0 ||
                           _stricmp(ev, "sneakStop") == 0 || _stricmp(ev, "sneakStart") == 0)) {
                    g_blocked.fetch_add(1, std::memory_order_relaxed);
                    // Its AI copies the LOCAL player's crouch like a follower would; even dropped, that
                    // left the graph refusing moveStart for ~2 s afterwards while the body was moved
                    // along: it slid. Have the sync resend its own state.
                    if (_strnicmp(ev, "sneak", 5) == 0) slot->resync.store(true, std::memory_order_release);
                    return true;
                }
                // The body's own AI drawing or holstering against its player (in and after fights: the
                // weapon tug-of-war of shots refused while it played). Our own requests
                // match the player's state, so they pass; an equipment swap gets a moment to holster.
                if (ev && NowMs() > slot->swapUntil.load(std::memory_order_relaxed)) {
                    const int want = slot->wantDrawn.load(std::memory_order_relaxed);
                    if ((want == 1 && _stricmp(ev, "Unequip") == 0) || (want == 0 && _stricmp(ev, "WeapEquip") == 0)) {
                        const auto n = g_weaponBlocked.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (n <= 20 || n % 200 == 0) REX::INFO("[GraphOverride] body: its AI's {} dropped (player has the weapon {}) (#{})", ev,
                            want ? "out" : "away", n);
                        return true;
                    }
                }
                // Anything else (RelaxedStateStart, WeapEquip, Unequip, archetype changes...) can
                // drop the graph out of its locomotion state: have the sync resend move/sprint.
                const bool ok = g_originalNotify(a_self, a_event);
                slot->resync.store(true, std::memory_order_release);
                return ok;
            }
            if (!t_ours) {
                // A mirrored NPC idles only as its owner's copy does (CoopNpcAnim): its own AI picked
                // other idles than the owner's.
                const char* ev = a_event.c_str();
                // Its own AI holstering a gun its owner's copy holds out (an idle NPC on a do-nothing package
                // puts its weapon away right after every draw), or drawing one the owner keeps away.
                if (ev) {
                    const int want = MirrorWantsDrawn(static_cast<RE::Actor*>(a_self)->GetFormID());
                    if ((want == 1 && _stricmp(ev, "Unequip") == 0) || (want == 0 && _stricmp(ev, "WeapEquip") == 0)) {
                        const auto n = g_mirrorWeaponBlocked.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (n <= 20 || n % 200 == 0)
                            REX::INFO("[GraphOverride] mirrored NPC {:#x}: its AI's {} dropped (owner has the weapon {}) (#{})",
                                static_cast<RE::Actor*>(a_self)->GetFormID(), ev, want ? "out" : "away", n);
                        return true;
                    }
                }
                if (ev && IsNpcIdleEvent(ev) && IsMirror(static_cast<RE::Actor*>(a_self)->GetFormID())) {
                    const auto n = g_idlesBlocked.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (n <= 20 || n % 500 == 0)
                        REX::INFO("[GraphOverride] mirrored NPC {:#x}: own idle {} dropped (#{})",
                            static_cast<RE::Actor*>(a_self)->GetFormID(), ev, n);
                    return true;
                }
            }
            const bool ok = g_originalNotify(a_self, a_event);
            if (ok && !t_ours && g_captureNpc.load(std::memory_order_relaxed)) {
                const char* ev = a_event.c_str();
                if (ev && (IsNpcCombatEvent(ev) || IsNpcIdleEvent(ev))) {
                    const auto fid = static_cast<RE::Actor*>(a_self)->GetFormID();
                    std::lock_guard l(g_npcLock);
                    if (g_npcEvents.size() < 256) g_npcEvents.emplace_back(fid, ev);
                }
            }
            return ok;
        }

        void Patch(std::uintptr_t** a_vtbl, std::size_t a_slot, std::uintptr_t a_fn, std::uintptr_t& a_orig)
        {
            auto* entry = reinterpret_cast<std::uintptr_t*>(&a_vtbl[a_slot]);
            DWORD old = 0;
            if (!VirtualProtect(entry, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &old)) {
                REX::WARN("[GraphOverride] VirtualProtect failed ({})", GetLastError());
                return;
            }
            a_orig = *entry;
            *entry = a_fn;
            VirtualProtect(entry, sizeof(std::uintptr_t), old, &old);
        }
    }

    namespace
    {
        // Bodies are copies of the player template: the game offers "E) Talk" on them and would
        // start a conversation. The base-object vfuncs below answer "nothing to do" for bodies.
        using GetActivateText_t = bool (*)(RE::TESBoundObject*, RE::TESObjectREFR*, RE::BSString&);
        using Activate_t = bool (*)(RE::TESForm*, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::TESBoundObject*, std::int32_t);
        constexpr std::size_t kGetActivateTextSlot = 0x60;   // TESBoundObject::GetActivateText
        constexpr std::size_t kActivateSlot = 0x40;          // TESForm::Activate
        GetActivateText_t g_origActivateText = nullptr;
        Activate_t g_origActivate = nullptr;

        bool IsBodyRef(const RE::TESObjectREFR* a_ref)
        {
            return a_ref && IsBody(static_cast<const Holder*>(a_ref));
        }

        bool ActivateTextHook(RE::TESBoundObject* a_self, RE::TESObjectREFR* a_ref, RE::BSString& a_out)
        {
            if (IsBodyRef(a_ref)) {
                static std::atomic<bool> s_logged{ false };
                if (!s_logged.exchange(true)) REX::INFO("[GraphOverride] hid the activate prompt on body {:#x}", a_ref->GetFormID());
                return false;   // no prompt
            }
            return g_origActivateText(a_self, a_ref, a_out);
        }

        bool ActivateHook(RE::TESForm* a_self, RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_by, RE::TESBoundObject* a_obj, std::int32_t a_count)
        {
            if (IsBodyRef(a_ref)) return true;    // handled: nothing happens
            return g_origActivate(a_self, a_ref, a_by, a_obj, a_count);
        }
    }

    void InstallNoTalk(RE::Actor* a_body)
    {
        if (g_origActivateText || !a_body) return;
        auto* base = a_body->GetObjectReference();
        if (!base || !base->As<RE::TESNPC>()) return;
        auto** vtbl = *reinterpret_cast<std::uintptr_t***>(base);
        g_origActivateText = reinterpret_cast<GetActivateText_t>(vtbl[kGetActivateTextSlot]);
        g_origActivate = reinterpret_cast<Activate_t>(vtbl[kActivateSlot]);
        std::uintptr_t unused = 0;
        Patch(vtbl, kGetActivateTextSlot, reinterpret_cast<std::uintptr_t>(&ActivateTextHook), unused);
        Patch(vtbl, kActivateSlot, reinterpret_cast<std::uintptr_t>(&ActivateHook), unused);
        REX::INFO("[GraphOverride] bodies: no Talk prompt / activation (TESNPC vtable {:#x})", reinterpret_cast<std::uintptr_t>(vtbl));
    }

    bool SendEvent(RE::Actor* a_body, const char* a_event)
    {
        t_ours = true;
        const bool ok = a_body && a_body->NotifyAnimationGraphImpl(a_event);
        t_ours = false;
        return ok;
    }

    unsigned long long BlockedCount() { return g_blocked.load(); }

    void SetWantDrawn(RE::Actor* a_body, bool a_drawn)
    {
        if (Slot* s = SlotOf(HolderOf(a_body))) s->wantDrawn.store(a_drawn ? 1 : 0, std::memory_order_relaxed);
    }

    void AllowWeaponSwap(RE::Actor* a_body)
    {
        if (Slot* s = SlotOf(HolderOf(a_body))) s->swapUntil.store(NowMs() + 1500, std::memory_order_relaxed);
    }

    bool TakeResync(RE::Actor* a_body)
    {
        Slot* slot = SlotOf(HolderOf(a_body));
        return slot && slot->resync.exchange(false, std::memory_order_acq_rel);
    }

    void Install(RE::Actor* a_anyBody)
    {
        if (g_original || !a_anyBody) return;
        auto** vtbl = *reinterpret_cast<std::uintptr_t***>(const_cast<Holder*>(HolderOf(a_anyBody)));
        // Originals first: the hooks may run on another thread the moment a slot is patched.
        g_original = reinterpret_cast<PreUpdate_t>(vtbl[kPreUpdateSlot]);
        g_originalNotify = reinterpret_cast<Notify_t>(vtbl[kNotifySlot]);
        std::uintptr_t unused = 0;
        Patch(vtbl, kPreUpdateSlot, reinterpret_cast<std::uintptr_t>(&Hook), unused);
        Patch(vtbl, kNotifySlot, reinterpret_cast<std::uintptr_t>(&NotifyHook), unused);
        REX::INFO("[GraphOverride] hooked Character graph pre-update + events (vtable {:#x})",
            reinterpret_cast<std::uintptr_t>(vtbl));
    }

    bool Installed() { return g_original != nullptr; }

    namespace
    {
        // BSTArray<T> layout: data 0x00, capacity 0x08, size 0x10.
        struct RawArray
        {
            void**        data;
            std::uint32_t capacity;
            std::uint32_t pad;
            std::uint32_t size;
        };
        constexpr std::size_t kBoundChannelOffset = 0x10;   // BSAnimationGraphManager::boundChannel
        constexpr std::size_t kChannelNameOffset = 0x10;    // after vtable + refcount (as in Skyrim)

        // Channel name, or nullptr if the layout guess is wrong (guarded: never crash on it).
        const char* ChannelName(void* a_channel)
        {
            __try {
                auto* fs = reinterpret_cast<const RE::BSFixedString*>(static_cast<char*>(a_channel) + kChannelNameOffset);
                const char* str = fs->c_str();   // BSFixedString -> string pool entry -> characters
                if (!str) return nullptr;
                volatile char probe = str[0];
                (void)probe;
                return str;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        std::vector<void*> g_detached;   // graph managers whose movement channels are unbound
    }

    void DetachMovementChannels(RE::Actor* a_body)
    {
        if (!a_body) return;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
        if (!a_body->GetAnimationGraphManagerImpl(mgr) || !mgr) return;   // graph not loaded yet
        // Per graph manager, not per actor: a 3D reset (weapon swap, cell change) builds a new
        // manager with its channels bound again.
        void* key = mgr.get();
        for (auto* m : g_detached) if (m == key) return;
        g_detached.push_back(key);
        if (g_detached.size() > 64) g_detached.erase(g_detached.begin());

        auto* arr = reinterpret_cast<RawArray*>(reinterpret_cast<char*>(mgr.get()) + kBoundChannelOffset);
        if (!arr->data || arr->size > 256) {
            REX::WARN("[GraphOverride] bound channels look wrong (size {}), not touching them", arr->size);
            return;
        }
        std::string names;
        std::uint32_t kept = 0, removed = 0;
        for (std::uint32_t i = 0; i < arr->size; ++i) {
            void* ch = arr->data[i];
            const char* name = ch ? ChannelName(ch) : nullptr;
            names += name ? name : "?";
            names += ' ';
            const bool drop = name && (_stricmp(name, "Speed") == 0 || _stricmp(name, "Direction") == 0 ||
                                       _stricmp(name, "SpeedSmoothed") == 0 || _stricmp(name, "DirectionDegrees") == 0 ||
                                       _stricmp(name, "Pitch") == 0);
            if (drop) { removed++; continue; }   // its reference is leaked on purpose (tiny, once per body)
            arr->data[kept++] = ch;
        }
        arr->size = kept;
        REX::INFO("[GraphOverride] body {:#x}: bound channels [{}] -> unbound {}", a_body->GetFormID(), names, removed);
    }

    void Set(RE::Actor* a_body, float a_speed, float a_direction)
    {
        const Holder* h = HolderOf(a_body);
        if (!h) return;
        Slot* free = nullptr;
        for (auto& s : g_slots) {
            const Holder* cur = s.holder.load(std::memory_order_relaxed);
            if (cur == h) { free = &s; break; }
            if (!cur && !free) free = &s;
        }
        if (!free) return;
        free->speed.store(a_speed, std::memory_order_relaxed);
        free->direction.store(a_direction, std::memory_order_relaxed);
        free->holder.store(h, std::memory_order_release);
    }

    void SetPitch(RE::Actor* a_body, float a_pitch)
    {
        if (Slot* s = SlotOf(HolderOf(a_body))) s->pitch.store(a_pitch, std::memory_order_relaxed);
    }

    void SetPitchScale(float a_scale) { g_pitchScale = a_scale; }
    void EngineAim(float& a_pitch, float& a_heading)
    {
        a_pitch = g_engineAimPitch.load(std::memory_order_relaxed);
        a_heading = g_engineAimHeading.load(std::memory_order_relaxed);
    }

    void KeepOnly(const std::vector<RE::Actor*>& a_live)
    {
        for (auto& s : g_slots) {
            const Holder* cur = s.holder.load(std::memory_order_relaxed);
            if (!cur) continue;
            bool live = false;
            for (auto* a : a_live) live = live || HolderOf(a) == cur;
            if (!live) s.holder.store(nullptr, std::memory_order_release);
        }
    }

    void ClearAll()
    {
        g_detached.clear();
        for (auto& s : g_slots) s.holder.store(nullptr, std::memory_order_release);
    }

    void CaptureNpcEvents(bool a_on)
    {
        g_captureNpc = a_on;
        if (!a_on) { std::lock_guard l(g_npcLock); g_npcEvents.clear(); }
    }

    std::vector<std::pair<std::uint32_t, std::string>> DrainNpcEvents()
    {
        std::lock_guard l(g_npcLock);
        std::vector<std::pair<std::uint32_t, std::string>> out;
        out.swap(g_npcEvents);
        return out;
    }

    bool IsNpcIdleEvent(const char* a_ev)
    {
        // Idle forms play through events named idle... (IdleSmoking, idleChairSitting, IdleStop...).
        return a_ev && _strnicmp(a_ev, "idle", 4) == 0;
    }

    void SetMirror(std::uint32_t a_formId, bool a_on)
    {
        std::lock_guard l(g_mirrorLock);
        if (a_on) g_mirrors.insert(a_formId);
        else { g_mirrors.erase(a_formId); g_mirrorDrawn.erase(a_formId); }
    }

    void SetMirrorDrawn(std::uint32_t a_formId, bool a_drawn)
    {
        std::lock_guard l(g_mirrorLock);
        if (g_mirrors.contains(a_formId)) g_mirrorDrawn[a_formId] = a_drawn;
    }

    void ClearMirrors()
    {
        std::lock_guard l(g_mirrorLock);
        g_mirrors.clear();
        g_mirrorDrawn.clear();
    }

    unsigned long long CallCount() { return g_calls.load(); }
    unsigned long long OverrideCount() { return g_overrides.load(); }
}
