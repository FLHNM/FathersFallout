// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Wasteland Wars sides on this side of the wire. The server decides (it keeps every crew and every player's pick,
// and the cooldown); this file shows it and asks for changes: the choice window that opens by itself until we've
// picked, the same choices in F1, name tags with the crew's tag and colour, the crew's own radio frequency, and the
// faction we sided with made our own in the game (its NPCs friendly: addtofaction).

#include "pch.h"
#include "sync/WarsTeams.h"
#include "sync/Wars.h"
#include "sync/WarsVoice.h"
#include "network/NetworkClient.h"
#include "F4SEMenuFramework.h"

#include <cstring>
#include <map>
#include <mutex>

namespace F4MP::Wars::Teams
{
    using namespace Network;

    namespace
    {
        struct Info {
            uint8_t kind = 0, faction = 0, colour = 0;
            uint32_t crew = 0, cooldown = 0;
            std::string tag, crewName;
        };
        struct CrewInfo {
            uint8_t colour = 0, members = 0;
            bool open = true;
            std::string name, tag, leader;
            std::vector<std::string> requests;
        };
        std::mutex g_lock;
        std::map<uint32_t, Info> g_players;      // by player id (ours too)
        std::map<uint32_t, CrewInfo> g_crews;
        bool g_haveMine = false;
        std::string g_notice;
        F4SEMenuFramework::Model::WindowInterface* g_window = nullptr;
        bool g_windowShown = false;              // opened by itself once per session
        int g_appliedFaction = -2;               // what we made ourselves part of in the game (-1 none)
        uint32_t g_tunedCrew = 0;

        constexpr const char* kColourNames[8] = { "Red", "Blue", "White", "Orange", "Green", "Yellow", "Purple", "Cyan" };
        constexpr float kColours[8][3] = { { 0.92f, 0.22f, 0.22f }, { 0.28f, 0.52f, 1.0f }, { 0.95f, 0.95f, 0.95f },
            { 1.0f, 0.55f, 0.12f }, { 0.3f, 0.9f, 0.32f }, { 1.0f, 0.9f, 0.2f }, { 0.72f, 0.38f, 1.0f }, { 0.2f, 0.9f, 0.95f } };
        // Brotherhood, Minutemen, Railroad, Institute: name, tag, colour, the game's faction(s) we join.
        struct Allegiance { const char* name; const char* tag; float rgb[3]; uint32_t factions[2]; const char* blurb; };
        constexpr Allegiance kFactions[4] = {
            { "Brotherhood of Steel", "BoS", { 0.75f, 0.8f, 0.9f }, { 0x0005DE41, 0 }, "Power armor, vertibirds, no love for mutants or synths." },
            { "Minutemen", "MM", { 0.55f, 0.7f, 1.0f }, { 0x00068043, 0 }, "At a moment's notice: settlers' friends." },
            { "Railroad", "RR", { 0.95f, 0.65f, 0.35f }, { 0x000994F6, 0 }, "Free the synths, stay in the shadows." },
            { "Institute", "INST", { 0.85f, 0.95f, 1.0f }, { 0x0005E558, 0x00083B31 }, "Mankind redefined: the synths are with you." },
        };

        uint32_t Abgr(const float a_rgb[3])
        {
            auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return 0xFF000000u | (c(a_rgb[2]) << 16) | (c(a_rgb[1]) << 8) | c(a_rgb[0]);
        }
        ImGuiMCP::ImVec4 Vec(const float a_rgb[3]) { return ImGuiMCP::ImVec4(a_rgb[0], a_rgb[1], a_rgb[2], 1.0f); }

        void Send(uint8_t a_action, uint8_t a_arg = 0, uint32_t a_crew = 0, const char* a_name = "", const char* a_tag = "",
            const char* a_who = "", bool a_open = true)
        {
            auto& net = NetworkClient::GetInstance();
            if (!net.IsConnected() || !Wars::Live()) return;
            WarsTeamActionMsg m{};
            m.action = a_action;
            m.arg = a_arg;
            m.open = a_open ? 1 : 0;
            m.crew = a_crew;
            strncpy_s(m.name, a_name, sizeof(m.name) - 1);
            strncpy_s(m.tag, a_tag, sizeof(m.tag) - 1);
            strncpy_s(m.who, a_who, sizeof(m.who) - 1);
            net.SendPacket(MessageType::WarsTeamAction, &m, sizeof(m));
            g_notice.clear();
        }

        std::string Text(const char* a_s, size_t a_n) { return std::string(a_s, strnlen(a_s, a_n)); }

        // The choices: shown in the window and in F1. Under g_lock.
        void Draw()
        {
            const uint32_t me = NetworkClient::GetInstance().GetPlayerId();
            const auto mineIt = g_players.find(me);
            const Info mine = mineIt != g_players.end() ? mineIt->second : Info{};
            if (!g_haveMine) {
                ImGuiMCP::TextDisabled("Waiting for the server...");
                return;
            }
            // Where we stand.
            switch (mine.kind) {
            case 0: ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Pick your side before you head out."); break;
            case 1: ImGuiMCP::Text("You play SOLO: everyone else is fair game, and so are you."); break;
            case 2: ImGuiMCP::TextColored(Vec(kColours[mine.colour % 8]), "You're in [%s] %s", mine.tag.c_str(), mine.crewName.c_str()); break;
            case 3: ImGuiMCP::TextColored(Vec(kFactions[mine.faction % 4].rgb), "You side with the %s", kFactions[mine.faction % 4].name); break;
            }
            if (mine.kind && mine.cooldown > 0)
                ImGuiMCP::TextDisabled("You can change sides again in %u min.", (mine.cooldown + 59) / 60);
            if (!g_notice.empty()) ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", g_notice.c_str());

            ImGuiMCP::SeparatorText("Solo");
            if (mine.kind != 1 && ImGuiMCP::Button("Play solo")) Send(1);
            ImGuiMCP::SameLine();
            ImGuiMCP::TextDisabled("No crew, no faction. Everyone's hostile.");

            ImGuiMCP::SeparatorText("Side with a faction");
            for (int i = 0; i < 4; ++i) {
                ImGuiMCP::PushID(100 + i);
                const bool on = mine.kind == 3 && mine.faction == i;
                ImGuiMCP::BeginDisabled(on);
                if (ImGuiMCP::Button(kFactions[i].name)) Send(2, static_cast<uint8_t>(i));
                ImGuiMCP::EndDisabled();
                ImGuiMCP::SameLine();
                ImGuiMCP::TextDisabled("%s%s", on ? "(yours) " : "", kFactions[i].blurb);
                ImGuiMCP::PopID();
            }

            ImGuiMCP::SeparatorText("Crews");
            if (g_crews.empty()) ImGuiMCP::TextDisabled("No crews on this server yet: make the first one below.");
            for (auto& [id, c] : g_crews) {
                ImGuiMCP::PushID(static_cast<int>(id));
                ImGuiMCP::TextColored(Vec(kColours[c.colour % 8]), "[%s] %s", c.tag.c_str(), c.name.c_str());
                ImGuiMCP::SameLine();
                ImGuiMCP::TextDisabled("%u member%s, led by %s, %s", c.members, c.members == 1 ? "" : "s", c.leader.c_str(),
                    c.open ? "open" : "invite only");
                const bool ours = mine.kind == 2 && mine.crew == id;
                if (!ours) {
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::SmallButton(c.open ? "Join" : "Ask to join")) Send(4, 0, id);
                }
                if (ours && !c.requests.empty()) {   // we lead it (only leaders get the requests)
                    for (const auto& who : c.requests) {
                        ImGuiMCP::PushID(who.c_str());
                        ImGuiMCP::Text("   %s asks to join", who.c_str());
                        ImGuiMCP::SameLine();
                        if (ImGuiMCP::SmallButton("Let in")) Send(6, 0, 0, "", "", who.c_str());
                        ImGuiMCP::SameLine();
                        if (ImGuiMCP::SmallButton("Turn away")) Send(7, 0, 0, "", "", who.c_str());
                        ImGuiMCP::PopID();
                    }
                }
                ImGuiMCP::PopID();
            }
            if (mine.kind == 2) {
                const auto c = g_crews.find(mine.crew);
                if (c != g_crews.end() && c->second.leader == NetworkClient::GetInstance().GetPlayerName()) {
                    bool open = c->second.open;
                    if (ImGuiMCP::Checkbox("Anyone may join (else you let them in)", &open)) Send(8, 0, 0, "", "", "", open);
                }
                if (ImGuiMCP::Button("Leave the crew")) Send(5);
                ImGuiMCP::TextDisabled("Crewmates share captured places and a radio frequency, and can't hurt each other.");
            }

            ImGuiMCP::SeparatorText("Make a crew");
            static char s_name[32] = "", s_tag[6] = "";
            static int s_colour = 0;
            static bool s_open = true;
            ImGuiMCP::InputText("Name", s_name, sizeof(s_name));
            ImGuiMCP::InputText("Tag (shown before names, up to 5)", s_tag, sizeof(s_tag));
            for (int i = 0; i < 8; ++i) {
                ImGuiMCP::PushID(200 + i);
                if (i) ImGuiMCP::SameLine();
                if (ImGuiMCP::ColorButton(kColourNames[i], Vec(kColours[i]), 0, ImGuiMCP::ImVec2(28, 28))) s_colour = i;
                ImGuiMCP::PopID();
            }
            ImGuiMCP::TextColored(Vec(kColours[s_colour]), "Colour: %s - [%s] %s", kColourNames[s_colour], s_tag[0] ? s_tag : "TAG",
                NetworkClient::GetInstance().GetPlayerName().c_str());
            ImGuiMCP::Checkbox("Anyone may join", &s_open);
            if (ImGuiMCP::Button("Make the crew")) Send(3, static_cast<uint8_t>(s_colour), 0, s_name, s_tag, "", s_open);
        }

        void __stdcall RenderWindow()
        {
            if (!g_window) return;
            bool open = g_window->IsOpen.load();
            // Big enough for every choice without scrolling (it opened small: the framework scales it down).
            ImGuiMCP::SetNextWindowSizeConstraints(ImGuiMCP::ImVec2(900, 820), ImGuiMCP::ImVec2(4096, 4096), nullptr, nullptr);
            ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(1000, 900), ImGuiMCP::ImGuiCond_FirstUseEver);
            if (ImGuiMCP::Begin("Wasteland Wars: choose your side", &open, 0)) {
                std::lock_guard lock(g_lock);
                Draw();
                ImGuiMCP::Separator();
                const auto it = g_players.find(NetworkClient::GetInstance().GetPlayerId());
                const bool picked = it != g_players.end() && it->second.kind != 0;
                ImGuiMCP::BeginDisabled(!picked);
                if (ImGuiMCP::Button(picked ? "Head out" : "Pick a side first")) open = false;
                ImGuiMCP::EndDisabled();
                ImGuiMCP::SameLine();
                ImGuiMCP::TextDisabled("You can change this later in F1 (FathersFallout).");
            }
            ImGuiMCP::End();
            g_window->IsOpen = open;
        }

        // Our faction in the game: its people (and the A-Life squads spawned from them) take us for one of theirs;
        // the other three (Wars: the factions are everyone's enemy) are our enemies. Wars worlds only.
        void ApplyFaction(int a_faction)
        {
            if (a_faction == g_appliedFaction) return;
            constexpr uint32_t kPlayerFaction = 0x0001C21C;
            for (int i = 0; i < 4; ++i)
                for (uint32_t f : kFactions[i].factions) {
                    if (!f || i == a_faction) continue;
                    RE::Console::ExecuteCommand(std::format("player.removefromfaction {:X}", f).c_str());
                    RE::Console::ExecuteCommand(std::format("setenemy {:X} {:X}", f, kPlayerFaction).c_str());
                }
            if (a_faction >= 0)
                for (uint32_t f : kFactions[a_faction].factions) {
                    if (!f) continue;
                    RE::Console::ExecuteCommand(std::format("player.addtofaction {:X} 1", f).c_str());
                    RE::Console::ExecuteCommand(std::format("setally {:X} {:X} 1 1", f, kPlayerFaction).c_str());
                }
            REX::INFO("[Teams] our game faction: {}", a_faction >= 0 ? kFactions[a_faction].name : "none");
            g_appliedFaction = a_faction;
        }
    }

    void Init()
    {
        if (!Wars::Mode() || g_window || !F4SEMenuFramework::IsInstalled()) return;
        g_window = F4SEMenuFramework::AddWindow(RenderWindow, true);
    }

    void OnMessage(uint16_t a_type, const std::vector<uint8_t>& a_payload)
    {
        if (!Wars::Live()) return;
        std::lock_guard lock(g_lock);
        if (a_type == static_cast<uint16_t>(MessageType::WarsTeamInfo) && a_payload.size() >= sizeof(WarsTeamInfoMsg)) {
            WarsTeamInfoMsg m;
            std::memcpy(&m, a_payload.data(), sizeof(m));
            auto& p = g_players[m.player];
            p.kind = m.kind;
            p.faction = m.faction;
            p.colour = m.colour;
            p.crew = m.crew;
            p.cooldown = m.cooldown;
            p.tag = Text(m.tag, sizeof(m.tag));
            p.crewName = Text(m.crewName, sizeof(m.crewName));
            if (m.player == NetworkClient::GetInstance().GetPlayerId()) g_haveMine = true;
        } else if (a_type == static_cast<uint16_t>(MessageType::WarsCrew) && a_payload.size() >= sizeof(WarsCrewMsg)) {
            WarsCrewMsg m;
            std::memcpy(&m, a_payload.data(), sizeof(m));
            if (m.gone) { g_crews.erase(m.crew); return; }
            auto& c = g_crews[m.crew];
            c.colour = m.colour;
            c.open = m.open != 0;
            c.members = m.members;
            c.name = Text(m.name, sizeof(m.name));
            c.tag = Text(m.tag, sizeof(m.tag));
            c.leader = Text(m.leader, sizeof(m.leader));
            c.requests.clear();
            for (const auto& r : m.requests) if (r[0]) c.requests.push_back(Text(r, sizeof(r)));
        } else if (a_type == static_cast<uint16_t>(MessageType::WarsNotice) && a_payload.size() >= sizeof(WarsNoticeMsg)) {
            WarsNoticeMsg m;
            std::memcpy(&m, a_payload.data(), sizeof(m));
            g_notice = Text(m.text, sizeof(m.text));
            REX::INFO("[Teams] {}", g_notice);
            const std::string t = g_notice;
            if (auto* tasks = F4SE::GetTaskInterface())
                tasks->AddTask([t]() { RE::SendHUDMessage::ShowHUDMessage(t.c_str(), nullptr, false, false); });
        }
    }

    void Tick()
    {
        if (!Wars::Live()) return;
        Info mine;
        bool have;
        {
            std::lock_guard lock(g_lock);
            have = g_haveMine;
            const auto it = g_players.find(NetworkClient::GetInstance().GetPlayerId());
            if (it != g_players.end()) mine = it->second;
            // Crews that are gone (the server only sends the ones there are).
            for (auto it2 = g_crews.begin(); it2 != g_crews.end();) {
                bool used = false;
                for (auto& [id, p] : g_players) used |= p.kind == 2 && p.crew == it2->first;
                it2 = used ? std::next(it2) : g_crews.erase(it2);
            }
        }
        if (!have) return;
        // Not picked yet: the choice opens by itself (once; F1 has it after).
        if (mine.kind == 0 && g_window && !g_windowShown) {
            g_windowShown = true;
            g_window->IsOpen = true;
        }
        ApplyFaction(mine.kind == 3 ? mine.faction % 4 : -1);
        // A crew talks on its own frequency (anyone can still tune elsewhere in F1).
        if (mine.kind == 2 && mine.crew != g_tunedCrew) {
            g_tunedCrew = mine.crew;
            Voice::TuneTo(880 + static_cast<int>((mine.crew * 37u) % 200u));
        } else if (mine.kind != 2) {
            g_tunedCrew = 0;
        }
    }

    void Reset()
    {
        std::lock_guard lock(g_lock);
        g_players.clear();
        g_crews.clear();
        g_haveMine = false;
        g_appliedFaction = -2;   // set again on the new world
        g_tunedCrew = 0;
    }

    void RenderMenu()
    {
        ImGuiMCP::SeparatorText("Wasteland Wars: your side");
        std::lock_guard lock(g_lock);
        Draw();
    }

    bool TagOf(uint32_t a_player, std::string& a_tag, uint32_t& a_abgr)
    {
        std::lock_guard lock(g_lock);
        const auto it = g_players.find(a_player);
        if (it == g_players.end()) return false;
        const auto& p = it->second;
        if (p.kind == 2) { a_tag = "[" + p.tag + "] "; a_abgr = Abgr(kColours[p.colour % 8]); return true; }
        if (p.kind == 3) { a_tag = std::string("[") + kFactions[p.faction % 4].tag + "] "; a_abgr = Abgr(kFactions[p.faction % 4].rgb); return true; }
        return false;
    }

    bool Friendly(uint32_t a_player)
    {
        std::lock_guard lock(g_lock);
        const auto me = g_players.find(NetworkClient::GetInstance().GetPlayerId());
        const auto them = g_players.find(a_player);
        if (me == g_players.end() || them == g_players.end()) return false;
        const auto& a = me->second;
        const auto& b = them->second;
        return (a.kind == 2 && b.kind == 2 && a.crew == b.crew) || (a.kind == 3 && b.kind == 3 && a.faction == b.faction);
    }
}
