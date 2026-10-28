// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Workshop.h"
#include "sync/GraphOverride.h"
#include "sync/CoopSync.h"
#include "network/NetworkClient.h"
#include "sync/Paths.h"
#include "sync/Authority.h"
#include "sync/Activity.h"
#include "sync/DynamicActors.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace F4MP::Workshop
{
    using namespace Network;

    namespace
    {
        constexpr uint32_t kWorkshopItemKeyword = 0x00054BA6;   // Fallout4.esm WorkshopItemKeyword
        constexpr uint32_t kDynamicFormIdStart = 0xFF000000;
        constexpr float kScanRadius = 12000.0f;                 // a settlement's whole build area

        struct Key {
            uint32_t owner, ref;
            bool operator==(const Key&) const = default;
        };
        struct KeyHash {
            size_t operator()(const Key& k) const { return (size_t(k.owner) << 32) ^ k.ref; }
        };

        // Built objects we know about, by FormID in OUR game. 'shared' = the others have it too
        // (built during this session, by us or by them); old builds of our own save are not.
        struct Tracked {
            Key key{};
            bool shared = false;
            RE::NiPoint3 pos, ang;
            bool fresh = false;   // loaded from the co-save: take its position before comparing
        };
        std::unordered_map<uint32_t, Tracked> g_tracked;
        std::unordered_map<Key, uint32_t, KeyHash> g_byKey;
        bool g_menuWasOpen = false;
        // Copies being placed: placeatme's reference only shows up a frame or more later.
        struct Pending {
            Key key;
            uint32_t base, bench;
            RE::NiPoint3 pos, ang;
            std::chrono::steady_clock::time_point since;
        };
        std::vector<Pending> g_pending;
        std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>> g_verify;
        // Loose props: the one we hold (and, after letting go, until it comes to rest).
        uint32_t g_carried = 0;
        RE::NiPoint3 g_carriedLast{};
        std::chrono::steady_clock::time_point g_settleUntil{};
        // Props other players carry: re-placed every frame while held, or physics drops them
        // between two updates (~10/s). Let go = placed once more where it came to rest.
        struct RemoteProp {
            RE::NiPoint3 pos, ang;
            bool held = false;
            std::chrono::steady_clock::time_point last{};
        };
        std::unordered_map<uint32_t, RemoteProp> g_remoteHeld;   // debug: copy state later
        bool g_applying = false;   // our own ActivateRef on a copy: not a local activation
        auto g_nextScan = std::chrono::steady_clock::now();

        template <class T>
        T* Lookup(uint32_t formId)
        {
            auto* form = formId ? RE::TESForm::GetFormByID(formId) : nullptr;
            return form ? form->As<T>() : nullptr;
        }

        template <class T>
        void Send(MessageType type, const T& msg) { NetworkClient::GetInstance().SendPacket(type, &msg, sizeof(T)); }

        // This player's permanent key: made once, kept in F4MP_player_key.txt. Builds are named by it
        // (not the session id, which changes on every connect) so links survive restarts.
        uint32_t MyKey()
        {
            static uint32_t s_key = 0;
            if (s_key) return s_key;
            const auto dir = F4SEDocsDir();
            const std::string file = dir.empty() ? "F4MP_player_key.txt" : dir + "/F4MP_player_key.txt";
            if (std::ifstream in(file); in) in >> std::hex >> s_key;
            if (!s_key) {
                std::random_device rd;
                while (!s_key) s_key = (static_cast<uint32_t>(rd()) << 1) ^ static_cast<uint32_t>(rd());
                std::ofstream(file) << std::hex << s_key << std::endl;
                REX::INFO("[Workshop] new permanent player key {:08X}", s_key);
            }
            return s_key;
        }

        RE::BGSKeyword* ItemKeyword() { return Lookup<RE::BGSKeyword>(kWorkshopItemKeyword); }

        bool WorkshopMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GetMenuOpen("WorkshopMenu");
        }

        bool Gone(RE::TESObjectREFR* ref) { return !ref || ref->IsDeleted() || ref->IsDisabled(); }
        bool IsWire(RE::TESObjectREFR* r);   // a power wire (below)

        bool Moved(const Tracked& t, const RE::TESObjectREFR* ref)
        {
            const auto p = ref->data.location, a = ref->data.angle;
            const float dp = std::fabs(p.x - t.pos.x) + std::fabs(p.y - t.pos.y) + std::fabs(p.z - t.pos.z);
            const float da = std::fabs(a.x - t.ang.x) + std::fabs(a.y - t.ang.y) + std::fabs(a.z - t.ang.z);
            return dp > 1.0f || da > 0.005f;
        }

        void Remember(Tracked& t, const RE::TESObjectREFR* ref)
        {
            t.pos = ref->data.location;
            t.ang = ref->data.angle;
        }

        // Put a reference exactly at (pos, ang). The console's setpos/setangle didn't move these
        // (the copy stayed at the workbench), and a disable/enable cycle left it without a model.
        void PlaceExactly(uint32_t fid, const RE::NiPoint3& p, const RE::NiPoint3& a)
        {
            auto* ref = Lookup<RE::TESObjectREFR>(fid);
            if (!ref) return;
            ref->SetLocationOnReference(p);
            ref->SetAngleOnReference(a);
            if (ref->Get3D()) ref->Update3DPosition(true);   // model + collision follow, no fade
            else ref->Load3D(false);
        }

        // Every built object (runtime reference linked to a workbench) near the player.
        template <class F>
        void ForEachBuilt(F&& fn)
        {
            auto* tes = RE::TES::GetSingleton();
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* kw = ItemKeyword();
            if (!tes || !pl || !kw) return;
            tes->ForEachReferenceInRange(pl->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* ref) {
                if (ref && ref->GetFormID() >= kDynamicFormIdStart && !Gone(ref) && !IsWire(ref) && ref->GetLinkedRef(kw))
                    fn(ref);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        void ScanPluginRefs(bool baseline);   // below, next to the host's-world state

        // ---- power wires: splines (BNDS) whose ExtraPowerLinks hold their two ends; not linked to a workbench ----
        struct WireState {
            Key key{};
            bool shared = false;
            RE::NiPoint3 pos{}, half{};
        };
        std::unordered_map<uint32_t, WireState> g_wires;            // our FormID -> wire
        std::unordered_map<Key, uint32_t, KeyHash> g_wireByKey;
        std::deque<std::pair<CoopWirePlacedMsg, std::chrono::steady_clock::time_point>> g_wireRetry;   // ends not here yet

        // ---- settlers' jobs: a work object's owner is the NPC working it (crops: owner = Marcy's base) ----
        std::unordered_map<uint32_t, uint32_t> g_workOwner;          // work object FormID -> owner FormID last seen

        bool WireEnds(RE::TESObjectREFR* w, RE::ExtraPowerLinks::Element& a, RE::ExtraPowerLinks::Element& b)
        {
            auto* pl = w && w->extraList ? w->extraList->GetByType<RE::ExtraPowerLinks>() : nullptr;
            if (!pl || pl->powerLinks.size() != 2) return false;
            a = pl->powerLinks[0];
            b = pl->powerLinks[1];
            return true;
        }

        bool WireShape(RE::TESObjectREFR* w, RE::NiPoint3& half, float& slack, float& thickness)
        {
            auto* sp = w && w->extraList ? w->extraList->GetByType<RE::ExtraBendableSplineParams>() : nullptr;
            if (!sp) return false;
            const auto* f = reinterpret_cast<const float*>(&sp->data);   // slack, thickness, half extents
            slack = f[0];
            thickness = f[1];
            half = { f[2], f[3], f[4] };
            return true;
        }

        bool IsWire(RE::TESObjectREFR* r)
        {
            auto* b = r ? r->GetObjectReference() : nullptr;
            return b && b->GetFormType() == RE::ENUM_FORM_ID::kBNDS;
        }

        void ScanWires(bool baseline);
        void ScanWorkers(bool baseline);
        bool ApplyWire(const CoopWirePlacedMsg& m);

        void Scan(bool baseline)
        {
            ScanPluginRefs(baseline);
            ScanWires(baseline);
            ScanWorkers(baseline);
            auto* kw = ItemKeyword();
            if (!kw) return;
            // New objects: built just now (menu open) or simply loaded (baseline, not shared).
            ForEachBuilt([&](RE::TESObjectREFR* ref) {
                const uint32_t fid = ref->GetFormID();
                auto it = g_tracked.find(fid);
                if (it == g_tracked.end()) {
                    // A copy of someone else's build still being placed: not ours to announce.
                    if (auto* b = ref->GetObjectReference(); b && std::any_of(g_pending.begin(), g_pending.end(),
                            [&](const Pending& p) { return p.base == b->GetFormID(); })) return;
                    Tracked t{};
                    Remember(t, ref);
                    if (!baseline) {
                        auto* base = ref->GetObjectReference();
                        auto* bench = ref->GetLinkedRef(kw);
                        // The workbench must be a plugin reference to mean the same thing everywhere.
                        if (base && bench && bench->GetFormID() < kDynamicFormIdStart) {
                            t.key = { MyKey(), fid };
                            t.shared = true;
                            g_byKey[t.key] = fid;
                            CoopBuildPlacedMsg msg{};
                            msg.ownerKey = MyKey();
                            msg.refFormId = fid;
                            msg.baseFormId = base->GetFormID();
                            msg.workshopFormId = bench->GetFormID();
                            msg.x = t.pos.x; msg.y = t.pos.y; msg.z = t.pos.z;
                            msg.ax = t.ang.x; msg.ay = t.ang.y; msg.az = t.ang.z;
                            Send(MessageType::CoopBuildPlaced, msg);
                            REX::INFO("[Workshop] built {:08X} as {:#x} at workbench {:#x}: sent", msg.baseFormId, fid, msg.workshopFormId);
                        }
                    }
                    g_tracked[fid] = t;
                } else if (it->second.fresh) {
                    Remember(it->second, ref);
                    it->second.fresh = false;
                } else if (it->second.shared && Moved(it->second, ref)) {
                    Remember(it->second, ref);
                    CoopBuildMovedMsg msg{};
                    msg.ownerId = it->second.key.owner;
                    msg.ownerRefFormId = it->second.key.ref;
                    msg.x = it->second.pos.x; msg.y = it->second.pos.y; msg.z = it->second.pos.z;
                    msg.ax = it->second.ang.x; msg.ay = it->second.ang.y; msg.az = it->second.ang.z;
                    Send(MessageType::CoopBuildMoved, msg);
                    REX::INFO("[Workshop] moved {:#x}: sent", fid);
                }
            });
            // Shared objects that were scrapped or stored.
            std::vector<uint32_t> gone;
            for (auto& [fid, t] : g_tracked)
                if (Gone(Lookup<RE::TESObjectREFR>(fid))) gone.push_back(fid);
            for (uint32_t fid : gone) {
                const Tracked t = g_tracked[fid];
                g_tracked.erase(fid);
                if (!t.shared) continue;
                g_byKey.erase(t.key);
                CoopBuildRemovedMsg msg{};
                msg.ownerId = t.key.owner;
                msg.ownerRefFormId = t.key.ref;
                Send(MessageType::CoopBuildRemoved, msg);
                REX::INFO("[Workshop] removed {:#x}: sent", fid);
            }
        }

        // Newest runtime reference of `base` near `where` that we don't track yet.
        RE::TESObjectREFR* FindNewCopy(RE::TESObjectREFR* where, uint32_t base)
        {
            RE::TESObjectREFR* found = nullptr;
            auto* tes = RE::TES::GetSingleton();
            if (!tes) return nullptr;
            tes->ForEachReferenceInRange(where->GetPosition(), 1000.0f, [&](RE::TESObjectREFR* ref) {
                auto* b = ref ? ref->GetObjectReference() : nullptr;
                if (b && b->GetFormID() == base && ref->GetFormID() >= kDynamicFormIdStart && !g_tracked.count(ref->GetFormID()) &&
                    !Gone(ref) && (!found || ref->GetFormID() > found->GetFormID()))
                    found = ref;
                return RE::BSContainer::ForEachResult::kContinue;
            });
            return found;
        }

        // ---- the host's world ----
        // References we disabled so this game shows the host's settlements: our own builds there
        // and plugin objects the host scrapped. Enabled again on disconnect and on every load
        // (the list is in the co-save), so they're only hidden while playing together.
        std::unordered_set<uint32_t> g_hidden;
        RE::NiPoint3 g_lastWorldReq{};
        std::chrono::steady_clock::time_point g_lastWorldReqAt{};
        std::chrono::steady_clock::time_point g_connectedAt{};
        bool g_worldReqSent = false;
        // Copies to place, a few per tick (a settlement can have hundreds of objects).
        std::deque<CoopBuildPlacedMsg> g_toPlace;

        void Hide(RE::TESObjectREFR* ref)
        {
            if (!ref || ref->IsDisabled() || ref->IsDeleted()) return;
            ref->Disable();
            g_hidden.insert(ref->GetFormID());
        }

        void UnhideAll(const char* why)
        {
            size_t n = 0;
            for (uint32_t fid : g_hidden)
                if (auto* ref = Lookup<RE::TESObjectREFR>(fid); ref && ref->IsDisabled() && !ref->IsDeleted()) {
                    ref->Enable(false);
                    ++n;
                }
            if (!g_hidden.empty()) REX::INFO("[World] {}: {} hidden reference(s) shown again", why, n);
            g_hidden.clear();
        }

        // ---- scrapping the world's own objects (trees, junk, ruins: plugin references) ----
        // Scrapping one marks it deleted. While the workshop menu is open we watch the plugin
        // references around us and tell everyone which ones went (CoopRefsHidden, targetId 0).
        std::unordered_set<uint32_t> g_pluginPresent;    // plugin refs seen standing while the menu is open
        std::unordered_set<uint32_t> g_remoteScrapped;   // removed here because another player scrapped them
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_scrapRetry;   // not loaded here yet

        bool WatchedPluginRef(RE::TESObjectREFR* ref)
        {
            return ref && ref->GetFormID() < kDynamicFormIdStart && !ref->As<RE::Actor>() && !Gone(ref);
        }

        void ScanPluginRefs(bool baseline)
        {
            auto* tes = RE::TES::GetSingleton();
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!tes || !pl) return;
            if (baseline) g_pluginPresent.clear();
            tes->ForEachReferenceInRange(pl->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* ref) {
                if (WatchedPluginRef(ref)) g_pluginPresent.insert(ref->GetFormID());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (baseline) return;
            std::vector<uint32_t> scrapped;
            for (auto it = g_pluginPresent.begin(); it != g_pluginPresent.end();) {
                auto* ref = Lookup<RE::TESObjectREFR>(*it);
                // Only "deleted" counts: a reference whose cell unloaded can't be looked up any more.
                if (ref && ref->IsDeleted() && !g_remoteScrapped.contains(*it) && !g_hidden.contains(*it)) {
                    scrapped.push_back(*it);
                    it = g_pluginPresent.erase(it);
                } else {
                    ++it;
                }
            }
            if (scrapped.empty() || !NetworkClient::GetInstance().IsConnected()) return;
            for (size_t i = 0; i < scrapped.size(); i += kMaxRefsPerMsg) {
                CoopRefsHiddenMsg h{};
                h.targetId = 0;   // everyone: scrapped live
                h.count = static_cast<uint32_t>((std::min)(kMaxRefsPerMsg, scrapped.size() - i));
                std::copy_n(scrapped.begin() + i, h.count, h.refs);
                Send(MessageType::CoopRefsHidden, h);
            }
            REX::INFO("[Workshop] scrapped {} world object(s) (first {:#x}): sent", scrapped.size(), scrapped.front());
        }

        // Another player scrapped a world object. The host's world is the real one, so the host
        // removes it for good; a guest only hides it while connected (its own save keeps it).
        bool ApplyRemoteScrap(uint32_t fid)
        {
            auto* ref = Lookup<RE::TESObjectREFR>(fid);
            if (!ref) return false;
            g_remoteScrapped.insert(fid);
            g_pluginPresent.erase(fid);
            if (F4MP::IsHost()) {
                if (!ref->IsDeleted()) {
                    ref->Disable();
                    ref->SetDelete(true);
                }
            } else {
                Hide(ref);
            }
            return true;
        }

        float Dist(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        uint32_t LocalFor(uint32_t owner, uint32_t ref)
        {
            if (owner == 0) return ref;                     // plugin reference
            if (owner == MyKey()) return g_tracked.count(ref) ? ref : 0;
            auto it = g_byKey.find({ owner, ref });
            return it != g_byKey.end() ? it->second : 0;
        }
    }

    static void PlaceCopy(const CoopBuildPlacedMsg& m);
    static void WorldTick();

    // Finish placing copies whose reference has appeared (before the scan, which would
    // otherwise take them for our own new builds).
    static void ResolvePending()
    {
        auto* kw = ItemKeyword();
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < g_pending.size();) {
            auto& p = g_pending[i];
            auto* bench = Lookup<RE::TESObjectREFR>(p.bench);
            auto* copy = bench ? FindNewCopy(bench, p.base) : nullptr;
            if (copy && kw) {
                PlaceExactly(copy->GetFormID(), p.pos, p.ang);
                // Linked to our workbench: part of our settlement, so we can move or scrap it too.
                copy->SetLinkedRef(reinterpret_cast<RE::Actor*>(bench), kw);
                g_tracked[copy->GetFormID()] = Tracked{ p.key, true, p.pos, p.ang };
                g_byKey[p.key] = copy->GetFormID();
                g_verify.push_back({ copy->GetFormID(), now + std::chrono::milliseconds(1500) });
                const auto at = copy->GetPosition();
                REX::INFO("[Workshop] player {} built {:08X}: copy {:#x} placed at {:.0f} {:.0f} {:.0f} (wanted {:.0f} {:.0f} {:.0f}), linked: {}",
                    p.key.owner, p.base, copy->GetFormID(), at.x, at.y, at.z, p.pos.x, p.pos.y, p.pos.z, copy->GetLinkedRef(kw) == bench);
            } else if (now - p.since < std::chrono::seconds(3)) {
                ++i;
                continue;
            } else {
                g_byKey.erase(p.key);
                REX::WARN("[Workshop] player {} built {:08X}: our copy did not appear", p.key.owner, p.base);
            }
            g_pending.erase(g_pending.begin() + i);
        }
    }

    static void VerifyCopies()
    {
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < g_verify.size();) {
            if (now < g_verify[i].second) { ++i; continue; }
            auto* ref = Lookup<RE::TESObjectREFR>(g_verify[i].first);
            auto* obj = ref ? ref->Get3D() : nullptr;
            const auto d = ref ? ref->GetPosition() : RE::NiPoint3{};
            const auto w = obj ? obj->world.translate : RE::NiPoint3{};
            REX::INFO("[Workshop] copy {:#x}: exists {} disabled {} deleted {} 3D {} data {:.0f} {:.0f} {:.0f} | 3D at {:.0f} {:.0f} {:.0f} | cell {:#x}",
                g_verify[i].first, ref != nullptr, ref && ref->IsDisabled(), ref && ref->IsDeleted(), obj != nullptr,
                d.x, d.y, d.z, w.x, w.y, w.z, ref && ref->GetParentCell() ? ref->GetParentCell()->GetFormID() : 0u);
            g_verify.erase(g_verify.begin() + i);
        }
    }

    // Carrying a loose prop (hold E on a cup, a can, a tire...): its position goes out ~10/s while
    // held, then for up to 4 s while it falls or rolls, so it comes to rest in the same spot.
    static void PropsTick()
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return;
        const auto now = std::chrono::steady_clock::now();
        auto held = pl->grabbedObject.get();
        uint32_t heldId = held ? held->GetFormID() : 0;
        if (heldId >= kDynamicFormIdStart) heldId = 0;   // runtime objects: different IDs per game
        static bool s_wasHeld = false;
        if (heldId && heldId != g_carried) {
            g_carried = heldId;
            REX::INFO("[Props] picked up {:#x}", heldId);
        }
        if (s_wasHeld && !heldId) {
            g_settleUntil = now + std::chrono::seconds(4);   // let go: follow the fall
            REX::INFO("[Props] let go of {:#x}", g_carried);
        }
        s_wasHeld = heldId != 0;
        if (!heldId && g_carried && now >= g_settleUntil) {
            g_carried = 0;
            return;
        }
        if (!g_carried) return;
        auto* ref = Lookup<RE::TESObjectREFR>(g_carried);
        if (!ref) { g_carried = 0; return; }
        const auto p = ref->GetPosition();
        if (std::fabs(p.x - g_carriedLast.x) + std::fabs(p.y - g_carriedLast.y) + std::fabs(p.z - g_carriedLast.z) < 0.5f && !heldId) return;
        g_carriedLast = p;
        CoopPropMovedMsg msg{};
        msg.refFormId = g_carried;
        msg.x = p.x; msg.y = p.y; msg.z = p.z;
        msg.ax = ref->data.angle.x; msg.ay = ref->data.angle.y; msg.az = ref->data.angle.z;
        msg.held = heldId ? 1 : 0;
        Send(MessageType::CoopPropMoved, msg);
    }

    void OnPropMoved(const CoopPropMovedMsg& m)
    {
        auto* ref = Lookup<RE::TESObjectREFR>(m.refFormId);
        if (!ref || Gone(ref) || !ref->Get3D() || m.refFormId == g_carried) return;   // we hold it: ours wins
        const bool first = !g_remoteHeld.count(m.refFormId);
        PlaceExactly(m.refFormId, { m.x, m.y, m.z }, { m.ax, m.ay, m.az });
        g_remoteHeld[m.refFormId] = { { m.x, m.y, m.z }, { m.ax, m.ay, m.az }, m.held != 0, std::chrono::steady_clock::now() };
        if (first) REX::INFO("[Props] player {} moves {:#x}", m.senderId, m.refFormId);
    }

    void HoldRemoteProps()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = g_remoteHeld.begin(); it != g_remoteHeld.end();) {
            // Held, and heard from in the last second (a lost "let go" mustn't pin it forever).
            if (!it->second.held || now - it->second.last > std::chrono::seconds(1)) {
                it = g_remoteHeld.erase(it);
                continue;
            }
            PlaceExactly(it->first, it->second.pos, it->second.ang);
            ++it;
        }
    }

    void Tick()
    {
        PropsTick();
        for (int i = 0; i < 8 && !g_toPlace.empty(); ++i) {
            PlaceCopy(g_toPlace.front());
            g_toPlace.pop_front();
        }
        ResolvePending();
        // Wires whose ends were copies still being placed.
        if (!g_wireRetry.empty()) {
            const auto t = std::chrono::steady_clock::now();
            for (size_t i = 0, n = g_wireRetry.size(); i < n; ++i) {
                auto [m, since] = g_wireRetry.front();
                g_wireRetry.pop_front();
                if (!ApplyWire(m) && t - since < std::chrono::seconds(10)) g_wireRetry.emplace_back(m, since);
            }
        }
        WorldTick();
        FurnitureTick();
        // Scraps of objects that weren't loaded here: apply once their cell is.
        if (!g_scrapRetry.empty()) {
            static auto s_next = std::chrono::steady_clock::time_point{};
            const auto t = std::chrono::steady_clock::now();
            if (t >= s_next) {
                s_next = t + std::chrono::seconds(2);
                for (auto it = g_scrapRetry.begin(); it != g_scrapRetry.end();)
                    it = (ApplyRemoteScrap(it->first) || t > it->second) ? g_scrapRetry.erase(it) : std::next(it);
            }
        }
        VerifyCopies();
        const auto now = std::chrono::steady_clock::now();
        const bool open = WorkshopMenuOpen();
        if (open != g_menuWasOpen) {
            size_t n = 0, linked = 0;
            if (auto* tes = RE::TES::GetSingleton(); tes && RE::PlayerCharacter::GetSingleton()) {
                tes->ForEachReferenceInRange(RE::PlayerCharacter::GetSingleton()->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* ref) {
                    if (ref && ref->GetFormID() >= kDynamicFormIdStart) { ++n; if (ItemKeyword() && ref->GetLinkedRef(ItemKeyword())) ++linked; }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            REX::INFO("[Workshop] workshop menu {} ({} runtime refs in range, {} linked to a workbench, {} tracked)",
                open ? "opened" : "closed", n, linked, g_tracked.size());
        }
        if (open && !g_menuWasOpen) {
            Scan(true);                                   // what was already there: not new builds
            g_nextScan = now + std::chrono::milliseconds(500);
        } else if ((open && now >= g_nextScan) || (!open && g_menuWasOpen)) {   // + one last pass on close
            Scan(false);
            g_nextScan = now + std::chrono::milliseconds(500);
        }
        g_menuWasOpen = open;
    }

    void OnPlaced(const CoopBuildPlacedMsg& m)
    {
        if (g_byKey.count({ m.ownerKey, m.refFormId })) return;
        g_toPlace.push_back(m);
    }

    // ---------- power wires ----------

    // A wire made as the workshop makes one: placed in the middle, its look and shape set, its ends connected
    // (that's the power grid too), then its mesh rebuilt from the shape.
    static RE::TESObjectREFR* MakeWire(RE::TESObjectREFR* a, uint32_t aLink, RE::TESObjectREFR* b, uint32_t bLink, uint32_t base,
        const RE::NiPoint3& mid, RE::NiPoint3 half, float slack, float thickness)
    {
        auto* spline = Lookup<RE::TESBoundObject>(base ? base : 0x1D971);   // BlackWireSpline01
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* cell = a ? a->GetParentCell() : nullptr;
        if (!a || !b || !spline || !dh || !cell) return nullptr;
        RE::NEW_REFR_DATA data;
        data.location = mid;
        data.object = spline;
        if (cell->IsInterior()) data.interior = cell;
        else data.world = cell->worldSpace;
        auto wire = dh->CreateReferenceAtLocation(data).get();
        if (!wire) return nullptr;
        if (wire->extraList) wire->extraList->SetBendableSplineInfo(&thickness, &slack, &half);
        RE::SplineUtils::ConnectSpline(a, static_cast<int32_t>(aLink), b, static_cast<int32_t>(bLink), wire.get());
        if (auto* tq = RE::TaskQueueInterface::GetSingleton()) tq->QueueRebuildBendableSpline(*wire, true, nullptr);
        return wire.get();
    }

    uint32_t Wire(RE::TESObjectREFR* a, RE::TESObjectREFR* b, uint32_t splineBase)
    {
        if (!a || !b) return 0;
        const auto pa = a->GetPosition(), pb = b->GetPosition();
        const RE::NiPoint3 mid{ (pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f, (pa.z + pb.z) * 0.5f };
        const RE::NiPoint3 half{ (pb.x - pa.x) * 0.5f, (pb.y - pa.y) * 0.5f, (pb.z - pa.z) * 0.5f };
        auto* w = MakeWire(a, 0, b, 0, splineBase, mid, half, 0.05f, 1.5f);
        return w ? w->GetFormID() : 0;
    }

    static void RemoveWire(uint32_t fid)
    {
        auto* w = Lookup<RE::TESObjectREFR>(fid);
        if (!w || Gone(w)) return;
        RE::SplineUtils::DisconnectSpline(*w);
        w->Disable();
        w->SetDelete(true);
    }

    namespace
    {
        void SendWire(uint32_t fid, RE::TESObjectREFR* w, const WireState& st)
        {
            RE::ExtraPowerLinks::Element ea{}, eb{};
            CoopWirePlacedMsg msg{};
            auto* ra = WireEnds(w, ea, eb) ? Lookup<RE::TESObjectREFR>(ea.formID) : nullptr;
            auto* rb = ra ? Lookup<RE::TESObjectREFR>(eb.formID) : nullptr;
            // Both ends must mean the same thing in every game (shared builds or plugin references).
            if (!rb || !SeatKey(ra, msg.aOwner, msg.aRef) || !SeatKey(rb, msg.bOwner, msg.bRef)) {
                REX::INFO("[Workshop] wire {:#x}: its ends ({:#x}, {:#x}) aren't shared, not sent", fid, ea.formID, eb.formID);
                return;
            }
            RE::NiPoint3 half;
            if (!WireShape(w, half, msg.slack, msg.thickness)) return;
            msg.ownerKey = st.key.owner;
            msg.wireRefFormId = st.key.ref;
            msg.baseFormId = w->GetObjectReference()->GetFormID();
            msg.aLink = ea.linkType;
            msg.bLink = eb.linkType;
            const auto p = w->GetPosition();
            msg.x = p.x; msg.y = p.y; msg.z = p.z;
            msg.hx = half.x; msg.hy = half.y; msg.hz = half.z;
            Send(MessageType::CoopWirePlaced, msg);
            REX::INFO("[Workshop] wire {:#x} ({:08X}/{:#x} -> {:08X}/{:#x}) sent", fid, msg.aOwner, msg.aRef, msg.bOwner, msg.bRef);
        }

        void ScanWires(bool baseline)
        {
            auto* tes = RE::TES::GetSingleton();
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!tes || !pl) return;
            std::unordered_set<uint32_t> seen;
            tes->ForEachReferenceInRange(pl->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* w) {
                if (!w || w->GetFormID() < kDynamicFormIdStart || Gone(w) || !IsWire(w)) return RE::BSContainer::ForEachResult::kContinue;
                RE::ExtraPowerLinks::Element ea{}, eb{};
                if (!WireEnds(w, ea, eb)) return RE::BSContainer::ForEachResult::kContinue;   // still being dragged out
                const uint32_t fid = w->GetFormID();
                seen.insert(fid);
                RE::NiPoint3 half;
                float slack = 0, thick = 0;
                WireShape(w, half, slack, thick);
                auto it = g_wires.find(fid);
                if (it == g_wires.end()) {
                    WireState st{ { MyKey(), fid }, !baseline, w->GetPosition(), half };
                    g_wires[fid] = st;
                    if (st.shared) { g_wireByKey[st.key] = fid; SendWire(fid, w, st); }
                } else if (it->second.shared) {
                    // An end moved: the game reshaped the wire. Sent again (the copies are made again).
                    const auto p = w->GetPosition();
                    const auto& o = it->second;
                    const float d = std::fabs(p.x - o.pos.x) + std::fabs(p.y - o.pos.y) + std::fabs(p.z - o.pos.z) +
                                    std::fabs(half.x - o.half.x) + std::fabs(half.y - o.half.y) + std::fabs(half.z - o.half.z);
                    if (d > 1.0f) {
                        it->second.pos = p;
                        it->second.half = half;
                        SendWire(fid, w, it->second);
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            // Shared wires scrapped here (ours, or a copy of someone else's): everyone scraps theirs.
            for (auto it = g_wires.begin(); it != g_wires.end();) {
                auto* w = Lookup<RE::TESObjectREFR>(it->first);
                if (seen.count(it->first) || (w && !Gone(w))) { ++it; continue; }
                if (it->second.shared && !baseline) {
                    CoopWireRemovedMsg msg{};
                    msg.ownerKey = it->second.key.owner;
                    msg.wireRefFormId = it->second.key.ref;
                    Send(MessageType::CoopWireRemoved, msg);
                    REX::INFO("[Workshop] wire {:#x} scrapped: sent", it->first);
                }
                g_wireByKey.erase(it->second.key);
                it = g_wires.erase(it);
            }
        }

        bool ApplyWire(const CoopWirePlacedMsg& m)
        {
            const Key key{ m.ownerKey, m.wireRefFormId };
            auto* a = Lookup<RE::TESObjectREFR>(LocalSeat(m.aOwner, m.aRef));
            auto* b = Lookup<RE::TESObjectREFR>(LocalSeat(m.bOwner, m.bRef));
            if (!a || !b || Gone(a) || Gone(b)) return false;
            // Reshaped over there (an end moved): made again with the new shape.
            if (auto it = g_wireByKey.find(key); it != g_wireByKey.end()) {
                RemoveWire(it->second);
                g_wires.erase(it->second);
                g_wireByKey.erase(it);
            }
            const RE::NiPoint3 mid{ m.x, m.y, m.z }, half{ m.hx, m.hy, m.hz };
            auto* w = MakeWire(a, m.aLink, b, m.bLink, m.baseFormId, mid, half, m.slack, m.thickness);
            if (!w) return false;
            g_wires[w->GetFormID()] = WireState{ key, true, mid, half };
            g_wireByKey[key] = w->GetFormID();
            REX::INFO("[Workshop] player {}'s wire {:#x} -> ours {:#x} between {:#x} and {:#x}", m.senderId, m.wireRefFormId,
                w->GetFormID(), a->GetFormID(), b->GetFormID());
            return true;
        }

        // ---------- settlers' jobs ----------

        RE::Actor* WorkerFor(RE::TESForm* owner, const RE::NiPoint3& at)
        {
            if (!owner) return nullptr;
            if (auto* a = owner->As<RE::Actor>()) return a;
            if (owner->GetFormType() != RE::ENUM_FORM_ID::kNPC_) return nullptr;
            RE::Actor* found = nullptr;
            if (auto* tes = RE::TES::GetSingleton())
                tes->ForEachReferenceInRange(at, kScanRadius, [&](RE::TESObjectREFR* r) {
                    auto* a = r ? r->As<RE::Actor>() : nullptr;
                    if (a && !a->IsDead(false) && a->GetNPC() == owner) { found = a; return RE::BSContainer::ForEachResult::kStop; }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            return found;
        }

        void ScanWorkers(bool baseline)
        {
            auto* tes = RE::TES::GetSingleton();
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* kw = ItemKeyword();
            if (!tes || !pl || !kw) return;
            tes->ForEachReferenceInRange(pl->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* r) {
                if (!r || Gone(r) || r->As<RE::Actor>() || IsWire(r) || !r->GetLinkedRef(kw)) return RE::BSContainer::ForEachResult::kContinue;
                auto* owner = r->GetOwner();
                const uint32_t ownerId = owner ? owner->GetFormID() : 0;
                auto [it, inserted] = g_workOwner.try_emplace(r->GetFormID(), ownerId);
                if (inserted || it->second == ownerId) return RE::BSContainer::ForEachResult::kContinue;
                it->second = ownerId;
                if (baseline) return RE::BSContainer::ForEachResult::kContinue;
                CoopWorkerAssignedMsg msg{};
                auto* npc = WorkerFor(owner, r->GetPosition());
                if (!npc || !SeatKey(r, msg.objOwner, msg.objRef)) return RE::BSContainer::ForEachResult::kContinue;
                msg.npcFormId = npc->GetFormID();
                Send(MessageType::CoopWorkerAssigned, msg);
                REX::INFO("[Workshop] {:#x} now works {:#x}: sent", msg.npcFormId, r->GetFormID());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (g_workOwner.size() > 8192) g_workOwner.clear();
        }
    }

    void OnWirePlaced(const CoopWirePlacedMsg& m)
    {
        if (!ApplyWire(m)) {
            g_wireRetry.emplace_back(m, std::chrono::steady_clock::now());   // its ends are copies still being placed
            if (g_wireRetry.size() > 256) g_wireRetry.pop_front();
        }
    }

    void OnWireRemoved(const CoopWireRemovedMsg& m)
    {
        const Key key{ m.ownerKey, m.wireRefFormId };
        uint32_t fid = 0;
        if (auto it = g_wireByKey.find(key); it != g_wireByKey.end()) { fid = it->second; g_wireByKey.erase(it); }
        if (!fid) return;
        g_wires.erase(fid);
        RemoveWire(fid);
        REX::INFO("[Workshop] player {} scrapped wire {:#x}", m.senderId, fid);
    }

    bool AssignWorker(RE::Actor* a_npc, RE::TESObjectREFR* a_obj)
    {
        // The game's own job assignment (WorkshopParent's script): ownership, packages, the settlement's numbers.
        auto* quest = RE::TESForm::GetFormByID(0x2058E);   // WorkshopParent
        auto* game = RE::GameVM::GetSingleton();
        auto vm = game ? game->GetVM() : nullptr;
        if (!vm || !quest || !a_npc || !a_obj) return false;
        const auto& handles = vm->GetObjectHandlePolicy();
        const auto handle = handles.GetHandleForObject(static_cast<std::uint32_t>(quest->GetFormType()), static_cast<const void*>(quest));
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        if (handle == handles.EmptyHandle() || !vm->FindBoundObject(handle, "WorkshopParentScript", false, object, false) || !object) return false;
        return vm->DispatchMethodCall(object, "AssignActorToObjectPUBLIC", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
            a_out = RE::BSScript::detail::PackVariables(a_npc, a_obj, false);
            return true;
        }, nullptr);
    }

    void OnWorkerAssigned(const CoopWorkerAssignedMsg& m)
    {
        auto* obj = Lookup<RE::TESObjectREFR>(LocalSeat(m.objOwner, m.objRef));
        auto* npc = DynamicActors::ResolveNpc(m.senderId, m.npcFormId);
        if (!obj || !npc) {
            REX::INFO("[Workshop] player {}: {:#x} works {:08X}/{:#x} - not here", m.senderId, m.npcFormId, m.objOwner, m.objRef);
            return;
        }
        // Known as this owner already: our own scan doesn't send it back.
        if (auto* base = npc->GetNPC()) g_workOwner[obj->GetFormID()] = base->GetFormID();
        const bool ok = AssignWorker(npc, obj);
        REX::INFO("[Workshop] player {}: {:#x} now works {:#x} -> {}", m.senderId, npc->GetFormID(), obj->GetFormID(), ok);
    }

    static void PlaceCopy(const CoopBuildPlacedMsg& m)
    {
        const Key key{ m.ownerKey, m.refFormId };
        if (g_byKey.count(key)) return;
        auto* bench = Lookup<RE::TESObjectREFR>(m.workshopFormId);
        auto* base = RE::TESForm::GetFormByID(m.baseFormId);
        auto* kw = ItemKeyword();
        if (!bench || !bench->Get3D() || !base || !kw) {
            REX::INFO("[Workshop] player {} built {:08X} at workbench {:#x}: not loaded here, skipped", m.senderId, m.baseFormId, m.workshopFormId);
            return;
        }
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "%X.placeatme %X 1", m.workshopFormId, m.baseFormId);
        RE::Console::ExecuteCommand(cmd);
        g_pending.push_back({ key, m.baseFormId, m.workshopFormId, { m.x, m.y, m.z }, { m.ax, m.ay, m.az }, std::chrono::steady_clock::now() });
        g_byKey[key] = 0;   // claimed: a repeat of this message must not place a second one
    }

    void OnMoved(const CoopBuildMovedMsg& m)
    {
        const uint32_t fid = LocalFor(m.ownerId, m.ownerRefFormId);
        auto it = g_tracked.find(fid);
        if (!fid || it == g_tracked.end()) return;
        const RE::NiPoint3 p{ m.x, m.y, m.z }, a{ m.ax, m.ay, m.az };
        PlaceExactly(fid, p, a);
        it->second.pos = p;
        it->second.ang = a;
        REX::INFO("[Workshop] player {} moved {:#x}", m.senderId, fid);
    }

    void OnRemoved(const CoopBuildRemovedMsg& m)
    {
        const uint32_t fid = LocalFor(m.ownerId, m.ownerRefFormId);
        auto it = g_tracked.find(fid);
        if (!fid || it == g_tracked.end()) return;
        g_byKey.erase(it->second.key);
        g_tracked.erase(it);
        if (auto* ref = Lookup<RE::TESObjectREFR>(fid); ref && !Gone(ref)) {
            ref->Disable();
            ref->SetDelete(true);
        }
        REX::INFO("[Workshop] player {} scrapped {:#x}", m.senderId, fid);
    }

    // The scripts attached to a reference (not the plain ObjectReference type): a quest's button, switch or lever
    // listens with OnActivate and usually checks that the player did it.
    std::string ScriptsOn(RE::TESObjectREFR* a_ref)
    {
        std::string names;
        auto* game = RE::GameVM::GetSingleton();
        auto vm = game ? game->GetVM() : nullptr;
        if (!vm || !a_ref) return names;
        const auto& handles = vm->GetObjectHandlePolicy();
        const auto handle = handles.GetHandleForObject(static_cast<std::uint32_t>(a_ref->GetFormType()), static_cast<const void*>(a_ref));
        if (handle == handles.EmptyHandle()) return names;
        vm->ForEachBoundObject(handle, [&](RE::BSScript::Object* a_obj) {
            const auto* info = a_obj ? a_obj->GetTypeInfo() : nullptr;
            if (info && info->name != "ObjectReference") names += std::string(names.empty() ? "" : ", ") + info->name.c_str();
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return names;
    }

    void OnActivated(const CoopRefActivatedMsg& m)
    {
        const uint32_t fid = LocalFor(m.ownerId, m.ownerRefFormId);
        auto* ref = Lookup<RE::TESObjectREFR>(fid);
        if (!ref || Gone(ref) || !ref->Get3D()) return;
        // The sender's body does it on our screen (as if they pressed E). Without a body only doors
        // fall back to our player: an activator could be a quest object that reacts to the player.
        RE::TESObjectREFR* by = nullptr;
        for (auto& [pid, bodyId] : GetBodies())
            if (pid == m.senderId) by = Lookup<RE::TESObjectREFR>(bodyId);
        auto* base = ref->GetObjectReference();
        if (!by && base && base->GetFormType() == RE::ENUM_FORM_ID::kDOOR) by = RE::PlayerCharacter::GetSingleton();
        // A scripted button/switch/lever pressed by a guest: in the host's game (the story's) the host's player
        // presses it, or a script checking "was it the player?" ignored the guest's body and the vault door,
        // generator or quest stage it drives never moved for anyone. Guests' games only show the body doing it:
        // the host's game runs the script once and its stages come back through quest sync.
        // Only with the host nearby: a script that moves whoever pressed it (an elevator, a lift) mustn't pull the
        // host away from across the map.
        std::string scripts;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        const bool hostNear = pl && pl->GetParentCell() && ref->GetParentCell() &&
                              pl->GetParentCell()->IsInterior() == ref->GetParentCell()->IsInterior() &&
                              pl->GetPosition().GetDistance(ref->GetPosition()) < 3000.0f;
        // Not for scripts that hand out items.
        auto givesItems = [](const std::string& a_names) {
            static const char* kGivers[] = { "bobbypin", "loot", "item", "give", "pickup", "container", "reward", "vendor" };
            std::string low(a_names);
            for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return std::any_of(std::begin(kGivers), std::end(kGivers), [&](const char* k) { return low.find(k) != std::string::npos; });
        };
        if (F4MP::IsHost() && hostNear && base && base->GetFormType() == RE::ENUM_FORM_ID::kACTI && !(scripts = ScriptsOn(ref)).empty() &&
            !givesItems(scripts))
            by = pl;
        else
            scripts.clear();
        if (!by) return;
        g_applying = true;
        ref->ActivateRef(by, nullptr, 1, false, false, false);
        g_applying = false;
        REX::INFO("[Workshop] player {} activated {:#x}{}{}", m.senderId, fid, scripts.empty() ? "" : " as our player, scripts: ", scripts);
    }

    void OnLocalActivate(RE::TESObjectREFR* target)
    {
        if (g_applying || !target) return;
        auto* base = target->GetObjectReference();
        if (!base) return;
        const auto type = base->GetFormType();
        CoopRefActivatedMsg msg{};
        if (auto it = g_tracked.find(target->GetFormID()); it != g_tracked.end() && it->second.shared) {
            // Built objects: doors, lights, switches. Not furniture/containers/workbenches (those
            // would sit the other player's body down or open menus).
            if (type != RE::ENUM_FORM_ID::kDOOR && type != RE::ENUM_FORM_ID::kACTI && type != RE::ENUM_FORM_ID::kLIGH) return;
            msg.ownerId = it->second.key.owner;
            msg.ownerRefFormId = it->second.key.ref;
        } else if (target->GetFormID() < kDynamicFormIdStart &&
                   (type == RE::ENUM_FORM_ID::kDOOR || type == RE::ENUM_FORM_ID::kACTI || type == RE::ENUM_FORM_ID::kLIGH)) {
            // Plugin doors, radios, lamps, switches. Never load doors (they'd move the other
            // player to another area).
            if (target->extraList && target->extraList->HasType(RE::EXTRA_DATA_TYPE::kTeleport)) return;
            msg.ownerId = 0;
            msg.ownerRefFormId = target->GetFormID();
        } else {
            return;
        }
        Send(MessageType::CoopRefActivated, msg);
        REX::INFO("[Workshop] activated {:#x}: sent", target->GetFormID());
    }

    // ---- furniture ----
    namespace
    {
        struct BodySeat { uint32_t furn = 0; uint32_t marker = 0; bool onIt = false; int tries = 0;
                          std::chrono::steady_clock::time_point nextTry{}; };
        std::unordered_map<uint32_t, BodySeat> g_bodySeat;   // playerId -> our furniture their body uses
        CoopSeatMsg g_mySeat{};                               // what we last sent
        std::chrono::steady_clock::time_point g_mySeatSent{};

        RE::Actor* BodyOf(uint32_t playerId)
        {
            for (auto& [pid, bodyId] : GetBodies())
                if (pid == playerId) return Lookup<RE::Actor>(bodyId);
            return nullptr;
        }

        // Put the body on its seat (the furniture places and animates it); until that works, hold it still
        // where its player sits. Retried a few times: the
        // furniture's 3D may still be loading.
        void ApplyBodySeat(uint32_t pid, BodySeat& s)
        {
            auto* body = BodyOf(pid);
            auto* furn = Lookup<RE::TESObjectREFR>(s.furn);
            if (!body || !furn || Gone(furn) || !furn->Get3D()) return;
            if (Activity::SeatOf(body) == furn) {
                if (!s.onIt) REX::INFO("[Furniture] body of player {} is on {:#x} (marker {})", pid, s.furn, s.marker);
                s.onIt = true;
                return;
            }
            s.onIt = false;
            GraphOverride::Set(body, 0.0f, 0.0f);   // still while not on it
            const auto now = std::chrono::steady_clock::now();
            if (s.tries >= 5 || now < s.nextTry) return;
            ++s.tries;
            s.nextTry = now + std::chrono::seconds(1);
            const bool ok = Activity::Seat(body, furn, s.marker);
            REX::INFO("[Furniture] body of player {}: onto {:#x} marker {} -> {} (try {})", pid, s.furn, s.marker, ok, s.tries);
        }
    }

    bool BuildKey(RE::TESObjectREFR* ref, uint32_t& owner, uint32_t& key)
    {
        auto it = ref ? g_tracked.find(ref->GetFormID()) : g_tracked.end();
        if (it == g_tracked.end() || !it->second.shared) return false;
        owner = it->second.key.owner;
        key = it->second.key.ref;
        return true;
    }

    uint32_t LocalBuild(uint32_t owner, uint32_t key) { return owner && key ? LocalFor(owner, key) : 0; }

    bool SeatKey(RE::TESObjectREFR* furn, uint32_t& owner, uint32_t& ref)
    {
        owner = ref = 0;
        if (!furn) return false;
        if (BuildKey(furn, owner, ref)) return true;
        if (furn->GetFormID() < kDynamicFormIdStart) { ref = furn->GetFormID(); return true; }   // a plugin chair/bed/workbench
        return false;
    }

    uint32_t LocalSeat(uint32_t owner, uint32_t ref) { return ref ? LocalFor(owner, ref) : 0; }

    void FurnitureTick()
    {
        // Bodies first: keep them on their seats (a body respawned while its player sits is seated again).
        for (auto& [pid, s] : g_bodySeat) ApplyBodySeat(pid, s);

        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return;
        uint32_t marker = 0;
        // In power armor the frame is "furniture" too, but that has its own message (sync/Activity).
        RE::TESObjectREFR* furn = RE::PowerArmor::PlayerInPowerArmor() ? nullptr : Activity::SeatOf(pl, &marker);
        CoopSeatMsg seat{};
        if (furn && SeatKey(furn, seat.ownerId, seat.ownerRefFormId)) seat.marker = marker;
        const auto now = std::chrono::steady_clock::now();
        const bool same = seat.ownerId == g_mySeat.ownerId && seat.ownerRefFormId == g_mySeat.ownerRefFormId && seat.marker == g_mySeat.marker;
        // On change, and every 5 s while seated (someone who arrives later sees it too).
        if (same && (seat.ownerRefFormId == 0 || now - g_mySeatSent < std::chrono::seconds(5))) return;
        if (!same) REX::INFO("[Furniture] {} {:#x} (marker {}): sent", seat.ownerRefFormId ? "using" : "left",
            seat.ownerRefFormId ? seat.ownerRefFormId : g_mySeat.ownerRefFormId, seat.marker);
        g_mySeat = seat;
        g_mySeatSent = now;
        Send(MessageType::CoopFurniture, seat);
    }

    void OnFurniture(const CoopSeatMsg& m)
    {
        const uint32_t fid = m.ownerRefFormId ? LocalFor(m.ownerId, m.ownerRefFormId) : 0;
        auto it = g_bodySeat.find(m.senderId);
        const uint32_t current = it != g_bodySeat.end() ? it->second.furn : 0;
        if (fid == current && (!fid || it->second.marker == m.marker)) return;
        if (current) {
            // Got up (or moved to another seat): the position sync takes over again.
            g_bodySeat.erase(m.senderId);
            if (auto* body = BodyOf(m.senderId)) Activity::Unseat(body);
            REX::INFO("[Furniture] player {} got up from {:#x}", m.senderId, current);
        }
        if (fid) {
            auto* furn = Lookup<RE::TESObjectREFR>(fid);
            if (!furn || Gone(furn)) return;
            BodySeat s{};
            s.furn = fid;
            s.marker = m.marker;
            g_bodySeat[m.senderId] = s;
            REX::INFO("[Furniture] player {} uses {:#x} marker {}", m.senderId, fid, m.marker);
            ApplyBodySeat(m.senderId, g_bodySeat[m.senderId]);
        }
    }

    bool BodySeated(uint32_t playerId) { return g_bodySeat.contains(playerId); }

    // Disconnected: session state only. Shared builds stay linked - they belong to the save.
    void Reset()
    {
        UnhideAll("disconnected");
        g_bodySeat.clear();
        g_mySeat = {};
        g_mySeatSent = {};
        g_toPlace.clear();
        g_worldReqSent = false;
        g_connectedAt = {};
        g_pending.clear();
        g_wireRetry.clear();
        g_carried = 0;
        g_remoteHeld.clear();
        g_menuWasOpen = false;
    }

    // ---------- co-save: shared builds stay linked across restarts ----------
    // ---------- the host's world ----------
    // A joiner asks the host for its world around them on arrival and whenever they've moved on
    // to another area. The host announces every object built there (also builds from before
    // this session, so they're shared from now on) and the plugin objects it scrapped; the
    // joiner hides its own builds there and the scrapped objects, and gets copies of the host's.
    static void WorldTick()
    {
        auto& net = NetworkClient::GetInstance();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!net.IsConnected() || !pl || F4MP::IsHost()) return;
        const auto now = std::chrono::steady_clock::now();
        if (g_connectedAt == std::chrono::steady_clock::time_point{}) g_connectedAt = now;
        if (now - g_connectedAt < std::chrono::seconds(8)) return;   // travelling to the host first
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->GetMenuOpen("LoadingMenu")) return;
        const RE::NiPoint3 here = pl->GetPosition();
        if (g_worldReqSent && (Dist(here, g_lastWorldReq) < 6000.0f || now - g_lastWorldReqAt < std::chrono::seconds(20))) return;
        CoopWorldReqMsg msg{};
        msg.x = here.x; msg.y = here.y; msg.z = here.z;
        msg.space = Authority::LocalSpace();
        Send(MessageType::CoopWorldReq, msg);
        g_worldReqSent = true;
        g_lastWorldReq = here;
        g_lastWorldReqAt = now;
        REX::INFO("[World] asked the host for its world around {:.0f} {:.0f} {:.0f}", here.x, here.y, here.z);
    }

    void OnWorldReq(const CoopWorldReqMsg& m)
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* tes = RE::TES::GetSingleton();
        auto* kw = ItemKeyword();
        if (!F4MP::IsHost() || !pl || !tes || !kw) return;
        if (m.space != Authority::LocalSpace()) return;   // not where we are: we don't have it loaded
        const RE::NiPoint3 at{ m.x, m.y, m.z };
        if (Dist(at, pl->GetPosition()) > kScanRadius) return;
        const float radius = kScanRadius;

        std::vector<CoopBuildPlacedMsg> builds;
        ForEachBuilt([&](RE::TESObjectREFR* ref) {
            if (Dist(ref->GetPosition(), at) > radius) return;
            auto* base = ref->GetObjectReference();
            auto* bench = ref->GetLinkedRef(kw);
            if (!base || !bench || bench->GetFormID() >= kDynamicFormIdStart) return;
            const uint32_t fid = ref->GetFormID();
            auto& t = g_tracked[fid];
            if (t.shared && t.key.owner != MyKey()) return;   // a copy of someone else's: they have it
            if (!t.shared) {                                   // an old build of ours: shared from now on
                Remember(t, ref);
                t.key = { MyKey(), fid };
                t.shared = true;
                g_byKey[t.key] = fid;
            }
            CoopBuildPlacedMsg b{};
            b.ownerKey = MyKey();
            b.refFormId = fid;
            b.baseFormId = base->GetFormID();
            b.workshopFormId = bench->GetFormID();
            b.x = t.pos.x; b.y = t.pos.y; b.z = t.pos.z;
            b.ax = t.ang.x; b.ay = t.ang.y; b.az = t.ang.z;
            builds.push_back(b);
        });
        std::vector<uint32_t> scrapped;
        tes->ForEachReferenceInRange(at, radius, [&](RE::TESObjectREFR* ref) {
            if (ref && ref->GetFormID() < kDynamicFormIdStart && ref->IsDeleted()) scrapped.push_back(ref->GetFormID());
            return RE::BSContainer::ForEachResult::kContinue;
        });

        CoopWorldSnapshotMsg head{};
        head.targetId = m.senderId;
        head.x = m.x; head.y = m.y; head.z = m.z;
        head.radius = radius;
        head.builds = static_cast<uint32_t>(builds.size());
        head.hidden = static_cast<uint32_t>(scrapped.size());
        Send(MessageType::CoopWorldSnapshot, head);
        for (auto& b : builds) Send(MessageType::CoopBuildPlaced, b);
        for (size_t i = 0; i < scrapped.size(); i += kMaxRefsPerMsg) {
            CoopRefsHiddenMsg h{};
            h.targetId = m.senderId;
            h.count = static_cast<uint32_t>((std::min)(kMaxRefsPerMsg, scrapped.size() - i));
            std::copy_n(scrapped.begin() + i, h.count, h.refs);
            Send(MessageType::CoopRefsHidden, h);
        }
        REX::INFO("[World] player {} asked for our world: {} build(s), {} scrapped reference(s) sent", m.senderId,
            builds.size(), scrapped.size());
    }

    void OnWorldSnapshot(const CoopWorldSnapshotMsg& m)
    {
        if (m.targetId != NetworkClient::GetInstance().GetPlayerId() || F4MP::IsHost()) return;
        const RE::NiPoint3 at{ m.x, m.y, m.z };
        size_t n = 0;
        ForEachBuilt([&](RE::TESObjectREFR* ref) {
            if (Dist(ref->GetPosition(), at) > m.radius) return;
            auto it = g_tracked.find(ref->GetFormID());
            if (it != g_tracked.end() && it->second.shared) return;   // shared with the host: they have it too
            Hide(ref);
            ++n;
        });
        REX::INFO("[World] the host's world here: {} build(s) and {} scrapped reference(s) coming; {} of our own build(s) hidden",
            m.builds, m.hidden, n);
    }

    void OnRefsHidden(const CoopRefsHiddenMsg& m)
    {
        if (m.targetId == 0) {   // scrapped live by another player
            size_t n = 0, later = 0;
            const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(30);
            for (uint32_t i = 0; i < (std::min)(m.count, static_cast<uint32_t>(kMaxRefsPerMsg)); ++i) {
                if (ApplyRemoteScrap(m.refs[i])) ++n;
                else { g_scrapRetry[m.refs[i]] = until; ++later; }   // its cell isn't loaded here
            }
            REX::INFO("[Workshop] player {} scrapped {} world object(s): {} removed here, {} when loaded", m.senderId,
                m.count, n, later);
            return;
        }
        if (m.targetId != NetworkClient::GetInstance().GetPlayerId() || F4MP::IsHost()) return;
        size_t n = 0;
        for (uint32_t i = 0; i < (std::min)(m.count, static_cast<uint32_t>(kMaxRefsPerMsg)); ++i)
            if (auto* ref = Lookup<RE::TESObjectREFR>(m.refs[i]); ref && !ref->IsDisabled()) { Hide(ref); ++n; }
        if (n) REX::INFO("[World] hid {} object(s) the host scrapped", n);
    }

    void HideRef(RE::TESObjectREFR* ref) { Hide(ref); }

    void OnGameLoaded()
    {
        UnhideAll("game loaded");   // a save made while connected: show our own world again
    }

    // Record 'BLDS' v1: count, then (our ref, owner key, owner's ref) per shared build. Kept in the
    // save's .f4se file, so loading an older save brings back the links that save had.
    namespace
    {
        constexpr uint32_t kRecordBuilds = 'BLDS';
        constexpr uint32_t kRecordHidden = 'HIDE';

        void OnSave(const F4SE::SerializationInterface* a_intfc)
        {
            std::vector<std::array<uint32_t, 3>> rows;
            for (auto& [fid, t] : g_tracked)
                if (t.shared && t.key.owner && t.key.ref) rows.push_back({ fid, t.key.owner, t.key.ref });
            if (!a_intfc->OpenRecord(kRecordBuilds, 1)) return;
            const uint32_t n = static_cast<uint32_t>(rows.size());
            a_intfc->WriteRecordData(n);
            for (auto& r : rows) a_intfc->WriteRecordData(r.data(), sizeof(uint32_t) * 3);
            REX::INFO("[Workshop] saved {} shared build link(s)", n);
            // References hidden to show the host's world: shown again when this save is loaded.
            if (!g_hidden.empty() && a_intfc->OpenRecord(kRecordHidden, 1)) {
                const uint32_t h = static_cast<uint32_t>(g_hidden.size());
                a_intfc->WriteRecordData(h);
                for (uint32_t fid : g_hidden) a_intfc->WriteRecordData(fid);
            }
        }

        void OnRevert(const F4SE::SerializationInterface*)
        {
            g_hidden.clear();
            g_tracked.clear();
            g_byKey.clear();
            g_pending.clear();
        }

        void OnLoad(const F4SE::SerializationInterface* a_intfc)
        {
            uint32_t type = 0, version = 0, length = 0;
            size_t loaded = 0;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type == kRecordHidden && version == 1) {
                    uint32_t n = 0;
                    a_intfc->ReadRecordData(n);
                    for (uint32_t i = 0; i < n; ++i) {
                        uint32_t fid = 0;
                        if (a_intfc->ReadRecordData(fid) != sizeof(fid)) break;
                        if (auto resolved = a_intfc->ResolveFormID(fid)) g_hidden.insert(*resolved);
                    }
                    continue;
                }
                if (type != kRecordBuilds || version != 1) continue;
                uint32_t n = 0;
                a_intfc->ReadRecordData(n);
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t r[3]{};
                    if (a_intfc->ReadRecordData(r, sizeof(r)) != sizeof(r)) break;
                    Tracked t{};
                    t.key = { r[1], r[2] };
                    t.shared = true;
                    t.fresh = true;
                    g_tracked[r[0]] = t;
                    g_byKey[t.key] = r[0];
                    ++loaded;
                }
            }
            REX::INFO("[Workshop] loaded {} shared build link(s) from the save", loaded);
        }
    }

    void RegisterSerialization()
    {
        auto* ser = F4SE::GetSerializationInterface();
        if (!ser) {
            REX::WARN("[Workshop] no F4SE serialization: build links won't survive a restart");
            return;
        }
        ser->SetUniqueID('F4MP');
        ser->SetSaveCallback(OnSave);
        ser->SetRevertCallback(OnRevert);
        ser->SetLoadCallback(OnLoad);
        MyKey();   // make sure the permanent key exists
    }
}
