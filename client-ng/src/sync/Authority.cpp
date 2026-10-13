// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Authority.h"
#include "sync/CoopSync.h"
#include "sync/Range.h"
#include "network/NetworkClient.h"

#include <atomic>
#include <unordered_map>
#include <vector>

namespace F4MP::Authority
{
    namespace
    {
        struct PlayerArea { uint32_t id; uint32_t space; RE::NiPoint3 pos; float radius; };

        std::vector<PlayerArea> g_players;                    // refreshed every tick, includes us
        std::unordered_map<uint32_t, float> g_radius;          // remote loaded radius (from hello)
        std::unordered_map<uint32_t, uint32_t> g_lastOwner;    // ref FormID -> owner last tick
        std::atomic<uint32_t> g_localSpace{ 0 };
        uint32_t g_hostId = 0;

        // Ownership needs a margin inside the loaded edge (cells at the border may still be
        // loading) and stickiness so an NPC near a boundary doesn't flip owners every tick.
        constexpr float kClaimFraction = 0.85f;

        float Dist2(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        const PlayerArea* Find(uint32_t id)
        {
            for (auto& p : g_players)
                if (p.id == id) return &p;
            return nullptr;
        }

        bool Covers(const PlayerArea& p, uint32_t space, const RE::NiPoint3& pos, float fraction)
        {
            // Space 0 = unknown (e.g. the test simulator): assume the same space.
            if (p.space && space && p.space != space) return false;
            const float r = p.radius * fraction;
            return Dist2(p.pos, pos) <= r * r;
        }
    }

    uint32_t SpaceOf(const RE::TESObjectREFR* ref)
    {
        auto* cell = ref ? ref->GetParentCell() : nullptr;
        if (!cell) return 0;
        if (cell->IsInterior()) return cell->GetFormID();
        return cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
    }

    uint32_t LocalSpace()
    {
        const uint32_t s = SpaceOf(RE::PlayerCharacter::GetSingleton());
        g_localSpace = s;
        return s;
    }

    uint32_t LocalSpaceAtomic() { return g_localSpace; }

    void SetRemoteRadius(uint32_t playerId, float radius) { if (radius > 0.0f) g_radius[playerId] = radius; }
    void SetHost(uint32_t playerId) { g_hostId = playerId; }
    uint32_t HostId() { return IsHost() ? MyId() : g_hostId; }
    uint32_t MyId() { return Network::NetworkClient::GetInstance().GetPlayerId(); }

    void Refresh()
    {
        g_players.clear();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (pl) g_players.push_back({ MyId(), LocalSpace(), pl->GetPosition(), Range::Loaded() });
        for (auto& [id, m] : Network::NetworkClient::GetInstance().GetRemotePlayers()) {
            auto r = g_radius.find(id);
            g_players.push_back({ id, m.cellId, { m.x, m.y, m.z }, r != g_radius.end() ? r->second : 2.0f * Range::kCellSize });
        }
    }

    uint32_t OwnerOf(uint32_t space, const RE::NiPoint3& pos)
    {
        if (auto* host = Find(HostId()); host && Covers(*host, space, pos, kClaimFraction)) return host->id;
        uint32_t best = 0;
        for (auto& p : g_players)
            if (Covers(p, space, pos, kClaimFraction) && (best == 0 || p.id < best)) best = p.id;
        return best;
    }

    bool IOwn(const RE::TESObjectREFR* ref)
    {
        if (!ref) return false;
        const uint32_t space = SpaceOf(ref);
        const auto pos = ref->GetPosition();
        const uint32_t fid = ref->GetFormID();
        uint32_t owner = 0;
        // Keep the previous owner while it still has the spot loaded at all.
        if (auto it = g_lastOwner.find(fid); it != g_lastOwner.end()) {
            if (auto* prev = Find(it->second); prev && Covers(*prev, space, pos, 1.0f)) {
                const uint32_t fresh = OwnerOf(space, pos);
                // The host always takes over areas it has loaded.
                owner = (fresh == HostId()) ? fresh : prev->id;
            }
        }
        if (!owner) owner = OwnerOf(space, pos);
        if (!owner) owner = MyId();   // nobody else has it loaded: it's ours by definition
        g_lastOwner[fid] = owner;
        return owner == MyId();
    }

    void Reset()
    {
        g_players.clear();
        g_radius.clear();
        g_lastOwner.clear();
        g_hostId = 0;
    }
}
