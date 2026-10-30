// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Dialogue.h"
#include "sync/Authority.h"
#include "sync/DynamicActors.h"
#include "network/NetworkClient.h"

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

    void Tick()
    {
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

    void Reset() { g_showing.clear(); }
}
