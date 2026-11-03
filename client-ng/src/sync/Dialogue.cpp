// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Dialogue.h"
#include "sync/Authority.h"
#include "sync/DynamicActors.h"
#include "network/NetworkClient.h"

#include <chrono>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace F4MP::Dialogue
{
    using namespace Network;

    namespace
    {
        template <class T>
        T* Lookup(uint32_t a_fid)
        {
            auto* f = a_fid ? RE::TESForm::GetFormByID(a_fid) : nullptr;
            return f ? f->As<T>() : nullptr;
        }

        // (speaker, line) pairs on the subtitle list last tick: a line goes out when it first shows up.
        std::set<std::pair<uint32_t, uint32_t>> g_showing;
        // Every line any NPC said here lately (ours or not), by (speaker, line): a scene that runs in both games
        // (Piper at Diamond City's gate with both players there) must not have its lines said twice here.
        std::map<std::pair<uint32_t, uint32_t>, std::chrono::steady_clock::time_point> g_heard;
        std::map<uint32_t, std::chrono::steady_clock::time_point> g_talking;   // speaker -> last seen speaking here
        std::map<std::pair<uint32_t, uint32_t>, std::chrono::steady_clock::time_point> g_replayed;   // lines we replayed (not "talking")

        // The line has a Papyrus fragment (TopicInfo script) bound: saying it here would run it again.
        bool HasFragment(RE::TESTopicInfo* a_info)
        {
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            if (!vm || !a_info) return false;
            auto* internal = static_cast<RE::BSScript::Internal::VirtualMachine*>(vm.get());
            const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<std::uint32_t>(a_info->GetFormType()), a_info);
            if (handle == vm->GetObjectHandlePolicy().EmptyHandle()) return false;
            RE::BSAutoLock l{ internal->attachedScriptsLock };
            const auto it = internal->attachedScripts.find(handle);
            return it != internal->attachedScripts.end() && !it->second.empty();
        }
    }

    namespace
    {
        // The game only lists a line (speaker + TopicInfo) while subtitles are on, and that list is how lines are
        // found here: with subtitles off no NPC line was ever sent
        // (a line is only listed after setini bDialogueSubtitles 1). So while connected both subtitle
        // settings are on in memory, and the HUD's subtitle text is hidden for a player who had them off. The
        // player's own values are back whenever the pause menu (Settings) is open, so they are what it shows and
        // saves, and when the session ends.
        constexpr const char* kSubtitleKeys[] = { "bDialogueSubtitles:Interface", "bGeneralSubtitles:Interface" };
        struct Pref { RE::Setting* setting = nullptr; bool user = false; };
        Pref g_prefs[2]{};
        bool g_forced = false;
        bool g_hudHidden = false;

        RE::Setting* FindSetting(const char* a_key)
        {
            if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton())
                if (auto* s = static_cast<RE::INISettingCollection*>(static_cast<void*>(prefs))->GetSetting(a_key)) return s;
            auto* ini = RE::INISettingCollection::GetSingleton();
            return ini ? ini->GetSetting(a_key) : nullptr;
        }

        void ShowHudSubtitles(bool a_show)
        {
            auto* ui = RE::UI::GetSingleton();
            auto menu = ui ? ui->GetMenu("HUDMenu") : nullptr;
            if (!menu || !menu->menuObj.IsObject()) return;
            Scaleform::GFx::Value group, text;
            if (!menu->menuObj.GetMember("BottomCenterGroup_mc", &group) || !group.IsObject()) return;
            if (!group.GetMember("SubtitleText_mc", &text) || !text.IsObject()) return;
            // The HUD's script sets "visible" again for every new line, so the text is
            // kept see-through instead, plus visible for good measure.
            const bool a = text.SetMember("alpha", Scaleform::GFx::Value(a_show ? 1.0 : 0.0));
            const bool v = text.SetMember("visible", Scaleform::GFx::Value(a_show));
            static int s_logged = 0;
            if (s_logged++ < 2) REX::INFO("[Dialogue] HUD subtitle text {}: alpha {} visible {}", a_show ? "shown" : "hidden", a, v);
        }

        void Release()
        {
            if (!g_forced) return;
            for (auto& p : g_prefs)
                if (p.setting) p.setting->SetBinary(p.user);
            if (g_hudHidden) ShowHudSubtitles(true);
            g_forced = false;
            g_hudHidden = false;
            REX::INFO("[Dialogue] subtitle settings back to the player's own");
        }

        void KeepListening()
        {
            auto* ui = RE::UI::GetSingleton();
            if (ui && (ui->GetMenuOpen("PauseMenu") || ui->GetMenuOpen("MainMenu"))) { Release(); return; }
            if (!g_forced) {
                bool userAll = true;
                for (int i = 0; i < 2; ++i) {
                    g_prefs[i].setting = FindSetting(kSubtitleKeys[i]);
                    g_prefs[i].user = g_prefs[i].setting && g_prefs[i].setting->GetBinary();
                    if (g_prefs[i].setting && !g_prefs[i].user) userAll = false;
                }
                if (userAll) return;   // already on: nothing to do
                for (auto& p : g_prefs)
                    if (p.setting) p.setting->SetBinary(true);
                g_forced = true;
                REX::INFO("[Dialogue] subtitles were off (dialogue {}, general {}): on in memory so NPC lines can be shared, text hidden",
                    g_prefs[0].user, g_prefs[1].user);
            }
            // Hidden only when the player had dialogue subtitles off; general-only players still see conversations.
            if (!g_prefs[0].user) { ShowHudSubtitles(false); g_hudHidden = true; }
        }
    }

    void Tick()
    {
        KeepListening();
        auto* subs = RE::SubtitleManager::GetSingleton();
        if (!subs) return;
        std::vector<std::pair<RE::NiPointer<RE::TESObjectREFR>, RE::TESTopicInfo*>> lines;
        {
            RE::BSAutoReadLock l{ RE::SubtitleManager::GetRWLock() };
            for (auto& s : subs->subtitlePriorityArray)
                if (s.topicInfo) lines.emplace_back(s.speaker.get(), s.topicInfo);
        }
        std::set<std::pair<uint32_t, uint32_t>> now;
        for (auto& [speaker, info] : lines) {
            if (!speaker) continue;
            const std::pair<uint32_t, uint32_t> key{ speaker->GetFormID(), info->GetFormID() };
            now.insert(key);
            g_heard[key] = std::chrono::steady_clock::now();
            // A line we replayed doesn't count as this NPC talking by itself: the owner's next line must still play.
            if (auto r = g_replayed.find(key); r == g_replayed.end() || std::chrono::steady_clock::now() - r->second > std::chrono::seconds(30))
                g_talking[key.first] = std::chrono::steady_clock::now();
            if (g_showing.contains(key)) continue;
            auto* actor = speaker->As<RE::Actor>();
            // Ours only: a copy saying a line we asked for must not send it back.
            if (!actor || actor == RE::PlayerCharacter::GetSingleton() || !Authority::IOwn(actor)) continue;
            auto* topic = info->parentTopic;
            if (!topic) continue;
            CoopNpcSayMsg msg{};
            msg.npcFormId = actor->GetFormID();
            msg.topicFormId = topic->GetFormID();
            msg.infoFormId = info->GetFormID();
            msg.hasFragment = HasFragment(info) ? 1 : 0;
            NetworkClient::GetInstance().SendPacket(MessageType::CoopNpcSay, &msg, sizeof(msg));
            static int s_logged = 0;
            if (s_logged++ < 60)
                REX::INFO("[Dialogue] our NPC {:#x} says {:#x} (topic {:#x}{}): sent", msg.npcFormId, msg.infoFormId, msg.topicFormId,
                    msg.hasFragment ? ", has a script fragment" : "");
        }
        g_showing = std::move(now);
        const auto t = std::chrono::steady_clock::now();
        std::erase_if(g_heard, [&](auto& e) { return t - e.second > std::chrono::seconds(30); });
        std::erase_if(g_talking, [&](auto& e) { return t - e.second > std::chrono::seconds(30); });
        std::erase_if(g_replayed, [&](auto& e) { return t - e.second > std::chrono::seconds(60); });
    }

    void OnSay(const CoopNpcSayMsg& m)
    {
        RE::Actor* npc = DynamicActors::ResolveNpc(m.senderId, m.npcFormId);
        auto* topic = Lookup<RE::TESTopic>(m.topicFormId);
        auto* info = Lookup<RE::TESTopicInfo>(m.infoFormId);
        static int s_logged = 0;
        if (!npc || !npc->Get3D() || npc->IsDead(false) || Authority::IOwn(npc) || !topic) {
            if (s_logged++ < 60) REX::INFO("[Dialogue] player {}'s NPC {:#x} says {:#x}: not here", m.senderId, m.npcFormId, m.infoFormId);
            return;
        }
        // Our game already said it (the same scene runs here too), or this NPC is talking here right now.
        {
            const auto t = std::chrono::steady_clock::now();
            auto heard = g_heard.find({ npc->GetFormID(), m.infoFormId });
            auto talking = g_talking.find(npc->GetFormID());
            const bool said = heard != g_heard.end() && t - heard->second < std::chrono::seconds(15);
            const bool busy = talking != g_talking.end() && t - talking->second < std::chrono::milliseconds(300);
            if (said || busy) {
                if (s_logged++ < 60)
                    REX::INFO("[Dialogue] player {}'s NPC {:#x} says {:#x}: {} here, not replayed", m.senderId, npc->GetFormID(), m.infoFormId,
                        said ? "already said" : "talking");
                return;
            }
            g_heard[{ npc->GetFormID(), m.infoFormId }] = t;   // our own game saying it a moment later is the copy then
            g_replayed[{ npc->GetFormID(), m.infoFormId }] = t;
        }
        // The sender's game knows best; ours may have bound the script only now.
        if (m.hasFragment || HasFragment(info)) {
            if (s_logged++ < 60)
                REX::INFO("[Dialogue] player {}'s NPC {:#x} says {:#x}: has a script fragment, not replayed", m.senderId, npc->GetFormID(), m.infoFormId);
            return;
        }
        auto* game = RE::GameVM::GetSingleton();
        auto vm = game ? game->GetVM() : nullptr;
        if (!vm) return;
        const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<std::uint32_t>(npc->GetFormType()), npc);
        if (handle == vm->GetObjectHandlePolicy().EmptyHandle()) return;
        const bool ok = vm->DispatchMethodCall(handle, "ObjectReference", "Say", nullptr, topic, static_cast<RE::Actor*>(nullptr), false,
            static_cast<RE::TESObjectREFR*>(nullptr));
        if (s_logged++ < 60)
            REX::INFO("[Dialogue] player {}'s NPC {:#x} says {:#x} (topic {:#x}) -> {}", m.senderId, npc->GetFormID(), m.infoFormId, m.topicFormId, ok);
    }

    void Reset()
    {
        g_showing.clear();
        g_heard.clear();
        g_talking.clear();
        g_replayed.clear();
        Release();
    }
}
