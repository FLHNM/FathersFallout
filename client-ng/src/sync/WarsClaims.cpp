// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Wasteland Wars claims. A cleared place (its people wiped out) with a settlement workbench is taken by holding
// that workbench for 30 s with no enemies near; the server keeps who holds what. Here: the countdown on the HUD,
// and each workbench set to what the server says - ours: owned by the player (WorkshopScript.SetOwnedByPlayer),
// usable; someone else's or nobody's: activation blocked, so no one takes a place just by using its workbench (the
// game makes a settlement yours when you do).

#include "pch.h"
#include "sync/WarsClaims.h"
#include "sync/Wars.h"
#include "network/NetworkClient.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace F4MP::Wars::Claims
{
    using namespace Network;

    namespace
    {
        struct Claim {
            bool mine = false;
            std::string owner;
            uint32_t place = 0;   // its map marker
            int applied = -1;     // what we last set on the workbench (-1 nothing, 0 blocked, 1 ours)
        };
        std::mutex g_lock;
        std::unordered_map<uint32_t, Claim> g_claims;   // by workbench reference

        void Hud(const std::string& a_text)
        {
            REX::INFO("[Wars] notice: {}", a_text);
            if (auto* tasks = F4SE::GetTaskInterface())
                tasks->AddTask([t = a_text]() { RE::SendHUDMessage::ShowHUDMessage(t.c_str(), nullptr, false, false); });
        }

        bool Call(RE::TESObjectREFR* a_ref, const char* a_script, const char* a_fn, auto... a_args)
        {
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            if (!vm || !a_ref) return false;
            const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<std::uint32_t>(a_ref->GetFormType()), a_ref);
            if (handle == vm->GetObjectHandlePolicy().EmptyHandle()) return false;
            return vm->DispatchMethodCall(handle, a_script, a_fn, nullptr, a_args...);
        }

        void Apply(uint32_t a_ref, Claim& a_c)
        {
            const int want = a_c.mine ? 1 : 0;
            if (a_c.applied == want) return;
            auto* f = RE::TESForm::GetFormByID(a_ref);
            auto* ref = f ? f->As<RE::TESObjectREFR>() : nullptr;
            if (!ref || !ref->parentCell) return;   // not here (yet)
            bool a, b = true;
            if (a_c.mine) {
                a = Call(ref, "ObjectReference", "BlockActivation", false, false);
                b = Call(ref, "WorkshopScript", "SetOwnedByPlayer", true);
            } else {
                a = Call(ref, "ObjectReference", "BlockActivation", true, true);
            }
            a_c.applied = want;
            REX::INFO("[Wars] workbench {:#x}: {} (owner '{}', calls {} {})", a_ref, a_c.mine ? "ours" : "blocked", a_c.owner, a, b);
        }
    }

    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload)
    {
        if (!Wars::Live()) return;
        if (a_type == static_cast<uint16_t>(MessageType::WarsCapture)) {
            if (a_payload.size() < sizeof(WarsCaptureMsg)) return;
            WarsCaptureMsg m;
            std::memcpy(&m, a_payload.data(), sizeof(m));
            m.name[sizeof(m.name) - 1] = 0;
            if (m.state == 2) {
                Hud(std::format("{} IS YOURS. Its workbench is yours to build with.", m.name));
            } else if (m.state == 1) {
                Hud(std::format("Capture of {} stopped: stay at its workbench, with no enemies near.", m.name));
            } else if (m.held == 1 || m.held % 5 == 0) {
                Hud(std::format("CAPTURING {}: {} / {} s - hold the workbench", m.name, m.held, m.need));
            }
            return;
        }
        if (a_type == static_cast<uint16_t>(MessageType::WarsClaim)) {
            if (a_payload.size() < sizeof(WarsClaimMsg)) return;
            WarsClaimMsg m;
            std::memcpy(&m, a_payload.data(), sizeof(m));
            m.owner[sizeof(m.owner) - 1] = 0;
            m.name[sizeof(m.name) - 1] = 0;
            std::lock_guard lock(g_lock);
            auto& c = g_claims[m.workbench];
            const bool mine = m.mine != 0;
            if (c.mine && !mine)
                Hud(m.owner[0] ? std::format("You lost {}: {} holds it now.", m.name, m.owner)
                               : std::format("You lost {}: it was overrun while nobody was there to hold it.", m.name));
            if (c.mine != mine) c.applied = -1;
            c.mine = mine;
            c.owner = m.owner;
            c.place = m.place;
        }
    }

    void Tick()
    {
        if (!Wars::Live()) return;
        std::lock_guard lock(g_lock);
        for (auto& [ref, c] : g_claims) Apply(ref, c);
    }

    uint32_t MyPlace()
    {
        std::lock_guard lock(g_lock);
        for (auto& [ref, c] : g_claims) if (c.mine && c.place) return c.place;
        return 0;
    }

    void Reset()
    {
        std::lock_guard lock(g_lock);
        for (auto& [ref, c] : g_claims) c.applied = -1;   // the new world's workbenches get it again
    }
}
