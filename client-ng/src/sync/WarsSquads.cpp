// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Wasteland Wars A-Life, the game's side: the server simulates the Commonwealth's squads (server/src/Wars/ALife);
// the ones near this player are sent here to be real. This game spawns their NPCs (the faction's own leveled
// lists) on a spot where the game itself put NPCs at that place (on the ground: a map marker can sit on a roof),
// gives each member the squad's health and reports back who's still standing and where they are. The server
// keeps the squad's plans and says what it does next: camp at a spot (the game's sandbox: sitting, eating,
// sleeping, using workstations around it) or walk to the next place. When the server takes the squad back (the
// player moved away) its NPCs are removed and it carries on in the simulation. Deaths here are deaths in the
// server's world. In a Wars world the game's own placed enemies are removed: the server's squads take their spots.

#include "pch.h"
#include "sync/WarsSquads.h"
#include "sync/Wars.h"
#include "sync/WarsCamps.h"
#include "sync/WarsClaims.h"
#include "sync/WarsTeams.h"
#include "sync/WarsDeath.h"
#include "sync/WarsVoice.h"
#include "network/NetworkClient.h"

#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace F4MP::Wars::Squads
{
    using namespace Network;

    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kCommonwealth = 0x0000003C;
        constexpr uint32_t kXMarker = 0x0000003B;

        // What each A-Life faction spawns as (Wars::Faction order, server/src/Wars/ALife.h).
        constexpr uint32_t kSpawnForm[] = {
            0x00022E48,   // raider: LCharRaider
            0x000FD39A,   // gunner: LCharGunner
            0x0023D8C5,   // mutant: LCharSupermutantMixedWeapons
            0x000758AE,   // feral: LCharFeralGhoul
            0x0019DDD8,   // mirelurk: LCharMirelurkAll
            0x0007533D,   // deathclaw: LCharDeathclaw
            0x000342C8,   // creature: LCharMolerat
            0x000E6B42,   // synth: LCharSynth
            0x00106B08,   // robot: LCharProtectron
            0x000295A1,   // trader: CaravanGuardTemplate
            0x00076A03,   // brotherhood: LCharBoSSoldier
            0x001BF0C2,   // minutemen: LCharMinutemen
            0x00061BE7,   // railroad: RailroadHQGenericTemplate00 (the game has no Railroad leveled list)
        };

        // The game's own sandbox package around a linked reference (LinkCustom01 = the squad's XMarker): sit, eat,
        // sleep, use furniture and workstations, wander. A sandbox first walks its NPC to its place, so a squad on
        // the move follows its marker, moved ahead along the way (breadcrumbs; the game's travel-to-linked-ref
        // package was picked even with no link set and froze the squad, testing).
        constexpr uint32_t kLinkCustom01 = 0x0005D5E6;
        constexpr uint32_t kSandboxLink01 = 0x000286E8;    // SandboxLinkCustom01512WithWandering
        constexpr float kStep = 2500.0f;                   // how far ahead the marker goes
        // On the move each member also gets the game's jog-to-LinkCustom01 package as its own (an actor's created
        // package, as dialogue gets): the shared sandbox walks, ~60 u/s. Camping drops it (evp).
        constexpr uint32_t kTravelJog01 = 0x000E526D;      // defaultTravelToLinkCustom01Jog
        // Squads were a tight knot: they spawn around the spot, and on the move they
        // set off one by one, a column instead of a clump.
        constexpr auto kJogGap = std::chrono::seconds(2);

        // Caravans: a merchant, a pack brahmin, guards. The merchant trades from one of the game's caravan vendors'
        // stock (their own NPCs aren't in a Wars world; their vendor chests still are), picked by the squad's id.
        constexpr uint8_t kTrader = 9;                     // Wars::Faction::Trader
        constexpr uint32_t kMerchant = 0x001C4033;         // LvlMerchant
        constexpr uint32_t kPackBrahmin = 0x0014055E;      // EncBrahminPack
        struct Vendor { uint32_t faction; const char* name; };
        constexpr Vendor kVendors[] = {
            { 0x001679C8, "Carla" },                       // CaravanVendorCarlaFaction
            { 0x001679CA, "Cricket" },                     // CaravanVendorCricketFaction
            { 0x001679D1, "Doc Weathers" },                // CaravanVendorDocWeathersFaction
            { 0x001679CE, "Lucas" },                       // CaravanVendorLucasFaction
        };
        std::unordered_map<uint32_t, const Vendor*> g_merchants;   // merchant actor -> its stock
        bool g_jog = true;   // squads on the move jog (kTravelJog01)

        enum class Stage : uint8_t { Marker, FindMarker, Spawn, FindActors, Ready };

        struct Live {
            WarsSquadMsg order{};               // what the server sent
            WarsSquadGoalMsg goal{};            // what it does now
            std::vector<uint32_t> members;      // our NPCs, in the server's member order (0 = never spawned)
            uint32_t marker = 0;                // our XMarker: where it spawns, then where it lives / walks to
            RE::NiPoint3 markerAt{};            // where we last put it
            Stage stage = Stage::Marker;
            Clock::time_point stageAt{};
            std::unordered_set<uint32_t> before;   // runtime refs near the place before a placeatme
            float lastHp[kWarsSquadMax]{};         // each member's health when last seen (unloaded = as last seen)
            Clock::time_point jogAt{};             // jogging: when we last gave them the travel package
            RE::NiPoint3 jogFrom{};                // and where they were then
            bool jogged = false;                   // they were given the jog (camping then needs the sandbox put)
            std::vector<uint32_t> jogQueue;        // setting off: members still to go, one every kJogGap
            Clock::time_point jogNext{};
        };
        std::unordered_map<uint32_t, Live> g_squads;   // by squad id
        Clock::time_point g_lastPresence{}, g_lastReport{}, g_lastSweep{};
        std::unordered_set<uint32_t> g_vanillaRemoved;

        template <class T = RE::TESObjectREFR>
        T* RefOf(uint32_t a_fid)
        {
            auto* f = a_fid ? RE::TESForm::GetFormByID(a_fid) : nullptr;
            return f ? f->As<T>() : nullptr;
        }
        RE::Actor* ActorOf(uint32_t a_fid) { return RefOf<RE::Actor>(a_fid); }

        RE::ActorValueInfo* HealthAV()
        {
            auto* avs = RE::ActorValue::GetSingleton();
            return avs ? avs->health : nullptr;
        }

        void Console(const char* a_fmt, auto... a_args)
        {
            char cmd[128];
            std::snprintf(cmd, sizeof(cmd), a_fmt, a_args...);
            RE::Console::ExecuteCommand(cmd);
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

        RE::NiPoint3 AnchorPos(const Live& a_s)
        {
            if (auto* anchor = RefOf(a_s.order.anchorRef)) return anchor->GetPosition();
            return { a_s.order.x, a_s.order.y, a_s.order.z };
        }

        bool Outside(RE::PlayerCharacter* a_pl)
        {
            auto* cell = a_pl ? a_pl->parentCell : nullptr;
            return cell && cell->IsExterior() && cell->worldSpace && cell->worldSpace->GetFormID() == kCommonwealth;
        }

        void Send(MessageType a_type, const void* a_msg, uint32_t a_size)
        {
            auto& net = NetworkClient::GetInstance();
            if (net.IsConnected()) net.SendPacket(a_type, a_msg, a_size);
        }

        // ---- the game's packages on the spawned NPCs ----

        // The game's singly linked list, laid out by hand (CommonLibF4's BSSimpleList frees game-heap nodes with
        // C++ delete): the root node lives in the record, the others on the game's heap.
        struct PackNode {
            RE::TESPackage* value;
            PackNode* next;
        };

        std::string PackList(const RE::TESNPC* a_npc)
        {
            std::string out;
            for (auto* n = reinterpret_cast<const PackNode*>(&a_npc->aiPackList.listPackages); n; n = n->next)
                if (n->value) out += std::format("{:x} ", n->value->GetFormID());
            return out;
        }

        // [sandbox, what it had], once per base record. These shared vanilla records are changed in a
        // Wars game only (never saved: the game rebuilds its records from the plugins every launch), and both
        // packages act only on an NPC that has our linked references; any other NPC skips them.
        std::unordered_set<uint32_t> g_basesDone;

        void AddPackages(RE::TESNPC* a_npc)
        {
            auto* sandbox = RE::TESForm::GetFormByID<RE::TESPackage>(kSandboxLink01);
            if (!a_npc || !sandbox || !g_basesDone.insert(a_npc->GetFormID()).second) return;
            auto* root = reinterpret_cast<PackNode*>(&a_npc->aiPackList.listPackages);
            for (auto* n = root; n; n = n->next)
                if (n->value == sandbox) return;
            if (root->value) {
                auto* old = static_cast<PackNode*>(RE::malloc(sizeof(PackNode)));
                if (!old) return;
                *old = *root;
                root->next = old;
            }
            root->value = sandbox;
        }

        void MoveMarker(Live& a_s, const RE::NiPoint3& a_to)
        {
            Console("%X.setpos x %.1f", a_s.marker, a_to.x);
            Console("%X.setpos y %.1f", a_s.marker, a_to.y);
            Console("%X.setpos z %.1f", a_s.marker, a_to.z);
            a_s.markerAt = a_to;
        }

        // The next member sets off (the jog package, as Relink's comments say).
        void PumpJog(Live& a_s)
        {
            auto* jog = RE::TESForm::GetFormByID<RE::TESPackage>(kTravelJog01);
            const auto now = Clock::now();
            if (!jog || a_s.jogQueue.empty() || now < a_s.jogNext) return;
            a_s.jogNext = now + kJogGap;
            while (!a_s.jogQueue.empty()) {
                auto* a = ActorOf(a_s.jogQueue.front());
                a_s.jogQueue.erase(a_s.jogQueue.begin());
                if (!a || a->IsDead(false) || a->IsDeleted() || a->IsInCombat()) continue;
                a->PutCreatedPackage(jog, true, false, true);
                return;
            }
        }

        // Links set natively,
        // then the console's evp so they act on it now.
        void Relink(Live& a_s, bool a_travel = false)
        {
            auto* kw1 = RE::TESForm::GetFormByID<RE::BGSKeyword>(kLinkCustom01);
            auto* marker = RefOf(a_s.marker);
            if (!kw1 || !marker) return;
            auto* jog = a_travel && g_jog ? RE::TESForm::GetFormByID<RE::TESPackage>(kTravelJog01) : nullptr;
            // Camping after a jog: the jog stays the actor's own package (evp and resetai kept it: they stood in a
            // line where it ended, testing) - the sandbox becomes their own package instead.
            auto* sandbox = !a_travel && a_s.jogged ? RE::TESForm::GetFormByID<RE::TESPackage>(kSandboxLink01) : nullptr;
            a_s.jogQueue.clear();
            for (uint32_t fid : a_s.members) {
                auto* a = ActorOf(fid);
                if (!a || a->IsDead(false) || a->IsDeleted()) continue;
                AddPackages(a->GetNPC());
                a->SetLinkedRef(reinterpret_cast<RE::Actor*>(marker), kw1);
                // The console's evp runs after this call returns and would pick the sandbox again: a jog instead
                // starts at once as the actor's own package.
                if (jog && !a->IsInCombat()) {
                    a_s.jogQueue.push_back(fid);
                    a_s.jogged = true;
                } else if (sandbox) {
                    a->PutCreatedPackage(sandbox, true, false, true);
                } else {
                    Console("%X.evp", fid);
                }
            }
            a_s.jogNext = {};
            PumpJog(a_s);
        }

        // Where its living members stand (false = none loaded).
        bool Centroid(const Live& a_s, RE::NiPoint3& a_out)
        {
            RE::NiPoint3 sum{};
            int n = 0;
            for (uint32_t fid : a_s.members)
                if (auto* a = ActorOf(fid); a && !a->IsDead(false) && a->Get3D()) { sum += a->GetPosition(); ++n; }
            if (!n) return false;
            a_out = sum / static_cast<float>(n);
            return true;
        }

        // On the move: the marker kStep ahead of the squad, toward the next place (its map marker; persistent,
        // so its position is known when it isn't loaded).
        // Jogging: the travel package finds its own way on the navmesh, so the marker goes straight to the next
        // place;
        // given again when they've stopped (arrived at a roof marker's foot, or after a fight).
        void Breadcrumb(Live& a_s)
        {
            auto* goal = RefOf(a_s.goal.goalRef);
            RE::NiPoint3 at;
            if (!goal || !Centroid(a_s, at)) return;
            const auto to = goal->GetPosition();
            if (g_jog) {
                if (!a_s.jogQueue.empty()) return;   // still setting off
                const auto now = Clock::now();
                if (a_s.markerAt.GetDistance(to) > 1.0f) {
                    MoveMarker(a_s, to);
                } else if (now - a_s.jogAt < std::chrono::seconds(8) || at.GetDistance(a_s.jogFrom) > 300.0f) {
                    if (now - a_s.jogAt >= std::chrono::seconds(8)) { a_s.jogAt = now; a_s.jogFrom = at; }
                    return;   // on their way
                }
                a_s.jogAt = now;
                a_s.jogFrom = at;
                Relink(a_s, true);
                return;
            }
            const float dx = to.x - at.x, dy = to.y - at.y;
            const float dist = std::hypot(dx, dy);
            if (dist < 1.0f) return;
            const float t = std::min(1.0f, kStep / dist);
            const RE::NiPoint3 next{ at.x + dx * t, at.y + dy * t, at.z + (to.z - at.z) * t };
            if (next.GetDistance(a_s.markerAt) < 800.0f) return;   // still on their way to it
            MoveMarker(a_s, next);
            Relink(a_s, true);
        }

        // Camp: the marker on the camp spot. Travel: breadcrumbs from now on (Tick).
        void ApplyGoal(uint32_t a_id, Live& a_s)
        {
            if (!RefOf(a_s.marker)) return;
            const bool travel = a_s.goal.travel && RefOf(a_s.goal.goalRef);
            if (travel) {
                a_s.markerAt = {};   // the first breadcrumb goes out at once
                Breadcrumb(a_s);
            } else {
                if (a_s.goal.x != 0.0f || a_s.goal.y != 0.0f) MoveMarker(a_s, { a_s.goal.x, a_s.goal.y, a_s.goal.z });
                Relink(a_s);
            }
            REX::INFO("[Wars] squad #{}: {}", a_id, travel ? std::format("walking to {:#x}", a_s.goal.goalRef)
                : std::format("camping at {:.0f} {:.0f} {:.0f}", a_s.goal.x, a_s.goal.y, a_s.goal.z));
        }

        // ---- reports ----

        // Each member's health (0 = dead), where the living ones stand.
        WarsSquadMsg ReportOf(uint32_t a_id, Live& a_s, bool a_final)
        {
            WarsSquadMsg m{};
            m.squad = a_id;
            m.faction = a_s.order.faction;
            m.count = a_s.order.count;
            m.final = a_final ? 1 : 0;
            float sx = 0, sy = 0;
            int up = 0;
            auto* hp = HealthAV();
            for (int i = 0; i < m.count; ++i) {
                if (a_s.stage != Stage::Ready || i >= static_cast<int>(a_s.members.size()) || !a_s.members[i]) {
                    m.hp[i] = a_s.lastHp[i];   // not spawned (yet): as the server had it
                    continue;
                }
                // Dead only when seen dead here: a member that isn't loaded any more (we went away, a loading
                // screen) keeps its last health.
                auto* a = ActorOf(a_s.members[i]);
                if (!a || a->IsDeleted() || !a->Get3D()) { m.hp[i] = a_s.lastHp[i]; continue; }
                if (a->IsDead(false)) { m.hp[i] = a_s.lastHp[i] = 0.0f; continue; }
                const float full = hp ? a->GetPermanentActorValue(*hp) : 0.0f;
                m.hp[i] = a_s.lastHp[i] = full > 0.0f && hp ? std::clamp(a->GetActorValue(*hp) / full, 0.01f, 1.0f) : a_s.lastHp[i];
                const auto p = a->GetPosition();
                sx += p.x;
                sy += p.y;
                ++up;
            }
            if (up) { m.x = sx / up; m.y = sy / up; }
            return m;
        }

        void Remove(Live& a_s)
        {
            for (uint32_t fid : a_s.members) g_merchants.erase(fid);
            for (uint32_t fid : a_s.members)
                if (auto* a = ActorOf(fid); a && !a->IsDeleted()) {
                    if (a->IsDead(false)) continue;   // the fallen stay where they fell, with their loot
                    a->Disable();
                    a->SetWantsDelete(true);
                    a->MarkAsDeleted();
                }
            if (auto* m = RefOf(a_s.marker); m && !m->IsDeleted()) {
                m->Disable();
                m->SetWantsDelete(true);
                m->MarkAsDeleted();
            }
        }

        // ---- spawning, one step per tick ----

        // 1) an XMarker at the place's map marker; 2) found, moved onto the spawn spot; 3) the NPCs placed at the
        // XMarker = on the spot; 4) found, health set, packages and links: camping around it.
        bool StepSpawn(uint32_t a_id, Live& a_s)
        {
            const auto now = Clock::now();
            const uint32_t form = a_s.order.faction < std::size(kSpawnForm) ? kSpawnForm[a_s.order.faction] : 0;
            if (!form || !RefOf(a_s.order.anchorRef) || a_s.order.count == 0) {
                REX::WARN("[Wars] squad #{}: can't spawn (form {:#x}, place {:#x})", a_id, form, a_s.order.anchorRef);
                a_s.stage = Stage::Ready;
                return true;
            }
            const RE::NiPoint3 spot{ a_s.order.x, a_s.order.y, a_s.order.z };
            switch (a_s.stage) {
            case Stage::Marker:
                a_s.before.clear();
                ForEachRuntimeRefNear(AnchorPos(a_s), 600.0f, [&](RE::TESObjectREFR* r) { a_s.before.insert(r->GetFormID()); });
                Console("%X.placeatme %X 1", a_s.order.anchorRef, kXMarker);
                a_s.stage = Stage::FindMarker;
                a_s.stageAt = now;
                return false;
            case Stage::FindMarker: {
                ForEachRuntimeRefNear(AnchorPos(a_s), 600.0f, [&](RE::TESObjectREFR* r) {
                    auto* base = r->GetObjectReference();
                    if (!a_s.marker && base && base->GetFormID() == kXMarker && !a_s.before.contains(r->GetFormID())) a_s.marker = r->GetFormID();
                });
                if (!a_s.marker) {
                    if (now - a_s.stageAt > std::chrono::seconds(3)) { REX::WARN("[Wars] squad #{}: no marker", a_id); a_s.stage = Stage::Ready; return true; }
                    return false;
                }
                MoveMarker(a_s, spot);
                a_s.stage = Stage::Spawn;
                return false;
            }
            case Stage::Spawn:
                a_s.before.clear();
                ForEachRuntimeRefNear(spot, 2000.0f, [&](RE::TESObjectREFR* r) { a_s.before.insert(r->GetFormID()); });
                {
                    // One at a time, around the spot (placeatme's distance and side), not all in one place.
                    int left = a_s.order.count, i = 0;
                    auto spread = [&](uint32_t a_form) {
                        Console("%X.placeatme %X 1 %d %d", a_s.marker, a_form, 150 + (i * 130) % 520, i % 4);
                        ++i;
                        --left;
                    };
                    if (a_s.order.faction == kTrader && a_s.order.count >= 2) {
                        // In member order (new references are numbered in the order they're made): merchant, brahmin, guards.
                        spread(kMerchant);
                        if (left >= 2) spread(kPackBrahmin);
                    }
                    while (left > 0) spread(form);
                }
                a_s.stage = Stage::FindActors;
                a_s.stageAt = now;
                return false;
            case Stage::FindActors: {
                std::vector<RE::Actor*> fresh;
                std::unordered_set<uint32_t> taken;
                for (auto& [id, other] : g_squads)
                    for (uint32_t fid : other.members) taken.insert(fid);
                ForEachRuntimeRefNear(spot, 2000.0f, [&](RE::TESObjectREFR* r) {
                    auto* a = r->As<RE::Actor>();
                    if (a && !a_s.before.contains(a->GetFormID()) && !taken.contains(a->GetFormID()) && !a->IsDead(false)) fresh.push_back(a);
                });
                if (fresh.size() < a_s.order.count && now - a_s.stageAt < std::chrono::seconds(4)) return false;   // more to come
                std::sort(fresh.begin(), fresh.end(), [](RE::Actor* a, RE::Actor* b) { return a->GetFormID() < b->GetFormID(); });
                if (a_s.order.faction == kTrader) {
                    // Caravan: the merchant first, then the brahmin (new references aren't numbered in the order they
                    // were placed, testing). Told apart by the names their records give them (the game's language).
                    auto named = [&](uint32_t a_base) {
                        auto* f = RE::TESForm::GetFormByID(a_base);
                        const std::string want = f ? std::string(RE::TESFullName::GetFullName(*f)) : std::string();
                        return [want](RE::Actor* a) { return !want.empty() && a->GetDisplayFullName() && want == a->GetDisplayFullName(); };
                    };
                    std::stable_partition(fresh.begin(), fresh.end(), named(kPackBrahmin));
                    std::stable_partition(fresh.begin(), fresh.end(), named(kMerchant));
                }
                a_s.members.assign(a_s.order.count, 0);
                auto* hp = HealthAV();
                for (size_t i = 0; i < fresh.size() && i < a_s.order.count; ++i) {
                    a_s.members[i] = fresh[i]->GetFormID();
                    const float frac = a_s.order.hp[i];
                    if (hp && frac < 0.99f) {
                        const float full = fresh[i]->GetPermanentActorValue(*hp);
                        fresh[i]->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -full * (1.0f - std::max(frac, 0.05f)));
                    }
                }
                a_s.stage = Stage::Ready;
                if (a_s.order.faction == kTrader && a_s.order.count >= 2 && a_s.members[0]) {
                    const Vendor* v = &kVendors[a_id % std::size(kVendors)];
                    Console("%X.addtofaction %X 0", a_s.members[0], v->faction);
                    g_merchants[a_s.members[0]] = v;
                    REX::INFO("[Wars] squad #{}: caravan merchant {:#x} (stock: {}'s)", a_id, a_s.members[0], v->name);
                }
                REX::INFO("[Wars] squad #{} is here: {} of {} on the spot {:.0f} {:.0f} {:.0f} (place {:#x})", a_id,
                    std::min(fresh.size(), size_t(a_s.order.count)), a_s.order.count, spot.x, spot.y, spot.z, a_s.order.anchorRef);
                ApplyGoal(a_id, a_s);
                return true;
            }
            case Stage::Ready:
                return true;
            }
            return true;
        }

        // Talking to a caravan's merchant opens the trade menu with its stock.
        class TradeSink final : public RE::BSTEventSink<RE::TESActivateEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& a_e, RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                auto* target = a_e.objectActivated.get();
                auto* by = a_e.actionRef.get();
                if (!target || !by || by != RE::PlayerCharacter::GetSingleton()) return RE::BSEventNotifyControl::kContinue;
                auto it = g_merchants.find(target->GetFormID());
                if (it == g_merchants.end()) return RE::BSEventNotifyControl::kContinue;
                auto* a = target->As<RE::Actor>();
                if (!a || a->IsDead(false) || a->GetHostileToActor(RE::PlayerCharacter::GetSingleton())) return RE::BSEventNotifyControl::kContinue;
                const uint32_t fid = target->GetFormID();
                const Vendor* v = it->second;
                F4SE::GetTaskInterface()->AddTask([fid, v]() {
                    Console("%X.sbm", fid);
                    REX::INFO("[Wars] trading with caravan merchant {:#x} ({}'s stock)", fid, v->name);
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        TradeSink g_tradeSink;
        bool g_tradeSinkOn = false;

        // The game's own placed enemies (not unique, not ours, hostile to the player) go as their area loads: the
        // server's squads live in their spots. Their references stay (disabled), so the game's records are fine.
        void SweepVanillaEnemies(RE::PlayerCharacter* a_pl)
        {
            auto* tes = RE::TES::GetSingleton();
            if (!tes) return;
            int n = 0;
            tes->ForEachReferenceInRange(a_pl->GetPosition(), 12000.0f, [&](RE::TESObjectREFR* r) {
                auto* a = r ? r->As<RE::Actor>() : nullptr;
                if (!a || a == a_pl || a->GetFormID() >= 0xFF000000u || a->IsDisabled() || a->IsDead(false)) return RE::BSContainer::ForEachResult::kContinue;
                if (!a->GetHostileToActor(a_pl)) return RE::BSContainer::ForEachResult::kContinue;
                a->Disable();
                ++n;
                if (g_vanillaRemoved.insert(a->GetFormID()).second && g_vanillaRemoved.size() < 400)
                    REX::INFO("[Wars] the game's own enemy {:#x} '{}' removed (A-Life squads live here)", a->GetFormID(),
                        a->GetDisplayFullName() ? a->GetDisplayFullName() : "");
                return RE::BSContainer::ForEachResult::kContinue;
            });
            (void)n;
        }
    }

    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload)
    {
        if (!Wars::Live()) return;
        if (a_type == static_cast<uint16_t>(MessageType::WarsChatter)) {
            // A-Life radio: who says what, on the HUD with the Pip-Boy radio's click.
            if (a_payload.size() < sizeof(WarsChatterMsg) || !Voice::ChatterOn()) return;   // the radio's off
            WarsChatterMsg c;
            std::memcpy(&c, a_payload.data(), sizeof(c));
            c.who[sizeof(c.who) - 1] = 0;
            c.text[sizeof(c.text) - 1] = 0;
            const std::string line = std::format("[RADIO] {}: {}", c.who, c.text);
            REX::INFO("[Wars] {}", line);
            Voice::RadioLine(c.who, c.text);   // on screen as long as the player set (F1), or the game's notification
            return;
        }
        if (a_type == static_cast<uint16_t>(MessageType::WarsSquadGoal)) {
            if (a_payload.size() < sizeof(WarsSquadGoalMsg)) return;
            WarsSquadGoalMsg g;
            std::memcpy(&g, a_payload.data(), sizeof(g));
            auto it = g_squads.find(g.squad);
            if (it == g_squads.end()) return;
            it->second.goal = g;
            if (it->second.stage == Stage::Ready) ApplyGoal(g.squad, it->second);
            return;
        }
        if (a_payload.size() < sizeof(WarsSquadMsg)) return;
        WarsSquadMsg m;
        std::memcpy(&m, a_payload.data(), sizeof(m));
        if (m.count > kWarsSquadMax) m.count = kWarsSquadMax;
        if (a_type == static_cast<uint16_t>(MessageType::WarsSquadOnline)) {
            if (g_squads.contains(m.squad)) return;
            Live s;
            s.order = m;
            s.goal.squad = m.squad;
            s.goal.travel = m.goalRef ? 1 : 0;
            s.goal.goalRef = m.goalRef;
            const bool home = m.gx != 0.0f || m.gy != 0.0f;   // where it lives (its camp), else where it spawns
            s.goal.x = home ? m.gx : m.x;
            s.goal.y = home ? m.gy : m.y;
            s.goal.z = home ? m.gz : m.z;
            std::copy(m.hp, m.hp + kWarsSquadMax, s.lastHp);
            g_squads.emplace(m.squad, std::move(s));
            REX::INFO("[Wars] squad #{} (faction {}, {}) handed to us", m.squad, m.faction, m.count);
        } else if (a_type == static_cast<uint16_t>(MessageType::WarsSquadOffline)) {
            auto it = g_squads.find(m.squad);
            if (it == g_squads.end()) return;
            const auto report = ReportOf(m.squad, it->second, true);
            Send(MessageType::WarsSquadReport, &report, sizeof(report));
            Remove(it->second);
            REX::INFO("[Wars] squad #{} handed back", m.squad);
            g_squads.erase(it);
        }
    }

    // Main thread, about once a second (Wars' own tick).
    void Tick()
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!Wars::Live() || !pl) return;
        const auto now = Clock::now();
        if (now - g_lastPresence >= std::chrono::seconds(2)) {
            g_lastPresence = now;
            WarsPresenceMsg p{};
            p.outside = Outside(pl) ? 1 : 0;
            const auto pos = pl->GetPosition();
            p.x = pos.x;
            p.y = pos.y;
            p.z = pos.z;
            Send(MessageType::WarsPresence, &p, sizeof(p));
        }
        if (!g_tradeSinkOn)
            if (auto* src = RE::TESActivateEvent::GetEventSource()) { src->RegisterSink(&g_tradeSink); g_tradeSinkOn = true; }
        Camps::Tick(Outside(pl));
        Claims::Tick();
        Teams::Tick();
        Death::Tick();
        if (Outside(pl) && now - g_lastSweep >= std::chrono::seconds(3)) {
            g_lastSweep = now;
            SweepVanillaEnemies(pl);
        }
        // One squad spawning at a time (each one finds its own newcomers).
        for (auto& [id, s] : g_squads)
            if (s.stage != Stage::Ready) { StepSpawn(id, s); break; }

        if (now - g_lastReport >= std::chrono::seconds(2)) {
            g_lastReport = now;
            for (auto& [id, s] : g_squads)
                if (s.stage == Stage::Ready && s.goal.travel) Breadcrumb(s);
            for (auto& [id, s] : g_squads) PumpJog(s);
            for (auto it = g_squads.begin(); it != g_squads.end();) {
                if (it->second.stage != Stage::Ready) { ++it; continue; }
                const auto report = ReportOf(it->first, it->second, false);
                Send(MessageType::WarsSquadReport, &report, sizeof(report));
                const bool wiped = std::all_of(report.hp, report.hp + report.count, [](float h) { return h <= 0.0f; });
                if (wiped) {
                    REX::INFO("[Wars] squad #{} wiped out", it->first);
                    if (auto* m = RefOf(it->second.marker)) { m->Disable(); m->SetWantsDelete(true); m->MarkAsDeleted(); }
                    it = g_squads.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    // A save loaded / the connection ended: our squads' NPCs go (the server takes them back by itself).
    void Reset()
    {
        for (auto& [id, s] : g_squads) Remove(s);
        g_squads.clear();
        g_vanillaRemoved.clear();
        g_merchants.clear();
        Camps::Reset();
        Claims::Reset();
        Teams::Reset();
    }

    size_t Count() { return g_squads.size(); }

    void Log()
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        const auto me = pl ? pl->GetPosition() : RE::NiPoint3{};
        REX::INFO("[Wars] {} A-Life squad(s) here; player at {:.0f} {:.0f} {:.0f}; {} of the game's enemies removed", g_squads.size(),
            me.x, me.y, me.z, g_vanillaRemoved.size());
        Camps::Log();
        for (auto& [id, s] : g_squads)
            for (uint32_t fid : s.members) {
                auto* a = ActorOf(fid);
                if (!a) { REX::INFO("[Wars]   squad #{} member {:#x}: gone", id, fid); continue; }
                const auto p = a->GetPosition();
                auto* cur = a->currentProcess ? a->currentProcess->currentPackage.package : nullptr;
                REX::INFO("[Wars]   squad #{} member {:#x} '{}' at {:.0f} {:.0f} {:.0f} ({:.0f} away) package {:#x}{}{}", id, fid,
                    a->GetDisplayFullName() ? a->GetDisplayFullName() : "", p.x, p.y, p.z, p.GetDistance(me), cur ? cur->GetFormID() : 0u,
                    a->IsDead(false) ? " dead" : "", a->IsDisabled() ? " disabled" : "");
            }
    }

    void SetJog(bool a_on)
    {
        g_jog = a_on;
        REX::INFO("[Wars] squads on the move: {}", a_on ? "jog" : "walk");
    }

    void AiProbe()
    {
        for (auto& [id, s] : g_squads)
            for (uint32_t fid : s.members) {
                auto* a = ActorOf(fid);
                auto* npc = a ? a->GetNPC() : nullptr;
                if (!npc) continue;
                auto* cur = a->currentProcess ? a->currentProcess->currentPackage.package : nullptr;
                REX::INFO("[Wars]   {:#x}: base {:#x} packages [{}] now running {:#x}", fid, npc->GetFormID(), PackList(npc), cur ? cur->GetFormID() : 0u);
            }
    }
}
