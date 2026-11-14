// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Wasteland Wars portable camp (see WarsMyCamp.h). The kit is the game's own workshop furniture, placed with placeatme
// at the player and moved into a small ring in front of them (the same way WarsCamps.cpp builds A-Life camps). A
// runtime-placed settlement workbench is only a container: workshop mode needs a plugin
// Location, so the camp is a kit of things that work anywhere instead.

#include "pch.h"
#include "sync/WarsMyCamp.h"
#include "sync/Wars.h"
#include "sync/WarsTeams.h"
#include "sync/Appearance.h"
#include "F4SEMenuFramework.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <format>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace F4MP::Wars::MyCamp
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The kit: base form, offset forward / right from the camp's spot (units), turn (degrees, 180 = faces back
        // toward the player), only in our own camp (the stash: each game's copy would hold different things).
        struct Piece { uint32_t form; float fwd, right, turn; bool ownOnly; const char* what; };
        constexpr Piece kKit[] = {
            { 0x002476B7, 220.0f, 0.0f, 0.0f, false, "cooking fire" },         // WorkbenchCookingFireWorkshop
            { 0x00118F57, 140.0f, 190.0f, 270.0f, false, "sleeping bag" },     // WorkshopNpcBedSleepingBagSleep01
            { 0x00175500, 140.0f, -190.0f, 90.0f, true, "stash" },             // FootLocker01
            { 0x0017B3A4, 380.0f, 160.0f, 200.0f, false, "weapons workbench" } // workbenchWeaponsA
        };
        constexpr int kPieces = static_cast<int>(std::size(kKit));
        constexpr float kShowWithin = 8000.0f;   // others' camps are placed here when this close

        struct Spot { uint32_t space = 0; RE::NiPoint3 at{}; float heading = 0.0f; };

        // A camp being placed: the pieces are placeatme'd at the player, then picked up as "new runtime refs near us".
        struct Build {
            Spot spot;
            bool own = false;
            uint32_t owner = 0;
            std::unordered_set<uint32_t> before;
            Clock::time_point at{};
            bool active = false;
        };

        struct Camp { Spot spot; bool deployed = false; uint32_t refs[kPieces]{}; };
        Camp g_mine;                                       // ours (saved in the co-save)
        std::unordered_map<uint32_t, Camp> g_others;       // others' copies here, by player id
        std::mutex g_inLock;
        std::unordered_map<uint32_t, std::pair<Network::WarsPlayerCampMsg, Clock::time_point>> g_inbox;   // latest per player
        Build g_build;
        std::vector<uint32_t> g_ground;   // pieces just moved into place: onto the ground on the next tick
        Clock::time_point g_lastBroadcast{};

        void Hud(const std::string& a_text)
        {
            REX::INFO("[Wars] camp: {}", a_text);
            RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false, false);
        }

        void Console(const std::string& a_cmd) { RE::Console::ExecuteCommand(a_cmd.c_str()); }

        template <class T = RE::TESObjectREFR>
        T* RefOf(uint32_t a_fid)
        {
            auto* f = a_fid ? RE::TESForm::GetFormByID(a_fid) : nullptr;
            return f ? f->As<T>() : nullptr;
        }

        void Delete(uint32_t a_fid)
        {
            if (auto* r = RefOf(a_fid); r && !r->IsDeleted()) {
                r->Disable();
                r->SetWantsDelete(true);
                r->MarkAsDeleted();
            }
        }

        uint32_t SpaceOf(RE::TESObjectREFR* a_ref)
        {
            auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
            if (!cell) return 0;
            if (cell->IsInterior()) return cell->GetFormID();
            return cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
        }

        RE::NiPoint3 PieceAt(const Spot& a_s, const Piece& a_p)
        {
            // Heading 0 looks north (+y); right is +x at heading 0.
            const float fx = std::sin(a_s.heading), fy = std::cos(a_s.heading);
            const float rx = std::cos(a_s.heading), ry = -std::sin(a_s.heading);
            return { a_s.at.x + fx * a_p.fwd + rx * a_p.right, a_s.at.y + fy * a_p.fwd + ry * a_p.right, a_s.at.z };
        }

        std::unordered_set<uint32_t> RuntimeRefsNear(const RE::NiPoint3& a_at, float a_r)
        {
            std::unordered_set<uint32_t> out;
            if (auto* tes = RE::TES::GetSingleton())
                tes->ForEachReferenceInRange(a_at, a_r, [&](RE::TESObjectREFR* r) {
                    if (r && r->GetFormID() >= 0xFF000000u) out.insert(r->GetFormID());
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            return out;
        }

        // Onto the ground: the camp's spot is the player's feet, which can be a roof, a ledge or a slope (testing: the
        // weapons workbench hung in the air past a roof's edge). The game's own navmesh snap (Papyrus).
        void ToGround(RE::TESObjectREFR* a_ref)
        {
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            if (!vm || !a_ref) return;
            const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<std::uint32_t>(a_ref->GetFormType()), a_ref);
            if (handle == vm->GetObjectHandlePolicy().EmptyHandle()) return;
            vm->DispatchMethodCall(handle, "ObjectReference", "MoveToNearestNavmeshLocation", nullptr);
        }

        void Broadcast()
        {
            auto& net = Network::NetworkClient::GetInstance();
            if (!net.IsConnected()) return;
            Network::WarsPlayerCampMsg m{};
            m.deployed = g_mine.deployed ? 1 : 0;
            m.space = g_mine.spot.space;
            m.x = g_mine.spot.at.x; m.y = g_mine.spot.at.y; m.z = g_mine.spot.at.z;
            m.heading = g_mine.spot.heading;
            net.SendPacket(Network::MessageType::WarsPlayerCamp, &m, sizeof(m));
            g_lastBroadcast = Clock::now();
        }

        void Remove(Camp& a_c)
        {
            for (auto& r : a_c.refs) { Delete(r); r = 0; }
            a_c.deployed = false;
        }

        // Step 1: placeatme every piece at the player (not at the spot: only a loaded reference can placeatme).
        void StartBuild(const Spot& a_spot, bool a_own, uint32_t a_owner)
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!pl) return;
            g_build = {};
            g_build.spot = a_spot;
            g_build.own = a_own;
            g_build.owner = a_owner;
            g_build.before = RuntimeRefsNear(pl->GetPosition(), 600.0f);
            for (auto& p : kKit)
                if (a_own || !p.ownOnly) Console(std::format("player.placeatme {:08X} 1", p.form));
            g_build.at = Clock::now();
            g_build.active = true;
        }

        // Step 2 (next ticks): the new references, moved into place.
        void FinishBuild()
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!pl) return;
            std::unordered_map<uint32_t, std::vector<uint32_t>> fresh;
            if (auto* tes = RE::TES::GetSingleton())
                tes->ForEachReferenceInRange(pl->GetPosition(), 600.0f, [&](RE::TESObjectREFR* r) {
                    auto* base = r ? r->GetObjectReference() : nullptr;
                    if (r && base && r->GetFormID() >= 0xFF000000u && !g_build.before.contains(r->GetFormID()))
                        fresh[base->GetFormID()].push_back(r->GetFormID());
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            int want = 0, got = 0;
            for (auto& p : kKit) {
                if (!g_build.own && p.ownOnly) continue;
                ++want;
                got += !fresh[p.form].empty();
            }
            if (got < want && Clock::now() - g_build.at < std::chrono::seconds(3)) return;   // more to come
            g_build.active = false;

            Camp camp;
            camp.spot = g_build.spot;
            camp.deployed = true;
            for (int i = 0; i < kPieces; ++i) {
                const auto& p = kKit[i];
                auto& v = fresh[p.form];
                if (v.empty()) continue;
                std::sort(v.begin(), v.end());
                const uint32_t ref = v.front();
                v.erase(v.begin());
                camp.refs[i] = ref;
                const auto at = PieceAt(camp.spot, p);
                Console(std::format("{:X}.setpos x {:.1f}", ref, at.x));
                Console(std::format("{:X}.setpos y {:.1f}", ref, at.y));
                Console(std::format("{:X}.setpos z {:.1f}", ref, at.z));
                Console(std::format("{:X}.setangle z {:.1f}", ref, camp.spot.heading * 57.2957795f + p.turn));
                // A footlocker comes with the game's loot in it: the stash starts empty.
                if (p.ownOnly) Console(std::format("{:X}.removeallitems", ref));
                g_ground.push_back(ref);
            }
            for (auto& [f, v] : fresh)
                for (uint32_t extra : v) Delete(extra);

            REX::INFO("[Wars] camp: pieces fire {:#x} bag {:#x} stash {:#x} bench {:#x}", camp.refs[0], camp.refs[1], camp.refs[2], camp.refs[3]);
            if (g_build.own) {
                Remove(g_mine);
                g_mine = camp;
                if (const uint32_t bag = g_mine.refs[1]) Wars::SetBed(bag);   // the camp's sleeping bag is where you get up
                Broadcast();
                Hud("Camp set up. Its sleeping bag is your respawn point; the footlocker is your stash.");
            } else {
                auto& c = g_others[g_build.owner];
                Remove(c);
                c = camp;
                REX::INFO("[Wars] camp: player {}'s camp shown at {:.0f} {:.0f}", g_build.owner, camp.spot.at.x, camp.spot.at.y);
            }
        }

        bool Own(uint32_t a_ref)
        {
            for (auto r : g_mine.refs) if (r && r == a_ref) return true;
            return false;
        }
    }

    bool Deployed() { return g_mine.deployed; }

    void Deploy()
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!Wars::Live() || !pl || !pl->parentCell) return;
        if (g_build.active) { Hud("Still setting up..."); return; }
        if (pl->IsInCombat()) { Hud("Not while you're fighting."); return; }
        // An old camp: the stash comes to us first (packing up), then the new one goes up here.
        if (g_mine.deployed) {
            PackUp();
            if (g_mine.deployed) return;   // too far from it to pack up
        }
        Spot s;
        s.space = SpaceOf(pl);
        s.at = pl->GetPosition();
        s.heading = pl->GetHeading();
        StartBuild(s, true, 0);
    }

    void PackUp()
    {
        if (!g_mine.deployed) return;
        // Far away the camp's references aren't in memory: the stash's things would be left behind with the camp.
        auto* stash = RefOf(g_mine.refs[2]);
        if (g_mine.refs[2] && (!stash || !stash->Get3D())) {
            Hud("Go to your camp to pack it up or move it (your stash is there).");
            return;
        }
        if (stash) Console(std::format("{:X}.removeallitems player", stash->GetFormID()));
        if (g_mine.refs[1] && Wars::Bed() == g_mine.refs[1]) Wars::SetBed(0);
        // The removeallitems runs before the delete takes effect (the console is synchronous).
        Remove(g_mine);
        g_mine = {};
        Broadcast();
        Hud("Camp packed up. Your stash's things are back in your pockets.");
    }

    void RenderMenu()
    {
        using namespace ImGuiMCP;
        if (!Wars::Live()) return;
        SeparatorText("Camp");
        if (g_mine.deployed) {
            if (Button("Move camp here")) Deploy();
            SameLine();
            if (Button("Pack up camp")) PackUp();
        } else {
            TextWrapped("Set up a camp where you stand: sleeping bag (respawn point), cooking fire, weapons workbench, stash.");
            if (Button("Set up camp here")) Deploy();
        }
    }

    void OnRemote(const Network::WarsPlayerCampMsg& a_m)
    {
        if (!Wars::Mode() || !std::isfinite(a_m.x) || !std::isfinite(a_m.y) || !std::isfinite(a_m.z) || !std::isfinite(a_m.heading)) return;
        std::lock_guard l(g_inLock);
        g_inbox[a_m.senderId] = { a_m, Clock::now() };
    }

    void Tick()
    {
        if (!Wars::Live()) return;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl || !pl->parentCell) return;
        if (!g_ground.empty()) {
            for (uint32_t r : g_ground) ToGround(RefOf(r));
            g_ground.clear();
        }
        if (g_build.active) { FinishBuild(); return; }

        // Ours: told again every 30 s (late joiners), and checked (a scrapped / vanished piece).
        if (g_mine.deployed && Clock::now() - g_lastBroadcast > std::chrono::seconds(30)) Broadcast();

        // Others': place, move or remove copies near us; one build at a time.
        std::unordered_map<uint32_t, std::pair<Network::WarsPlayerCampMsg, Clock::time_point>> inbox;
        {
            std::lock_guard l(g_inLock);
            inbox = g_inbox;
        }
        const auto me = pl->GetPosition();
        const uint32_t mySpace = SpaceOf(pl);
        const auto remotes = Network::NetworkClient::GetInstance().GetRemotePlayers();
        for (auto it = g_others.begin(); it != g_others.end();) {
            auto in = inbox.find(it->first);
            const bool gone = !remotes.contains(it->first) || in == inbox.end() || !in->second.first.deployed ||
                              Clock::now() - in->second.second > std::chrono::seconds(95);
            if (gone) { Remove(it->second); it = g_others.erase(it); }
            else ++it;
        }
        for (auto& [id, entry] : inbox) {
            const auto& m = entry.first;
            if (!m.deployed || !remotes.contains(id) || m.space != mySpace) continue;
            const float dx = m.x - me.x, dy = m.y - me.y;
            if (dx * dx + dy * dy > kShowWithin * kShowWithin) continue;
            auto c = g_others.find(id);
            const bool same = c != g_others.end() && c->second.deployed && std::abs(c->second.spot.at.x - m.x) < 1.0f &&
                              std::abs(c->second.spot.at.y - m.y) < 1.0f;
            if (same) continue;
            static std::unordered_map<uint32_t, Clock::time_point> s_lastBuilt;   // a player moving their camp non-stop
            auto& last = s_lastBuilt[id];                                          // can't make us rebuild every second
            if (Clock::now() - last < std::chrono::seconds(10)) continue;
            last = Clock::now();
            Spot s;
            s.space = m.space;
            s.at = { m.x, m.y, m.z };
            s.heading = m.heading;
            StartBuild(s, false, id);
            return;
        }
    }

    void Reset()
    {
        for (auto& [id, c] : g_others) Remove(c);
        g_others.clear();
        std::lock_guard l(g_inLock);
        g_inbox.clear();
    }

    std::vector<uint8_t> Save()
    {
        std::vector<uint8_t> out(1 + 4 + 16 + 4 * kPieces, 0);
        out[0] = g_mine.deployed ? 1 : 0;
        std::memcpy(out.data() + 1, &g_mine.spot.space, 4);
        const float f[4] = { g_mine.spot.at.x, g_mine.spot.at.y, g_mine.spot.at.z, g_mine.spot.heading };
        std::memcpy(out.data() + 5, f, 16);
        std::memcpy(out.data() + 21, g_mine.refs, 4 * kPieces);
        return out;
    }

    void Load(const uint8_t* a_data, size_t a_size, const std::function<uint32_t(uint32_t)>& a_resolve)
    {
        g_mine = {};
        if (a_size < 1 + 4 + 16 + 4 * kPieces) return;
        g_mine.deployed = a_data[0] != 0;
        std::memcpy(&g_mine.spot.space, a_data + 1, 4);
        float f[4];
        std::memcpy(f, a_data + 5, 16);
        g_mine.spot.at = { f[0], f[1], f[2] };
        g_mine.spot.heading = f[3];
        std::memcpy(g_mine.refs, a_data + 21, 4 * kPieces);
        for (auto& r : g_mine.refs) if (r) r = a_resolve(r);
        REX::INFO("[Wars] camp: loaded ({}, at {:.0f} {:.0f})", g_mine.deployed ? "set up" : "packed", f[0], f[1]);
    }

    void Revert()
    {
        g_mine = {};
        g_build = {};
    }

    void Bridge(const std::string& a_what)
    {
        if (a_what == "pack") PackUp();
        else Deploy();
    }
}
