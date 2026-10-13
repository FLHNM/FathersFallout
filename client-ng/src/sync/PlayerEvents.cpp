// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/PlayerEvents.h"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace F4MP::PlayerEvents
{
    namespace
    {
        using Holder = RE::IAnimationGraphManagerHolder;
        using Notify_t = bool (*)(Holder*, const RE::BSFixedString&);
        constexpr std::size_t kNotifySlot = 0x01;   // IAnimationGraphManagerHolder::NotifyAnimationGraphImpl

        Notify_t g_original = nullptr;
        const Holder* g_player = nullptr;
        std::atomic<bool> g_logAll{ false };
        std::mutex g_lock;
        std::vector<std::string> g_pending;

        bool Hook(Holder* a_self, const RE::BSFixedString& a_event)
        {
            const bool ok = g_original(a_self, a_event);
            if (a_self == g_player) {
                const char* ev = a_event.c_str();
                if (ev && *ev) {
                    if (g_logAll.load(std::memory_order_relaxed)) {
                        // Count repeats so per-frame events don't flood the log.
                        static std::unordered_map<std::string, int> s_seen;
                        static std::mutex s_seenLock;
                        std::lock_guard l(s_seenLock);
                        const int n = ++s_seen[ev];
                        if (n <= 3 || n % 100 == 0) REX::INFO("[PlayerEvents] {} -> {} (#{})", ev, ok, n);
                    }
                    std::lock_guard l(g_lock);
                    if (g_pending.size() < 256) g_pending.emplace_back(ev);
                }
            }
            return ok;
        }
    }

    void Install()
    {
        if (g_original) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        g_player = static_cast<const Holder*>(player);
        auto** vtbl = *reinterpret_cast<std::uintptr_t***>(const_cast<Holder*>(g_player));
        auto* entry = reinterpret_cast<std::uintptr_t*>(&vtbl[kNotifySlot]);
        g_original = reinterpret_cast<Notify_t>(*entry);   // before patching: the hook may run at once
        DWORD old = 0;
        if (!VirtualProtect(entry, sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &old)) {
            REX::WARN("[PlayerEvents] VirtualProtect failed ({})", GetLastError());
            g_original = nullptr;
            return;
        }
        *entry = reinterpret_cast<std::uintptr_t>(&Hook);
        VirtualProtect(entry, sizeof(std::uintptr_t), old, &old);
        REX::INFO("[PlayerEvents] hooked PlayerCharacter graph events (vtable {:#x})", reinterpret_cast<std::uintptr_t>(vtbl));
    }

    std::vector<std::string> Drain()
    {
        std::lock_guard l(g_lock);
        std::vector<std::string> out;
        out.swap(g_pending);
        return out;
    }

    void SetLogAll(bool a_on) { g_logAll = a_on; }
}
