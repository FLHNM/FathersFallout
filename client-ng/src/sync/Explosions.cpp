// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Explosions.h"
#include "sync/Authority.h"
#include "network/NetworkClient.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <vector>

namespace F4MP::Explosions
{
    using namespace Network;
    using Clock = std::chrono::steady_clock;

    namespace
    {
        constexpr float kScanRadius = 6000.0f;       // ~85 m around our player
        constexpr float kReplayRadius = 9000.0f;     // replay only what could matter to us
        constexpr uint32_t kXMarker = 0x0000003B;    // invisible static: where replays are placed

        struct Recent { RE::NiPoint3 pos; Clock::time_point at; };

        std::unordered_map<RE::TESObjectREFR*, Clock::time_point> g_seen;   // explosion refs already handled
        std::deque<Recent> g_replays;      // explosions we set off for someone else
        std::deque<Recent> g_sent;         // explosions we announced
        std::vector<CoopExplosionMsg> g_pending;   // waiting for the marker
        uint32_t g_marker = 0;
        Clock::time_point g_markerRequested{};
        bool g_primed = false;             // first scan only learns what already exists

        float Dist(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        bool Near(std::deque<Recent>& list, const RE::NiPoint3& pos, std::chrono::milliseconds window, float radius)
        {
            const auto now = Clock::now();
            while (!list.empty() && now - list.front().at > std::chrono::seconds(5)) list.pop_front();
            for (auto& r : list)
                if (now - r.at <= window && Dist(r.pos, pos) <= radius) return true;
            return false;
        }

        void Console(const char* cmd) { RE::Console::ExecuteCommand(cmd); }

        RE::TESObjectREFR* Marker()
        {
            auto* form = g_marker ? RE::TESForm::GetFormByID(g_marker) : nullptr;
            auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (ref && !ref->IsDeleted() && !ref->IsDisabled()) return ref;
            g_marker = 0;
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* tes = RE::TES::GetSingleton();
            if (!pl || !tes) return nullptr;
            // placeatme's reference appears a frame later: look for it on the next ticks.
            if (g_markerRequested != Clock::time_point{} && Clock::now() - g_markerRequested < std::chrono::seconds(2)) {
                tes->ForEachReferenceInRange(pl->GetPosition(), 400.0f, [&](RE::TESObjectREFR* r) {
                    auto* base = r ? r->GetObjectReference() : nullptr;
                    if (base && base->GetFormID() == kXMarker && r->GetFormID() >= 0xFF000000u) {
                        g_marker = r->GetFormID();
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                if (g_marker) {
                    REX::INFO("[Explosions] replay marker {:#x}", g_marker);
                    return RE::TESForm::GetFormByID(g_marker)->As<RE::TESObjectREFR>();
                }
                return nullptr;
            }
            Console("player.placeatme 3B 1");
            g_markerRequested = Clock::now();
            return nullptr;
        }

        void Replay(const CoopExplosionMsg& m, RE::TESObjectREFR* marker)
        {
            char cmd[96];
            std::snprintf(cmd, sizeof(cmd), "%X.moveto player", marker->GetFormID());
            Console(cmd);
            std::snprintf(cmd, sizeof(cmd), "%X.setpos x %.1f", marker->GetFormID(), m.x);
            Console(cmd);
            std::snprintf(cmd, sizeof(cmd), "%X.setpos y %.1f", marker->GetFormID(), m.y);
            Console(cmd);
            std::snprintf(cmd, sizeof(cmd), "%X.setpos z %.1f", marker->GetFormID(), m.z);
            Console(cmd);
            std::snprintf(cmd, sizeof(cmd), "%X.placeatme %X 1", marker->GetFormID(), m.explosionFormId);
            Console(cmd);
            g_replays.push_back({ { m.x, m.y, m.z }, Clock::now() });
            REX::INFO("[Explosions] player {}'s explosion {:08X} at {:.0f} {:.0f} {:.0f}: replayed", m.senderId,
                m.explosionFormId, m.x, m.y, m.z);
        }
    }

    void Tick()
    {
        auto& net = NetworkClient::GetInstance();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* tes = RE::TES::GetSingleton();
        if (!net.IsConnected() || !pl || !tes) return;

        // Replays waiting for the marker.
        if (!g_pending.empty()) {
            if (auto* marker = Marker()) {
                for (auto& m : g_pending) Replay(m, marker);
                g_pending.clear();
            } else if (g_markerRequested != Clock::time_point{} && Clock::now() - g_markerRequested > std::chrono::seconds(3)) {
                g_pending.clear();   // no marker: drop them rather than replay late
            }
        }

        // New explosions near us.
        const auto now = Clock::now();
        for (auto it = g_seen.begin(); it != g_seen.end();)
            it = now - it->second > std::chrono::seconds(30) ? g_seen.erase(it) : std::next(it);
        tes->ForEachReferenceInRange(pl->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* ref) {
            auto* base = ref ? ref->GetObjectReference() : nullptr;
            if (!base || !base->As<RE::BGSExplosion>()) return RE::BSContainer::ForEachResult::kContinue;
            if (!g_seen.emplace(ref, now).second) return RE::BSContainer::ForEachResult::kContinue;
            if (!g_primed) return RE::BSContainer::ForEachResult::kContinue;
            const RE::NiPoint3 pos = ref->GetPosition();
            // Our replay, or what it set off (the car it blew up): theirs already has it.
            if (Near(g_replays, pos, std::chrono::milliseconds(2500), 1200.0f)) return RE::BSContainer::ForEachResult::kContinue;
            // A child of an explosion we just sent (the receiver's copy makes its own).
            if (Near(g_sent, pos, std::chrono::milliseconds(400), 300.0f)) return RE::BSContainer::ForEachResult::kContinue;
            CoopExplosionMsg msg{};
            msg.explosionFormId = base->GetFormID();
            msg.x = pos.x; msg.y = pos.y; msg.z = pos.z;
            msg.space = Authority::LocalSpace();
            net.SendPacket(MessageType::CoopExplosion, &msg, sizeof(msg));
            g_sent.push_back({ pos, now });
            REX::INFO("[Explosions] explosion {:08X} at {:.0f} {:.0f} {:.0f}: sent", msg.explosionFormId, pos.x, pos.y, pos.z);
            return RE::BSContainer::ForEachResult::kContinue;
        });
        g_primed = true;
    }

    void OnReceived(const CoopExplosionMsg& m)
    {
        if (m.space && Authority::LocalSpace() && m.space != Authority::LocalSpace()) return;   // elsewhere
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* form = RE::TESForm::GetFormByID(m.explosionFormId);
        if (!pl || !form || !form->As<RE::BGSExplosion>()) return;
        if (Dist(pl->GetPosition(), { m.x, m.y, m.z }) > kReplayRadius) return;
        if (auto* marker = Marker()) Replay(m, marker);
        else if (g_pending.size() < 16) g_pending.push_back(m);
    }

    void Reset()
    {
        g_seen.clear();
        g_replays.clear();
        g_sent.clear();
        g_pending.clear();
        g_primed = false;
        // The marker stays (it's in the save, harmless); it's looked up again by FormID.
    }
}
