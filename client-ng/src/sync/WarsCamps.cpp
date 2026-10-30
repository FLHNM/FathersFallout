// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Wasteland Wars A-Life camps, the game's side: the server says what a squad has built where it lives (server/src/Wars/
// ALifeCamps.cpp); this game places those things (the game's own cooking fires, bedrolls, tents, workbenches...) and
// takes them away when the camp loses them or the player goes far away. Heights come from the server (the ground where
// the game itself placed NPCs nearby; a Havok ray from TESObjectCELL::Pick crashed the game, testing). The NPCs' sandbox
// around their camp then uses them: they cook, sleep and work at the benches like the game's settlers.

#include "pch.h"
#include "sync/WarsCamps.h"
#include "sync/Wars.h"
#include "network/NetworkClient.h"

#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace F4MP::Wars::Camps
{
    using namespace Network;

    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Placed {
            WarsCampItem item{};
            uint32_t ref = 0;      // 0 = not placed (yet)
        };
        struct Live {
            WarsCampMsg want{};                  // what the server says is there
            std::vector<Placed> placed;          // what we put there, in the camp's item order
            bool dirty = true;
            bool swept = false;                  // leftovers from an old save removed
            std::unordered_set<uint32_t> before; // runtime refs at the anchor before our placeatme
            Clock::time_point placedAt{};
            bool placing = false;
        };
        std::unordered_map<uint32_t, Live> g_camps;   // by the place's map marker

        template <class T = RE::TESObjectREFR>
        T* RefOf(uint32_t a_fid)
        {
            auto* f = a_fid ? RE::TESForm::GetFormByID(a_fid) : nullptr;
            return f ? f->As<T>() : nullptr;
        }

        void Console(const char* a_fmt, auto... a_args)
        {
            char cmd[160];
            std::snprintf(cmd, sizeof(cmd), a_fmt, a_args...);
            RE::Console::ExecuteCommand(cmd);
        }

        void Delete(uint32_t a_fid)
        {
            if (auto* r = RefOf(a_fid); r && !r->IsDeleted()) {
                r->Disable();
                r->SetWantsDelete(true);
                r->MarkAsDeleted();
            }
        }

        RE::NiPoint3 Middle(const WarsCampMsg& a_c)
        {
            RE::NiPoint3 m{};
            for (int i = 0; i < a_c.count; ++i) m += RE::NiPoint3{ a_c.items[i].x, a_c.items[i].y, a_c.items[i].z };
            return a_c.count ? m / static_cast<float>(a_c.count) : m;
        }

        bool Same(const WarsCampItem& a_a, const WarsCampItem& a_b)
        {
            return a_a.form == a_b.form && std::abs(a_a.x - a_b.x) < 1.0f && std::abs(a_a.y - a_b.y) < 1.0f;
        }

        // An old save can hold our things from last time (runtime references are saved): the same base objects near
        // the middle that aren't ours go first.
        void SweepLeftovers(Live& a_c)
        {
            auto* tes = RE::TES::GetSingleton();
            if (!tes) return;
            std::unordered_set<uint32_t> forms, ours;
            for (int i = 0; i < a_c.want.count; ++i) forms.insert(a_c.want.items[i].form);
            for (auto& p : a_c.placed) if (p.ref) ours.insert(p.ref);
            int n = 0;
            tes->ForEachReferenceInRange(Middle(a_c.want), 900.0f, [&](RE::TESObjectREFR* r) {
                auto* base = r ? r->GetObjectReference() : nullptr;
                if (r && base && r->GetFormID() >= 0xFF000000u && forms.contains(base->GetFormID()) && !ours.contains(r->GetFormID()) &&
                    !r->IsDeleted()) {
                    Delete(r->GetFormID());
                    ++n;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (n) REX::INFO("[Wars] camp {:#x}: {} leftover thing(s) from an old save removed", a_c.want.camp, n);
        }

        // One step per tick: drop what the camp lost, then place everything missing in one go and pick the new
        // references up on the next tick (placeatme makes them at the place's map marker; then moved into place).
        void Step(uint32_t a_id, Live& a_c)
        {
            auto* anchor = RefOf(a_id);
            if (!anchor) return;
            if (a_c.placing) {
                std::unordered_map<uint32_t, std::vector<uint32_t>> fresh;   // by base form, in FormID order
                auto* tes = RE::TES::GetSingleton();
                if (tes)
                    tes->ForEachReferenceInRange(anchor->GetPosition(), 700.0f, [&](RE::TESObjectREFR* r) {
                        auto* base = r ? r->GetObjectReference() : nullptr;
                        if (r && base && r->GetFormID() >= 0xFF000000u && !a_c.before.contains(r->GetFormID()))
                            fresh[base->GetFormID()].push_back(r->GetFormID());
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
                size_t need = 0;
                for (auto& p : a_c.placed) need += p.ref == 0;
                size_t got = 0;
                for (auto& [f, v] : fresh) got += v.size();
                if (got < need && Clock::now() - a_c.placedAt < std::chrono::seconds(3)) return;   // more to come
                for (auto& [f, v] : fresh) std::sort(v.begin(), v.end());
                int moved = 0;
                for (auto& p : a_c.placed) {
                    if (p.ref) continue;
                    auto& v = fresh[p.item.form];
                    if (v.empty()) continue;
                    p.ref = v.front();
                    v.erase(v.begin());
                    Console("%X.setpos x %.1f", p.ref, p.item.x);
                    Console("%X.setpos y %.1f", p.ref, p.item.y);
                    Console("%X.setpos z %.1f", p.ref, p.item.z);
                    Console("%X.setangle z %.1f", p.ref, p.item.rz * 57.2957795f);
                    ++moved;
                }
                for (auto& [f, v] : fresh)
                    for (uint32_t extra : v) Delete(extra);   // more than we asked for (shouldn't happen)
                a_c.placing = false;
                REX::INFO("[Wars] camp {:#x} (faction {}, level {}): {} thing(s) placed", a_id, a_c.want.faction, a_c.want.level, moved);
                return;
            }
            if (!a_c.dirty) return;
            a_c.dirty = false;
            if (!a_c.swept) { SweepLeftovers(a_c); a_c.swept = true; }
            // Keep what's still the same, in order; the rest goes.
            std::vector<Placed> next(a_c.want.count);
            for (int i = 0; i < a_c.want.count; ++i) {
                next[i].item = a_c.want.items[i];
                if (i < static_cast<int>(a_c.placed.size()) && a_c.placed[i].ref && Same(a_c.placed[i].item, next[i].item) && RefOf(a_c.placed[i].ref)) {
                    next[i].ref = a_c.placed[i].ref;
                    a_c.placed[i].ref = 0;
                }
            }
            for (auto& p : a_c.placed) if (p.ref) Delete(p.ref);
            a_c.placed = std::move(next);
            std::unordered_map<uint32_t, int> count;
            for (auto& p : a_c.placed) if (!p.ref && RE::TESForm::GetFormByID(p.item.form)) count[p.item.form]++;
            if (count.empty()) return;
            a_c.before.clear();
            if (auto* tes = RE::TES::GetSingleton())
                tes->ForEachReferenceInRange(anchor->GetPosition(), 700.0f, [&](RE::TESObjectREFR* r) {
                    if (r && r->GetFormID() >= 0xFF000000u) a_c.before.insert(r->GetFormID());
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            for (auto& [form, n] : count) Console("%X.placeatme %X %d", a_id, form, n);
            a_c.placing = true;
            a_c.placedAt = Clock::now();
        }
    }

    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload)
    {
        if (!Wars::Live()) return;
        if (a_type == static_cast<uint16_t>(MessageType::WarsCampGone)) {
            if (a_payload.size() < sizeof(WarsCampGoneMsg)) return;
            WarsCampGoneMsg g;
            std::memcpy(&g, a_payload.data(), sizeof(g));
            auto it = g_camps.find(g.camp);
            if (it == g_camps.end()) return;
            for (auto& p : it->second.placed) if (p.ref) Delete(p.ref);
            g_camps.erase(it);
            return;
        }
        if (a_payload.size() < sizeof(WarsCampMsg)) return;
        WarsCampMsg m;
        std::memcpy(&m, a_payload.data(), sizeof(m));
        m.count = std::min<uint8_t>(m.count, kWarsCampMax);
        for (int i = 0; i < m.count; ++i)
            if (!SanePosition(m.items[i].x, m.items[i].y, m.items[i].z) || !std::isfinite(m.items[i].rz)) return;
        auto& c = g_camps[m.camp];
        c.want = m;
        c.dirty = true;
    }

    // Main thread, about once a second; outdoors in the Commonwealth only (the anchors are its map markers).
    void Tick(bool a_outside)
    {
        if (!Wars::Live() || !a_outside) return;
        for (auto& [id, c] : g_camps)
            if (c.placing || c.dirty) { Step(id, c); break; }   // one camp at a time (each finds its own newcomers)
    }

    void Reset()
    {
        for (auto& [id, c] : g_camps)
            for (auto& p : c.placed) if (p.ref) Delete(p.ref);
        g_camps.clear();
    }

    void Log()
    {
        for (auto& [id, c] : g_camps) {
            int up = 0;
            for (auto& p : c.placed) up += p.ref && RefOf(p.ref);
            REX::INFO("[Wars] camp {:#x}: faction {}, level {}, {} of {} things here", id, c.want.faction, c.want.level, up, c.want.count);
        }
    }
}
