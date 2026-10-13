// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/DynamicActors.h"
#include "sync/CoopSync.h"
#include "sync/Range.h"
#include "sync/Authority.h"
#include "network/NetworkClient.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace F4MP::DynamicActors
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kAnnounceEvery = std::chrono::seconds(5);
        constexpr auto kForgetAfter = std::chrono::seconds(15);

        struct Announce
        {
            uint32_t spawnFormId = 0;
            RE::NiPoint3 pos;
            float angleZ = 0.0f;
            Clock::time_point lastSeen;
            Clock::time_point retryAfter;
        };

        // Key for another player's runtime actor: runtime IDs are only unique per game.
        uint64_t Key(uint32_t owner, uint32_t ref) { return (static_cast<uint64_t>(owner) << 32) | ref; }

        // owner side
        std::unordered_map<uint32_t, Clock::time_point> g_lastAnnounced;
        // mirror side
        std::unordered_map<uint64_t, Announce> g_announced;       // (owner, ref) -> latest announce
        std::unordered_map<uint64_t, uint32_t> g_map;             // (owner, ref) -> local FormID
        std::unordered_set<uint32_t> g_mappedLocal;                // local FormIDs we created
        std::unordered_set<uint32_t> g_suppressed;
        bool g_suppress = true;

        struct InFlight
        {
            uint64_t key = 0;
            std::unordered_set<uint32_t> before;
            Clock::time_point at;
        };
        bool g_inFlight = false;
        InFlight g_flight;

        template <class T>
        T* Lookup(uint32_t id)
        {
            auto* f = id ? RE::TESForm::GetFormByID(id) : nullptr;
            return f ? f->As<T>() : nullptr;
        }

        RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

        float Dist2(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        // Player bodies and test dummies are runtime actors too, but they belong to the co-op layer.
        bool IsCoopOwned(RE::TESObjectREFR* ref)
        {
            const uint32_t fid = ref->GetFormID();
            if (GetPlayerIdForBody(fid) != 0 || g_mappedLocal.contains(fid)) return true;
            auto* base = ref->GetObjectReference();
            const uint32_t b = base ? base->GetFormID() : 0;
            if (b == 0x00000007 || b == 0x0003C62E || b == 0x0018E8F2 || b >= 0xFFF00000) return true;
            // Built settlement objects that are actors (turrets): the workshop sync shares them already. As
            // "spawns" they got a second, unlinked copy on the other screen, or the copy was disabled.
            static auto* s_workshopItem = Lookup<RE::BGSKeyword>(0x00054BA6);
            return s_workshopItem && ref->GetLinkedRef(s_workshopItem) != nullptr;
        }

        template <class F>
        void ForEachRuntimeActor(float range, F&& fn)
        {
            auto* tes = RE::TES::GetSingleton();
            auto* pl = Player();
            if (!tes || !pl) return;
            tes->ForEachReferenceInRange(pl->GetPosition(), range, [&](RE::TESObjectREFR* ref) {
                if (ref && ref != static_cast<RE::TESObjectREFR*>(pl) && ref->GetFormID() >= kRuntimeIdStart &&
                    !ref->IsDeleted() && ref->As<RE::Actor>())
                    fn(ref->As<RE::Actor>());
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        // A runtime actor's base is itself a temporary record when it came from a leveled list.
        // Walk the template chain down to a record from the master files that placeatme can use.
        uint32_t SpawnFormFor(RE::Actor* a)
        {
            // The record its face came from first: a template like WorkshopNPC picks face and sex at
            // random on every placeatme, so the other screen showed a woman where this one had a man
            //. The face record itself is a full NPC with that one look.
            if (auto* npc = a->GetNPC()) {
                if (auto* face = npc->GetRootFaceNPC(); face && face != npc && face->GetFormID() < kRuntimeIdStart && !face->IsPreset()) {
                    static int s_logged = 0;
                    if (s_logged++ < 20)
                        REX::INFO("[Dynamic] actor {:#x}: copies spawn from its face record {:#x} ({})", a->GetFormID(), face->GetFormID(),
                            face->IsFemale() ? "female" : "male");
                    return face->GetFormID();
                }
            }
            if (auto* t = a->GetTemplateActorBase(); t && t->GetFormID() < kRuntimeIdStart) return t->GetFormID();
            RE::TESForm* cur = a->GetNPC();
            for (int depth = 0; cur && depth < 8; ++depth) {
                if (cur->GetFormID() < kRuntimeIdStart) return cur->GetFormID();
                auto* npc = cur->As<RE::TESNPC>();
                cur = npc ? npc->baseTemplateForm : nullptr;
            }
            return 0;
        }

        void RemoveLocal(uint32_t fid)
        {
            if (auto* ref = Lookup<RE::TESObjectREFR>(fid)) {
                ref->Disable();
                ref->SetWantsDelete(true);
                ref->MarkAsDeleted();
            }
        }
    }

    void HostAnnounce(RE::Actor* actor)
    {
        if (!actor) return;
        const uint32_t hostId = actor->GetFormID();
        if (hostId < kRuntimeIdStart || IsCoopOwned(actor) || !Authority::IOwn(actor)) return;
        const auto now = Clock::now();
        auto& last = g_lastAnnounced[hostId];
        if (now - last < kAnnounceEvery) return;
        last = now;

        const uint32_t spawnForm = SpawnFormFor(actor);
        if (!spawnForm) return;
        Network::CoopDynamicActorMsg msg{};
        msg.hostRefId = hostId;
        msg.spawnFormId = spawnForm;
        const auto p = actor->GetPosition();
        msg.x = p.x; msg.y = p.y; msg.z = p.z;
        msg.angleZ = actor->GetHeading();
        Network::NetworkClient::GetInstance().SendPacket(Network::MessageType::CoopDynamicActor, &msg, sizeof(msg));
    }

    void OnAnnounce(uint32_t ownerId, uint32_t ownerRefId, uint32_t spawnFormId, float x, float y, float z, float angleZ)
    {
        auto& a = g_announced[Key(ownerId, ownerRefId)];
        a.spawnFormId = spawnFormId;
        a.pos = { x, y, z };
        a.angleZ = angleZ;
        a.lastSeen = Clock::now();
    }

    RE::Actor* Resolve(uint32_t ownerId, uint32_t ownerRefId)
    {
        auto it = g_map.find(Key(ownerId, ownerRefId));
        return it == g_map.end() ? nullptr : Lookup<RE::Actor>(it->second);
    }

    bool IsMirrorCopy(uint32_t localFormId) { return g_mappedLocal.contains(localFormId); }

    bool OwnerOf(uint32_t localFormId, uint32_t& ownerId, uint32_t& ownerRefId)
    {
        if (!g_mappedLocal.contains(localFormId)) return false;
        for (auto& [key, local] : g_map)
            if (local == localFormId) {
                ownerId = static_cast<uint32_t>(key >> 32);
                ownerRefId = static_cast<uint32_t>(key);
                return true;
            }
        return false;
    }

    void ClientTick()
    {
        auto* pl = Player();
        if (!pl) return;
        const auto now = Clock::now();

        // 1. Finish a spawn: the new actor is the runtime actor that wasn't there before.
        if (g_inFlight && now - g_flight.at > std::chrono::milliseconds(500)) {
            g_inFlight = false;
            RE::Actor* found = nullptr;
            ForEachRuntimeActor(2000.0f, [&](RE::Actor* a) {
                if (!found && !g_flight.before.contains(a->GetFormID()) && !IsCoopOwned(a)) found = a;
            });
            auto ann = g_announced.find(g_flight.key);
            if (found && ann != g_announced.end()) {
                found->InitiateDoNothingPackage();
                found->StopCombat();
                found->SetPosition(ann->second.pos, true);
                found->SetHeading(ann->second.angleZ);
                g_map[g_flight.key] = found->GetFormID();
                g_mappedLocal.insert(found->GetFormID());
                REX::INFO("[Dynamic] player {} actor {:#x} -> local {:#x} (from {:#x})", g_flight.key >> 32,
                    static_cast<uint32_t>(g_flight.key), found->GetFormID(), ann->second.spawnFormId);
            } else if (ann != g_announced.end()) {
                ann->second.retryAfter = now + std::chrono::seconds(10);
                REX::WARN("[Dynamic] spawning a copy of player {} actor {:#x} (from {:#x}) failed; retry in 10 s",
                    g_flight.key >> 32, static_cast<uint32_t>(g_flight.key), ann->second.spawnFormId);
            }
        }

        // 2. Forget actors the host stopped announcing (died long ago, despawned, out of range).
        for (auto it = g_announced.begin(); it != g_announced.end();) {
            if (now - it->second.lastSeen > kForgetAfter) {
                if (auto m = g_map.find(it->first); m != g_map.end()) {
                    RemoveLocal(m->second);
                    g_mappedLocal.erase(m->second);
                    g_map.erase(m);
                }
                it = g_announced.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = g_map.begin(); it != g_map.end();) {   // local copy vanished (cell unload)
            auto* a = Lookup<RE::Actor>(it->second);
            if (!a || a->IsDeleted()) { g_mappedLocal.erase(it->second); it = g_map.erase(it); }
            else ++it;
        }

        // 3. Start the next spawn (one at a time).
        if (!g_inFlight) {
            for (auto& [key, a] : g_announced) {
                if (g_map.contains(key) || now < a.retryAfter) continue;
                if (Dist2(a.pos, pl->GetPosition()) > Range::Loaded() * Range::Loaded()) continue;
                g_flight = {};
                g_flight.key = key;
                ForEachRuntimeActor(2000.0f, [&](RE::Actor* x) { g_flight.before.insert(x->GetFormID()); });
                g_flight.at = now;
                g_inFlight = true;
                char cmd[64];
                std::snprintf(cmd, sizeof(cmd), "player.placeatme %X 1", a.spawnFormId);
                RE::Console::ExecuteCommand(cmd);
                break;
            }
        }

        // 4. In areas another player owns, our own runtime actors (random encounters, scripted
        //    spawns) would duplicate theirs. Disable the ones nobody announced to us.
        static Clock::time_point s_lastSweep{};
        if (g_suppress && !g_inFlight && now - s_lastSweep > std::chrono::seconds(2)) {
            s_lastSweep = now;
            std::vector<RE::Actor*> ghosts;
            ForEachRuntimeActor(Range::Loaded(), [&](RE::Actor* a) {
                if (!IsCoopOwned(a) && !a->IsDisabled() && !Authority::IOwn(a)) ghosts.push_back(a);
            });
            for (auto* a : ghosts) {
                a->Disable();
                if (g_suppressed.insert(a->GetFormID()).second)
                    REX::INFO("[Dynamic] disabled local-only runtime actor {:#x} (another player owns this area)", a->GetFormID());
            }
        }
    }

    bool& SuppressLocalSpawns() { return g_suppress; }
    uint32_t MappedCount() { return static_cast<uint32_t>(g_map.size()); }
    uint32_t SuppressedCount() { return static_cast<uint32_t>(g_suppressed.size()); }

    void Reset()
    {
        for (auto& [hostId, local] : g_map) RemoveLocal(local);
        g_map.clear();
        g_mappedLocal.clear();
        g_announced.clear();
        g_lastAnnounced.clear();
        // Our own spawns we hid while another player ran the area: back in our world.
        for (uint32_t fid : g_suppressed)
            if (auto* a = Lookup<RE::Actor>(fid); a && a->IsDisabled() && !a->IsDeleted()) a->Enable(false);
        g_suppressed.clear();
        g_inFlight = false;
    }
}
