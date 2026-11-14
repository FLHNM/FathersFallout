// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Down, not dead (Update 29; see Downed.h). The killing blow is stopped by the player's health clamp in WarsDeath.cpp
// (vtable slot 0x131, health stays at 1). Here: the down state (crawl speed, weapon away), the window with the ways
// out, the CoopDowned/CoopRevive messages and the revive itself (crouch beside a downed friend for 3 s).

#include "pch.h"
#include "sync/Downed.h"
#include "sync/Appearance.h"
#include "sync/CoopSync.h"
#include "sync/Wars.h"
#include "sync/WarsDeath.h"
#include "sync/WarsTeams.h"
#include "F4SEMenuFramework.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>

namespace F4MP::Downed
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kCampaignWait = std::chrono::seconds(90);   // then: respawn at host / the game's death
        constexpr auto kWarsWait = std::chrono::seconds(60);       // then: the Wars death (bag, respawn)
        constexpr auto kReviveHold = std::chrono::milliseconds(3000);
        constexpr float kReviveRange = 220.0f;                     // units: beside them

        enum class Choice : int { None, Wait, Respawn, GiveUp };

        // Ours.
        std::atomic<bool> g_down{ false };
        Clock::time_point g_downAt{}, g_lastSent{};
        std::atomic<int> g_choice{ static_cast<int>(Choice::None) };
        bool (*g_goToHost)(bool) = nullptr;
        F4SEMenuFramework::Model::WindowInterface* g_window = nullptr;
        bool g_test = false;   // f4mp:down

        // Others who are down (player id -> since / seconds they had left then), and our revive in progress.
        struct Other { Clock::time_point seen; float left = 0.0f; };
        std::mutex g_othersLock;
        std::unordered_map<uint32_t, Other> g_others;
        uint32_t g_reviving = 0;
        Clock::time_point g_reviveStart{}, g_lastHint{};

        void Hud(const std::string& a_text)
        {
            REX::INFO("[Down] {}", a_text);
            RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false, false);
        }

        std::string Name(uint32_t a_id)
        {
            auto n = Appearance::NameOf(a_id);
            return n.empty() ? std::format("Player {}", a_id) : n;
        }

        RE::ActorValueInfo* AV(bool a_speed)
        {
            auto* avs = RE::ActorValue::GetSingleton();
            return avs ? (a_speed ? avs->speedMult : avs->health) : nullptr;
        }

        auto Wait() { return Wars::Mode() ? kWarsWait : kCampaignWait; }

        float SecondsLeft()
        {
            const auto left = std::chrono::duration<float>(Wait() - (Clock::now() - g_downAt)).count();
            return (std::max)(0.0f, left);
        }

        void SendState(bool a_down)
        {
            auto& net = Network::NetworkClient::GetInstance();
            if (!net.IsConnected()) return;
            Network::CoopDownedMsg m{};
            m.downed = a_down ? 1 : 0;
            m.secondsLeft = a_down ? SecondsLeft() : 0.0f;
            net.SendPacket(Network::MessageType::CoopDowned, &m, sizeof(m));
            g_lastSent = Clock::now();
        }

        // No speed change while down: an actor value modifier is written into a save made while down and would
        // have stayed for good (review before release). Down = 1 health, weapon away.
        void SetCrawl(RE::PlayerCharacter*, bool) {}

        bool AnyoneElse()
        {
            auto& net = Network::NetworkClient::GetInstance();
            return net.IsConnected() && !net.GetRemotePlayers().empty();
        }

        // Back on our feet (revived, respawned at the host): health, speed, the window, everyone told.
        void StandUp(float a_healthFraction)
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (pl) {
                if (auto* hp = AV(false)) {
                    const float max = pl->GetPermanentActorValue(*hp);
                    const float cur = pl->GetActorValue(*hp);
                    const float want = (std::max)(1.0f, max * a_healthFraction);
                    if (cur < want) pl->RestoreActorValue(*hp, want - cur);
                }
                SetCrawl(pl, false);
            }
            if (g_window) g_window->IsOpen = false;
            g_down = false;
            g_test = false;
            g_choice = static_cast<int>(Choice::None);
            Wars::Death::ClearPending();
            SendState(false);
        }

        void GoDown(RE::PlayerCharacter* a_pl)
        {
            g_down = true;
            g_downAt = Clock::now();
            g_choice = static_cast<int>(Choice::None);
            SetCrawl(a_pl, true);
            if (a_pl->GetWeaponMagicDrawn()) a_pl->DrawWeaponMagicHands(false);
            SendState(true);
            if (g_window) g_window->IsOpen = true;
            Hud(Wars::Mode() ? "YOU'RE DOWN. Your crew can revive you: they crouch beside you for 3 seconds."
                             : "YOU'RE DOWN. A friend can revive you: they crouch beside you for 3 seconds.");
        }

        // The way out we (or the clock) picked.
        void Resolve(Choice a_how)
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!pl) return;
            if (Wars::Mode()) {
                // The Wars death takes it from here: bag, respawn (WarsDeath.cpp, its next tick).
                SetCrawl(pl, false);
                if (g_window) g_window->IsOpen = false;
                g_down = false;
                g_choice = static_cast<int>(Choice::None);
                SendState(false);
                Wars::Death::DieNow();
                REX::INFO("[Down] Wars: gave up / nobody came - the Wars death runs");
                return;
            }
            if (a_how == Choice::Respawn && !IsHost() && g_goToHost) {
                StandUp(0.5f);
                Hud("Back on your feet - travelling to the host.");
                if (!g_goToHost(false)) Hud("Couldn't reach the host right now: you got up where you fell.");
                return;
            }
            // The game's own death: the death screen, then the last save loads (the clamp lets KillImpl through).
            SetCrawl(pl, false);
            if (g_window) g_window->IsOpen = false;
            g_down = false;
            g_choice = static_cast<int>(Choice::None);
            Wars::Death::ClearPending();
            SendState(false);
            REX::INFO("[Down] campaign: the game's death (last save)");
            pl->KillImpl(nullptr, 100000.0f, true, false);
        }

        void __stdcall RenderWindow()
        {
            using namespace ImGuiMCP;
            if (!g_down) { if (g_window) g_window->IsOpen = false; return; }
            if (auto* io = GetIO())
                SetNextWindowPos(ImVec2(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.3f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
            Begin("YOU'RE DOWN##ffdown", nullptr, ImGuiWindowFlags_NoCollapse);
            const int left = static_cast<int>(SecondsLeft());
            if (Wars::Mode()) {
                TextWrapped("Your crew can revive you: they crouch beside you for 3 seconds.");
                TextWrapped("Nobody comes in %d s: everything you carry stays in a bag where you fell, and you get up at your bed.", left);
                if (Button("WAIT FOR MY CREW", ImVec2(210.0f, 0.0f))) g_choice = static_cast<int>(Choice::Wait);
                SameLine();
                if (Button("GIVE UP (RESPAWN)", ImVec2(210.0f, 0.0f))) g_choice = static_cast<int>(Choice::GiveUp);
            } else {
                TextWrapped("A friend can revive you: they crouch beside you for 3 seconds.");
                if (!IsHost()) {
                    TextWrapped("Or get up next to the host. In %d s that happens by itself.", left);
                    if (Button("RESPAWN AT HOST", ImVec2(210.0f, 0.0f))) g_choice = static_cast<int>(Choice::Respawn);
                    SameLine();
                } else {
                    TextWrapped("Nobody comes in %d s: your last save loads (the game's own death).", left);
                }
                if (Button("LOAD LAST SAVE", ImVec2(210.0f, 0.0f))) g_choice = static_cast<int>(Choice::GiveUp);
                if (Button("WAIT FOR A FRIEND (CLOSE)", ImVec2(428.0f, 0.0f))) g_choice = static_cast<int>(Choice::Wait);
            }
            End();
        }
    }

    void Install()
    {
        if (!F4SEMenuFramework::IsInstalled() || g_window) return;
        g_window = F4SEMenuFramework::AddWindow(RenderWindow, true);
        REX::INFO("[Down] down-not-dead window {}", g_window ? "ready" : "NOT available (menu framework)");
    }

    void SetGoToHost(bool (*a_fn)(bool)) { g_goToHost = a_fn; }

    bool HoldsDeath()
    {
        if (Wars::Mode()) return false;   // Wars: WarsDeath holds every death itself
        return Coop::GetSettings().downNotDead && AnyoneElse();
    }

    bool Revivable()
    {
        if (!Wars::Mode() || !Coop::GetSettings().downNotDead) return false;
        auto& net = Network::NetworkClient::GetInstance();
        if (!net.IsConnected()) return false;
        for (auto& [id, p] : net.GetRemotePlayers())
            if (Wars::Teams::Friendly(id)) return true;
        return false;
    }

    bool IsDown() { return g_down; }

    void OnDowned(const Network::CoopDownedMsg& a_msg)
    {
        if (Wars::Mode() && !Wars::Teams::Friendly(a_msg.senderId)) return;   // only your crew comes for you
        std::lock_guard l(g_othersLock);
        if (a_msg.downed) {
            const bool fresh = !g_others.contains(a_msg.senderId);
            g_others[a_msg.senderId] = { Clock::now(), a_msg.secondsLeft };
            if (fresh) Hud(std::format("{} IS DOWN! Crouch beside them for 3 seconds to revive them.", Name(a_msg.senderId)));
        } else if (g_others.erase(a_msg.senderId)) {
            if (g_reviving == a_msg.senderId) g_reviving = 0;
        }
    }

    void OnRevive(const Network::CoopReviveMsg& a_msg)
    {
        if (a_msg.targetId != Network::NetworkClient::GetInstance().GetPlayerId() || !g_down) return;
        if (Wars::Mode() && !Wars::Teams::Friendly(a_msg.senderId)) return;   // Wars: only your crew / faction
        StandUp(0.35f);
        Hud(std::format("{} got you back on your feet.", Name(a_msg.senderId)));
    }

    void Tick()
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl || !pl->parentCell) return;
        const auto now = Clock::now();

        // 1. Us. The clamp stopped a killing blow (or a test): down, if someone can come for us.
        if (!g_down) {
            const bool blow = Wars::Death::Pending() || g_test;
            if (blow && (g_test || HoldsDeath() || Revivable())) GoDown(pl);
        } else {
            if (pl->IsDead(false)) { g_down = false; SetCrawl(pl, false); return; }   // the game killed us anyway
            if (pl->GetWeaponMagicDrawn()) pl->DrawWeaponMagicHands(false);           // no fighting while down
            if (now - g_lastSent > std::chrono::seconds(2)) SendState(true);
            const auto choice = static_cast<Choice>(g_choice.exchange(static_cast<int>(Choice::None)));
            if (choice == Choice::Wait) {
                if (g_window) g_window->IsOpen = false;
                Hud("Waiting for a revive. The options are back in the F1 menu (FathersFallout).");
            } else if (choice == Choice::Respawn || choice == Choice::GiveUp) {
                Resolve(choice);
                return;
            } else if (SecondsLeft() <= 0.0f) {
                Resolve(Wars::Mode() ? Choice::GiveUp : (IsHost() ? Choice::GiveUp : Choice::Respawn));
                return;
            }
            // Nobody left to play with (they disconnected): the old rules.
            if (!g_test && !AnyoneElse()) { Resolve(Choice::GiveUp); return; }
        }

        // 2. Others who are down: crouch beside one for 3 s.
        std::unordered_map<uint32_t, Other> others;
        {
            std::lock_guard l(g_othersLock);
            std::erase_if(g_others, [&](auto& kv) { return now - kv.second.seen > std::chrono::seconds(6); });
            others = g_others;
        }
        if (others.empty() || g_down) { g_reviving = 0; return; }
        auto& net = Network::NetworkClient::GetInstance();
        const auto remotes = net.GetRemotePlayers();
        const auto me = pl->GetPosition();
        uint32_t nearId = 0;
        for (auto& [id, o] : others) {
            auto r = remotes.find(id);
            if (r == remotes.end()) continue;
            const float dx = r->second.x - me.x, dy = r->second.y - me.y, dz = r->second.z - me.z;
            if (dx * dx + dy * dy + dz * dz <= kReviveRange * kReviveRange) { nearId = id; break; }
        }
        if (!nearId) { g_reviving = 0; return; }
        if (!pl->IsSneaking()) {
            g_reviving = 0;
            if (now - g_lastHint > std::chrono::seconds(4)) {
                g_lastHint = now;
                RE::SendHUDMessage::ShowHUDMessage(std::format("Crouch to revive {}", Name(nearId)).c_str(), nullptr, false, false);
            }
            return;
        }
        if (g_reviving != nearId) {
            g_reviving = nearId;
            g_reviveStart = now;
            RE::SendHUDMessage::ShowHUDMessage(std::format("Reviving {}...", Name(nearId)).c_str(), nullptr, false, false);
            return;
        }
        if (now - g_reviveStart < kReviveHold) return;
        Network::CoopReviveMsg m{};
        m.targetId = nearId;
        net.SendPacket(Network::MessageType::CoopRevive, &m, sizeof(m));
        {
            std::lock_guard l(g_othersLock);
            g_others.erase(nearId);
        }
        g_reviving = 0;
        Hud(std::format("You revived {}.", Name(nearId)));
    }

    void Reset()
    {
        if (g_down) {
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) SetCrawl(pl, false);
            if (g_window) g_window->IsOpen = false;
        }
        g_down = false;
        g_test = false;
        g_reviving = 0;
        std::lock_guard l(g_othersLock);
        g_others.clear();
    }

    void Bridge(const std::string& a_what)
    {
        if (a_what == "revive") {
            if (g_down) StandUp(0.35f);
            return;
        }
        if (a_what == "respawn") { g_choice = static_cast<int>(Choice::Respawn); return; }
        if (a_what == "giveup") { g_choice = static_cast<int>(Choice::GiveUp); return; }
        g_test = true;
        REX::INFO("[Down] test: going down on the next tick");
    }

    // The F1 menu page shows the same choices while we are down (after WAIT closed the window).
    void RenderMenu()
    {
        using namespace ImGuiMCP;
        if (!g_down) return;
        SeparatorText("You're down");
        Text("%d s left", static_cast<int>(SecondsLeft()));
        if (Wars::Mode()) {
            if (Button("Give up (respawn, your gear stays in a bag)")) g_choice = static_cast<int>(Choice::GiveUp);
            return;
        }
        if (!IsHost()) {
            if (Button("Respawn at host")) g_choice = static_cast<int>(Choice::Respawn);
            SameLine();
        }
        if (Button("Load last save")) g_choice = static_cast<int>(Choice::GiveUp);
    }
}
