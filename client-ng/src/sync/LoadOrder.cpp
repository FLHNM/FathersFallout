// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/LoadOrder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>

namespace F4MP::LoadOrder
{
    namespace
    {
        constexpr uint8_t kFormatVersion = 1;
        constexpr size_t kLightSlots = 4096;

        struct Plugin { uint16_t index; std::string name; };   // name lowercased
        struct List { std::vector<Plugin> full, light; };

        struct Table
        {
            bool identical = false;
            std::array<int16_t, 256> full{};          // their full slot -> ours, -1 = we don't have it
            std::vector<int16_t> light;               // their light slot -> ours
            std::vector<std::string> fullNames;       // their slot -> name (for the missing-form log)
            std::vector<std::string> theyLack, weLack;
            bool reordered = false;
        };

        std::mutex g_mutex;
        std::unordered_map<uint32_t, Table> g_tables;
        std::atomic<size_t> g_dropped{ 0 };

        std::string Lower(std::string_view s)
        {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        const List& Mine()
        {
            static List list;
            static bool done = false;
            if (done) return list;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return list;
            done = true;
            for (auto* f : dh->compiledFileCollection.files)
                if (f) list.full.push_back({ static_cast<uint16_t>(f->GetCompileIndex()), Lower(f->GetFilename()) });
            for (auto* f : dh->compiledFileCollection.smallFiles)
                if (f) list.light.push_back({ static_cast<uint16_t>(f->GetSmallFileCompileIndex()), Lower(f->GetFilename()) });
            return list;
        }

        // Format: version u8, isHost u8, full count u16, light count u16, then per plugin: index u16,
        // name length u8, name bytes (full plugins first, then light ones).
        bool Parse(const uint8_t* p, size_t size, List& out)
        {
            const uint8_t* end = p + size;
            if (size < 6 || p[0] != kFormatVersion) return false;
            uint16_t nFull, nLight;
            std::memcpy(&nFull, p + 2, 2);
            std::memcpy(&nLight, p + 4, 2);
            p += 6;
            auto read = [&](std::vector<Plugin>& v, uint16_t n) {
                for (uint16_t i = 0; i < n; ++i) {
                    if (end - p < 3) return false;
                    Plugin pl;
                    std::memcpy(&pl.index, p, 2);
                    const uint8_t len = p[2];
                    p += 3;
                    if (end - p < len) return false;
                    pl.name = Lower(std::string_view(reinterpret_cast<const char*>(p), len));
                    p += len;
                    v.push_back(std::move(pl));
                }
                return true;
            };
            return read(out.full, nFull) && read(out.light, nLight);
        }

        std::string Join(const std::vector<std::string>& names, size_t max)
        {
            std::string out;
            for (size_t i = 0; i < names.size() && i < max; ++i) out += (i ? ", " : "") + names[i];
            if (names.size() > max) out += std::format(" (+{} more)", names.size() - max);
            return out;
        }
    }

    std::vector<uint8_t> BuildMyList(bool isHost)
    {
        const auto& me = Mine();
        std::vector<uint8_t> out;
        out.push_back(kFormatVersion);
        out.push_back(isHost ? 1 : 0);
        const uint16_t nFull = static_cast<uint16_t>(me.full.size()), nLight = static_cast<uint16_t>(me.light.size());
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&nFull), reinterpret_cast<const uint8_t*>(&nFull) + 2);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&nLight), reinterpret_cast<const uint8_t*>(&nLight) + 2);
        auto add = [&](const std::vector<Plugin>& v) {
            for (auto& pl : v) {
                const uint8_t len = static_cast<uint8_t>(std::min<size_t>(pl.name.size(), 255));
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&pl.index), reinterpret_cast<const uint8_t*>(&pl.index) + 2);
                out.push_back(len);
                out.insert(out.end(), pl.name.begin(), pl.name.begin() + len);
            }
        };
        add(me.full);
        add(me.light);
        return out;
    }

    void OnList(uint32_t senderId, const uint8_t* data, size_t size)
    {
        List theirs;
        if (!Parse(data, size, theirs)) {
            REX::WARN("[LoadOrder] bad plugin list from player {} ({} bytes)", senderId, size);
            return;
        }
        const auto& me = Mine();
        std::unordered_map<std::string, uint16_t> myFull, myLight;
        for (auto& p : me.full) myFull[p.name] = p.index;
        for (auto& p : me.light) myLight[p.name] = p.index;

        Table t;
        t.full.fill(-1);
        t.light.assign(kLightSlots, -1);
        t.fullNames.assign(256, {});
        t.identical = theirs.full.size() == me.full.size() && theirs.light.size() == me.light.size();
        for (auto& p : theirs.full) {
            if (p.index >= 256) continue;
            t.fullNames[p.index] = p.name;
            if (auto it = myFull.find(p.name); it != myFull.end()) {
                t.full[p.index] = static_cast<int16_t>(it->second);
                if (it->second != p.index) t.identical = false, t.reordered = true;
            } else {
                t.identical = false;
                t.weLack.push_back(p.name);
            }
        }
        for (auto& p : theirs.light) {
            if (p.index >= kLightSlots) continue;
            if (auto it = myLight.find(p.name); it != myLight.end()) {
                t.light[p.index] = static_cast<int16_t>(it->second);
                if (it->second != p.index) t.identical = false, t.reordered = true;
            } else {
                t.identical = false;
                t.weLack.push_back(p.name);
            }
        }
        std::unordered_map<std::string, bool> theirNames;
        for (auto& p : theirs.full) theirNames[p.name] = true;
        for (auto& p : theirs.light) theirNames[p.name] = true;
        for (auto& p : me.full) if (!theirNames.contains(p.name)) t.theyLack.push_back(p.name);
        for (auto& p : me.light) if (!theirNames.contains(p.name)) t.theyLack.push_back(p.name);

        if (t.identical)
            REX::INFO("[LoadOrder] player {} runs the same {}+{} plugins in the same order", senderId, theirs.full.size(), theirs.light.size());
        else {
            REX::WARN("[LoadOrder] player {} runs {}+{} plugins (we run {}+{}){}: their FormIDs are translated",
                senderId, theirs.full.size(), theirs.light.size(), me.full.size(), me.light.size(), t.reordered ? ", some in another order" : "");
            for (auto& n : t.weLack) REX::WARN("[LoadOrder]   only player {} has: {}", senderId, n);
            for (auto& n : t.theyLack) REX::WARN("[LoadOrder]   only we have: {}", n);
        }
        std::lock_guard lk(g_mutex);
        g_tables[senderId] = std::move(t);
    }

    bool Known(uint32_t senderId)
    {
        std::lock_guard lk(g_mutex);
        return g_tables.contains(senderId);
    }

    bool Identical(uint32_t senderId)
    {
        std::lock_guard lk(g_mutex);
        auto it = g_tables.find(senderId);
        return it != g_tables.end() && it->second.identical;
    }

    uint32_t ToLocal(uint32_t senderId, uint32_t formId)
    {
        const uint32_t hi = formId >> 24;
        if (!formId || hi == 0xFF) return formId;
        std::lock_guard lk(g_mutex);
        auto it = g_tables.find(senderId);
        if (it == g_tables.end() || it->second.identical) return formId;
        const auto& t = it->second;
        int16_t ours;
        uint32_t local;
        if (hi == 0xFE) {
            ours = t.light[(formId >> 12) & 0xFFF];
            local = 0xFE000000u | (static_cast<uint32_t>(ours) << 12) | (formId & 0xFFF);
        } else {
            ours = t.full[hi];
            local = (static_cast<uint32_t>(ours) << 24) | (formId & 0xFFFFFF);
        }
        if (ours < 0) {
            const size_t n = g_dropped++;
            if (n < 30)
                REX::INFO("[LoadOrder] form {:08X} from player {} is from {}, which this game doesn't have: skipped", formId, senderId,
                    hi == 0xFE ? std::string("a light plugin") : (t.fullNames[hi].empty() ? std::string("an unknown plugin") : t.fullNames[hi]));
            return 0;
        }
        return local;
    }

    std::string Differences(uint32_t senderId)
    {
        std::lock_guard lk(g_mutex);
        auto it = g_tables.find(senderId);
        if (it == g_tables.end() || it->second.identical) return {};
        const auto& t = it->second;
        std::string out;
        if (!t.weLack.empty()) out += "They have mods you don't: " + Join(t.weLack, 3) + ". ";
        if (!t.theyLack.empty()) out += "You have mods they don't: " + Join(t.theyLack, 3) + ". ";
        if (out.empty() && t.reordered) out = "Same mods, different load order (handled). ";
        return out;
    }

    size_t Dropped() { return g_dropped; }

    void Forget(uint32_t senderId)
    {
        std::lock_guard lk(g_mutex);
        g_tables.erase(senderId);
    }

    void Reset()
    {
        std::lock_guard lk(g_mutex);
        g_tables.clear();
    }
}
