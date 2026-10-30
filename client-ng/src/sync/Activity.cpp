// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Activity.h"
#include "sync/Appearance.h"
#include "sync/Authority.h"
#include "sync/CoopSync.h"
#include "sync/DynamicActors.h"
#include "sync/GraphOverride.h"
#include "sync/Workshop.h"
#include "network/NetworkClient.h"
#include "sync/CallHooks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace F4MP::Activity
{
    using namespace Network;
    using Clock = std::chrono::steady_clock;

    namespace
    {
        constexpr uint32_t kRuntimeIdStart = 0xFF000000;
        // The frame a body gets when its player wears power armor: the plain frame without a fusion core
        // (PowerArmorFrameFurnitureNoCore), the player's own pieces go on it.
        constexpr uint32_t kBodyFrameBase = 0x0002079E;

        template <class T>
        T* Lookup(uint32_t a_fid)
        {
            auto* f = a_fid ? RE::TESForm::GetFormByID(a_fid) : nullptr;
            return f ? f->As<T>() : nullptr;
        }

        template <class T>
        void Send(MessageType a_type, const T& a_msg)
        {
            NetworkClient::GetInstance().SendPacket(a_type, &a_msg, sizeof(T));
        }

        bool Usable(RE::Actor* a) { return a && a->Get3D() && !a->IsDisabled() && !a->IsDead(false); }

        // ---- idles the game starts: every call to AIProcess::SetupSpecialIdle ----
        // middleHigh->currentIdle is set only for an instant while an idle starts: the 3/s check read 0 nearly
        // always.
        // The game's own calls (sandbox, idle markers, scenes; AI threads too) go through hooked call sites.
        struct Played { uint32_t idle = 0; uint32_t seq = 0; Clock::time_point at{}; };
        std::mutex g_playedLock;
        std::unordered_map<uint32_t, Played> g_played;   // actor FormID -> the last idle its game started
        std::atomic<int> g_playedLogged{ 0 };

        using SetupSpecialIdleFn = bool (*)(RE::AIProcess*, RE::Actor&, RE::DEFAULT_OBJECT, RE::TESIdleForm*, bool, RE::TESObjectREFR*);
        REL::Relocation<SetupSpecialIdleFn> g_setupSpecialIdle{ RE::ID::AIProcess::SetupSpecialIdle };

        bool SetupSpecialIdleHook(RE::AIProcess* a_proc, RE::Actor& a_actor, RE::DEFAULT_OBJECT a_do, RE::TESIdleForm* a_idle,
            bool a_test, RE::TESObjectREFR* a_target)
        {
            const bool ok = g_setupSpecialIdle(a_proc, a_actor, a_do, a_idle, a_test, a_target);
            if (ok && a_idle && a_idle->GetFormID() < 0xFF000000) {
                const uint32_t fid = a_actor.GetFormID();
                {
                    std::lock_guard l(g_playedLock);
                    auto& p = g_played[fid];
                    p.idle = a_idle->GetFormID();
                    ++p.seq;
                    p.at = Clock::now();
                    if (g_played.size() > 1024) g_played.clear();
                }
                if (g_playedLogged.fetch_add(1, std::memory_order_relaxed) < 120)
                    REX::INFO("[Activity] {:#x} started idle {:#x} ({})", fid, a_idle->GetFormID(), a_idle->GetFormEditorID());
            }
            return ok;
        }

        void InstallIdleHook()
        {
            std::size_t found = 0;
            const auto n = HookAllCalls(g_setupSpecialIdle.address(), SetupSpecialIdleHook, 48, &found);
            REX::INFO("[Activity] idle starts hooked at {} of {} call site(s)", n, found);
        }

        // ---- NPCs this game runs: what each was last said to be doing ----
        struct Sent { uint32_t seatOwner = 0, seatRef = 0, marker = 0, idle = 0, idleSeq = 0; Clock::time_point at{}; RE::NiPoint3 idlePos{}; };
        std::unordered_map<uint32_t, Sent> g_sent;   // our FormID -> last sent

        // ---- NPCs mirrored here: what their owner's copy is doing, and what we did about it ----
        struct Want
        {
            uint32_t owner = 0;
            uint32_t seat = 0, marker = 0;   // local furniture FormID (0 = none)
            uint32_t idle = 0, idleSeq = 0;
            Clock::time_point at{};          // last message
            uint32_t seated = 0;             // furniture we put it on
            int tries = 0;
            Clock::time_point nextTry{};
            uint32_t idlePlayed = 0, idleSeqPlayed = 0;
        };
        std::unordered_map<uint32_t, Want> g_want;   // local FormID -> wanted

        // ---- power armor: ours and the bodies' ----
        struct Armor { bool in = false; std::vector<uint32_t> pieces; Clock::time_point at{}; };
        Armor g_myArmor;
        Clock::time_point g_myArmorSent{};
        std::unordered_map<uint32_t, Armor> g_wantArmor;   // playerId -> its player's power armor
        struct BodyFrame { uint32_t body = 0; uint32_t frame = 0; int tries = 0; Clock::time_point nextTry{}; bool in = false;
                           Clock::time_point exitAt{}; };
        std::unordered_map<uint32_t, BodyFrame> g_bodyFrame;   // playerId -> the frame we gave its body
        // Frames left standing where a body got out (like the real one its player left, with its pieces on):
        // removed when that player gets into power armor again (they took it back) or after a while.
        struct LeftFrame { uint32_t pid = 0, frame = 0; Clock::time_point until{}; };
        std::vector<LeftFrame> g_leftFrames;
        std::unordered_map<uint32_t, std::vector<uint32_t>> g_lastPieces;   // playerId -> pieces worn last time in

        RE::BGSKeyword* ArmorKeyword() { return RE::PowerArmor::GetArmorKeyword(); }

        bool IsPowerArmorPiece(RE::TESForm* f)
        {
            auto* armo = f ? f->As<RE::TESObjectARMO>() : nullptr;
            auto* kw = ArmorKeyword();
            return armo && kw && static_cast<RE::BGSKeywordForm*>(armo)->HasKeyword(kw);
        }

        RE::Actor* BodyOf(uint32_t a_pid)
        {
            for (auto& [pid, fid] : GetBodies())
                if (pid == a_pid) return Lookup<RE::Actor>(fid);
            return nullptr;
        }

        void DeleteRef(uint32_t a_fid)
        {
            if (auto* r = Lookup<RE::TESObjectREFR>(a_fid)) {
                r->Disable();
                r->SetWantsDelete(true);
                r->MarkAsDeleted();
            }
        }
    }

    // ================= seats =================

    RE::TESObjectREFR* SeatOf(RE::Actor* a_actor, uint32_t* a_marker)
    {
        auto* proc = a_actor ? a_actor->currentProcess : nullptr;
        if (!proc) return nullptr;
        auto ref = proc->GetOccupiedFurniture().get();
        if (a_marker) *a_marker = proc->middleHigh ? proc->middleHigh->occupiedFurnitureMarkerID : 0;
        return ref.get();
    }

    bool Seat(RE::Actor* a_actor, RE::TESObjectREFR* a_furniture, uint32_t a_marker)
    {
        if (!a_actor || !a_furniture) return false;
        auto* cur = SeatOf(a_actor);
        if (cur == a_furniture) return true;
        if (cur) a_actor->StopInteractingQuick(false, false, true);   // on another one: off it first
        return a_actor->PutActorInChairBedQuick(a_furniture, a_marker);
    }

    void Unseat(RE::Actor* a_actor)
    {
        if (a_actor && SeatOf(a_actor)) a_actor->StopInteractingQuick(false, true, true);
    }

    bool PlayIdle(RE::Actor* a_actor, RE::TESIdleForm* a_idle)
    {
        auto* proc = a_actor ? a_actor->currentProcess : nullptr;
        if (!proc || !a_idle) return false;
        // A mirrored NPC drops the idles its own AI starts; this one is its owner's.
        GraphOverride::AllowIdles(a_actor->GetFormID(), 2000);
        GraphOverride::BeginOurs();
        // Conditions not tested: they were met in the game that runs it (an idle marker's, a package's...).
        const bool ok = proc->SetupSpecialIdle(*a_actor, RE::DEFAULT_OBJECT::kActionIdle, a_idle, false, nullptr);
        GraphOverride::EndOurs();
        return ok;
    }

    void Describe(RE::Actor* a)
    {
        if (!a) { REX::INFO("[Activity] no such actor"); return; }
        uint32_t marker = 0;
        auto* seat = SeatOf(a, &marker);
        auto* mh = a->currentProcess ? a->currentProcess->middleHigh : nullptr;
        auto cur = mh ? mh->currentFurniture.get() : RE::NiPointer<RE::TESObjectREFR>{};
        REX::INFO("[Activity] {:#x}: seat {:#x} marker {} (current {:#x} marker {}) idle {:#x} last idle {:#x} furniture idle {:#x} "
                  "anim played {} | power armor {} | pos {:.0f} {:.0f} {:.0f}",
            a->GetFormID(), seat ? seat->GetFormID() : 0u, marker, cur ? cur->GetFormID() : 0u, mh ? mh->currentFurnitureMarkerID : 0u,
            mh && mh->currentIdle ? mh->currentIdle->GetFormID() : 0u, mh && mh->lastIdlePlayed ? mh->lastIdlePlayed->GetFormID() : 0u,
            mh && mh->furnitureIdle ? mh->furnitureIdle->GetFormID() : 0u, mh && mh->furnitureAnimationPlayed,
            RE::PowerArmor::ActorInPowerArmor(*a), a->GetPosition().x, a->GetPosition().y, a->GetPosition().z);
    }

    // ================= NPCs: the owner sends =================

    static void SendNpcActivity()
    {
        auto* lists = RE::ProcessLists::GetSingleton();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!lists || !pl) return;
        const auto now = Clock::now();
        auto visit = [&](RE::BSTArray<RE::ActorHandle>& handles) {
            for (auto& h : handles) {
                auto np = h.get();
                RE::Actor* a = np.get();
                if (!a || a == pl || !Usable(a)) continue;
                const uint32_t fid = a->GetFormID();
                if (GetPlayerIdForBody(fid) || !Authority::IOwn(a)) continue;
                if (fid >= kRuntimeIdStart && DynamicActors::IsMirrorCopy(fid)) continue;
                Sent s{};
                uint32_t marker = 0;
                if (auto* furn = SeatOf(a, &marker); furn && Workshop::SeatKey(furn, s.seatOwner, s.seatRef)) s.marker = marker;
                // Idles outside furniture (a seat plays its own). Runtime idles don't exist elsewhere.
                // The last one its game started counts until it walks off from where it started it, or for 30 s.
                auto& last = g_sent[fid];
                if (!s.seatRef) {
                    Played p;
                    {
                        std::lock_guard l(g_playedLock);
                        if (auto it = g_played.find(fid); it != g_played.end()) p = it->second;
                    }
                    if (p.idle && now - p.at < std::chrono::seconds(30)) {
                        const auto pos = a->GetPosition();
                        const bool started = p.seq != last.idleSeq;
                        s.idleSeq = p.seq;
                        s.idlePos = started ? pos : last.idlePos;
                        const bool ended = !started && (!last.idle || pos.GetSquaredDistance(last.idlePos) > 60.0f * 60.0f);
                        if (!ended) s.idle = p.idle;
                    }
                }
                const bool same = last.seatOwner == s.seatOwner && last.seatRef == s.seatRef && last.marker == s.marker && last.idle == s.idle &&
                                  last.idleSeq == s.idleSeq;
                const bool busy = s.seatRef || s.idle;
                // On change; again every 3 s while busy (someone arriving later sees it too).
                if (same && (!busy || now - last.at < std::chrono::seconds(3))) continue;
                CoopNpcActivityMsg msg{};
                msg.npcFormId = fid;
                msg.seatOwnerId = s.seatOwner;
                msg.seatRefFormId = s.seatRef;
                msg.marker = s.marker;
                msg.idleFormId = s.idle;
                msg.idleSeq = s.idleSeq;
                Send(MessageType::CoopNpcActivity, msg);
                static int s_logged = 0;
                if (!same && s_logged++ < 80)
                    REX::INFO("[Activity] our NPC {:#x}: seat {:#x} marker {} idle {:#x} sent", fid, s.seatRef, s.marker, s.idle);
                s.at = now;
                last = s;
            }
        };
        visit(lists->highActorHandles);
        visit(lists->middleHighActorHandles);
        if (g_sent.size() > 1024) g_sent.clear();
    }

    void OnNpcActivity(const CoopNpcActivityMsg& m)
    {
        if (m.senderId == Authority::MyId()) return;
        RE::Actor* npc = DynamicActors::ResolveNpc(m.senderId, m.npcFormId);
        if (!npc || Authority::IOwn(npc)) return;
        auto& w = g_want[npc->GetFormID()];
        const uint32_t seat = m.seatRefFormId ? Workshop::LocalSeat(m.seatOwnerId, m.seatRefFormId) : 0;
        if (seat != w.seat || m.marker != w.marker) { w.tries = 0; w.nextTry = {}; }
        static int s_logged = 0;
        if ((seat != w.seat || m.idleFormId != w.idle) && s_logged++ < 80)
            REX::INFO("[Activity] player {}'s NPC {:#x}: seat {:#x} (here {:#x}) marker {} idle {:#x}", m.senderId, npc->GetFormID(),
                m.seatRefFormId, seat, m.marker, m.idleFormId);
        w.owner = m.senderId;
        w.seat = seat;
        w.marker = m.marker;
        w.idle = m.idleFormId;
        w.idleSeq = m.idleSeq;
        w.at = Clock::now();
    }

    // ================= NPCs: the copies do it =================

    static void ApplyNpcActivity()
    {
        const auto now = Clock::now();
        for (auto it = g_want.begin(); it != g_want.end();) {
            const uint32_t local = it->first;
            Want& w = it->second;
            auto* a = Lookup<RE::Actor>(local);
            // Ours now (the area moved to us), or gone: its own AI decides again.
            if (!a || Authority::IOwn(a) || a->IsDead(false)) { it = g_want.erase(it); continue; }
            if (!a->Get3D()) { w.seated = 0; ++it; continue; }
            // The owner stopped telling us (it went out of its range): nothing to show.
            const bool stale = now - w.at > std::chrono::seconds(12);
            const uint32_t wantSeat = stale ? 0 : w.seat;
            auto* cur = SeatOf(a);
            if (wantSeat) {
                auto* furn = Lookup<RE::TESObjectREFR>(wantSeat);
                if (furn && cur == furn) {
                    if (!w.seated) REX::INFO("[Activity] NPC {:#x} is on {:#x} like its owner's copy", local, wantSeat);
                    w.seated = wantSeat;
                    w.tries = 0;
                } else if (furn && furn->Get3D() && w.tries < 6 && now >= w.nextTry) {
                    // A seat someone else holds here (our own NPC on that chair) can't be taken: tried a few times.
                    const bool ok = Seat(a, furn, w.marker);
                    ++w.tries;
                    w.nextTry = now + std::chrono::milliseconds(1500);
                    if (ok) w.seated = wantSeat;
                    REX::INFO("[Activity] NPC {:#x}: onto {:#x} marker {} -> {} (try {})", local, wantSeat, w.marker, ok, w.tries);
                } else if (w.seated && cur != furn) {
                    w.seated = 0;   // got off again behind our back: retried
                }
            } else if (w.seated || (cur && w.at != Clock::time_point{})) {
                // Its owner's copy got up. A copy that sits straight back down (its own AI picked the chair)
                // is only asked a few times: every frame meant 900+ unseats in a row.
                if (w.seated) { w.tries = 0; w.nextTry = {}; }
                w.seated = 0;
                if (cur && w.tries < 6 && now >= w.nextTry) {
                    Unseat(a);
                    ++w.tries;
                    w.nextTry = now + std::chrono::milliseconds(1500);
                    REX::INFO("[Activity] NPC {:#x}: off its seat like its owner's copy (try {})", local, w.tries);
                }
            }
            // Idles outside furniture, as its owner's copy plays them.
            // The same idle started again over there (a smoker's next drag) is played again here.
            const uint32_t wantIdle = (stale || w.seated) ? 0 : w.idle;
            if (wantIdle != w.idlePlayed || (wantIdle && w.idleSeq != w.idleSeqPlayed)) {
                if (wantIdle) {
                    const bool ok = PlayIdle(a, Lookup<RE::TESIdleForm>(wantIdle));
                    static int s_logged = 0;
                    if (s_logged++ < 60) REX::INFO("[Activity] NPC {:#x}: idle {:#x} -> {}", local, wantIdle, ok);
                } else if (w.idlePlayed) {
                    GraphOverride::SendEvent(a, "IdleStop");
                }
                w.idlePlayed = wantIdle;
                w.idleSeqPlayed = w.idleSeq;
            }
            ++it;
        }
    }

    bool MirrorSeated(uint32_t a_local)
    {
        auto it = g_want.find(a_local);
        return it != g_want.end() && it->second.seated != 0;
    }

    // ================= power armor =================

    bool EnterPowerArmor(RE::Actor* a_actor, uint32_t a_frameBase, const uint32_t* a_pieces, int a_count)
    {
        auto* base = Lookup<RE::TESBoundObject>(a_frameBase ? a_frameBase : kBodyFrameBase);
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* cell = a_actor ? a_actor->GetParentCell() : nullptr;
        if (!a_actor || !base || !dh || !cell) return false;
        RE::NEW_REFR_DATA data;
        data.location = a_actor->GetPosition();
        data.direction = { 0.0f, 0.0f, a_actor->GetHeading() };
        data.object = base;
        if (cell->IsInterior()) data.interior = cell;
        else data.world = cell->worldSpace;
        auto frame = dh->CreateReferenceAtLocation(data).get();
        if (!frame) { REX::WARN("[Activity] power armor: no frame placed for {:#x}", a_actor->GetFormID()); return false; }
        for (int i = 0; i < a_count; ++i)
            if (auto* p = Lookup<RE::TESBoundObject>(a_pieces[i]))
                frame->AddObjectToContainer(p, nullptr, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
        const bool ok = a_actor->PutActorInChairBedQuick(frame.get(), 0);
        REX::INFO("[Activity] power armor: {:#x} into frame {:#x} ({} pieces) -> {} (in power armor now {})", a_actor->GetFormID(),
            frame->GetFormID(), a_count, ok, RE::PowerArmor::ActorInPowerArmor(*a_actor));
        for (auto& [pid, bf] : g_bodyFrame)
            if (bf.body == a_actor->GetFormID()) bf.frame = frame->GetFormID();
        return ok;
    }

    // The frame the body was in can't be shown again once the body is rebuilt (it stays as worn, hidden: the
    // known issue "the empty frame isn't shown"). A fresh one goes where the body stood, with the pieces on it.
    static uint32_t PlaceLeftFrame(RE::Actor* a_body, const std::vector<uint32_t>& a_pieces)
    {
        auto* base = Lookup<RE::TESBoundObject>(kBodyFrameBase);
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* cell = a_body ? a_body->GetParentCell() : nullptr;
        if (!base || !dh || !cell) return 0;
        RE::NEW_REFR_DATA data;
        data.location = a_body->GetPosition();
        data.direction = { 0.0f, 0.0f, a_body->GetHeading() };
        data.object = base;
        if (cell->IsInterior()) data.interior = cell;
        else data.world = cell->worldSpace;
        auto frame = dh->CreateReferenceAtLocation(data).get();
        if (!frame) return 0;
        for (auto fid : a_pieces)
            if (auto* p = Lookup<RE::TESBoundObject>(fid))
                frame->AddObjectToContainer(p, nullptr, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
        // Only to look at: getting in or taking its pieces would copy the other player's armor into our game.
        DynamicActors::BlockActivationOf(frame.get());
        return frame->GetFormID();
    }

    void ExitPowerArmor(RE::Actor* a_actor)
    {
        if (!a_actor) return;
        a_actor->StopInteractingQuick(false, true, true);
        REX::INFO("[Activity] power armor: {:#x} out (in power armor now {})", a_actor->GetFormID(), RE::PowerArmor::ActorInPowerArmor(*a_actor));
    }

    static void SendPowerArmor()
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return;
        Armor now{};
        now.in = RE::PowerArmor::PlayerInPowerArmor();
        if (now.in && pl->inventoryList) {
            for (auto& item : pl->inventoryList->data) {
                if (!item.object || !IsPowerArmorPiece(item.object) || item.object->GetFormID() >= kRuntimeIdStart) continue;
                for (auto* s = item.stackData.get(); s; s = s->nextStack.get())
                    if (s->IsEquipped()) { now.pieces.push_back(item.object->GetFormID()); break; }
            }
            std::sort(now.pieces.begin(), now.pieces.end());
        }
        const auto t = Clock::now();
        const bool same = now.in == g_myArmor.in && now.pieces == g_myArmor.pieces;
        if (same && (!now.in || t - g_myArmorSent < std::chrono::seconds(5))) return;
        if (same && !now.in) return;
        if (!same) REX::INFO("[Activity] we're {} power armor ({} pieces): sent", now.in ? "in" : "out of", now.pieces.size());
        g_myArmor = now;
        g_myArmorSent = t;
        CoopPowerArmorMsg msg{};
        msg.inArmor = now.in ? 1 : 0;
        msg.count = static_cast<uint8_t>(std::min<size_t>(now.pieces.size(), std::size(msg.pieces)));
        std::copy_n(now.pieces.begin(), msg.count, msg.pieces);
        Send(MessageType::CoopPowerArmor, msg);
    }

    void OnPowerArmor(const CoopPowerArmorMsg& m)
    {
        if (m.senderId == Authority::MyId()) return;
        auto& w = g_wantArmor[m.senderId];
        std::vector<uint32_t> pieces(m.pieces, m.pieces + std::min<uint8_t>(m.count, static_cast<uint8_t>(std::size(m.pieces))));
        std::erase(pieces, 0u);
        if (w.in != (m.inArmor != 0) || w.pieces != pieces)
            REX::INFO("[Activity] player {} is {} power armor ({} pieces)", m.senderId, m.inArmor ? "in" : "out of", pieces.size());
        if (m.inArmor && !pieces.empty()) g_lastPieces[m.senderId] = pieces;
        w.in = m.inArmor != 0;
        w.pieces = std::move(pieces);
        w.at = Clock::now();
    }

    static void ApplyPowerArmor()
    {
        const auto now = Clock::now();
        for (auto& [pid, want] : g_wantArmor) {
            auto* body = BodyOf(pid);
            auto& bf = g_bodyFrame[pid];
            if (!body || !body->Get3D()) continue;
            if (bf.body != body->GetFormID()) {
                // A new body (respawned, rebuilt): the old one's frame goes with it.
                if (bf.frame) DeleteRef(bf.frame);
                bf = BodyFrame{};
                bf.body = body->GetFormID();
            }
            const bool in = RE::PowerArmor::ActorInPowerArmor(*body);
            if (want.in && !in) {
                // Back in: the frame they left here is the one they took.
                std::erase_if(g_leftFrames, [&](const LeftFrame& f) {
                    if (f.pid != pid) return false;
                    DeleteRef(f.frame);
                    return true;
                });
                if (bf.tries >= 4 || now < bf.nextTry) continue;
                ++bf.tries;
                bf.nextTry = now + std::chrono::seconds(2);
                if (bf.frame) { DeleteRef(bf.frame); bf.frame = 0; }
                EnterPowerArmor(body, kBodyFrameBase, want.pieces.data(), static_cast<int>(want.pieces.size()));
            } else if (!want.in && in) {
                // Getting out: the engine's quick exit doesn't leave power armor (testing: still in after 100
                // tries), so the body is rebuilt fresh, out of it, and the frame stays where it stood.
                if (bf.exitAt == Clock::time_point{}) {
                    bf.exitAt = now;
                    ExitPowerArmor(body);
                } else if (now - bf.exitAt > std::chrono::milliseconds(500)) {
                    if (bf.frame) DeleteRef(bf.frame);
                    bf.frame = 0;
                    if (const uint32_t left = PlaceLeftFrame(body, g_lastPieces[pid])) {
                        g_leftFrames.push_back({ pid, left, now + std::chrono::minutes(15) });
                        if (g_leftFrames.size() > 8) { DeleteRef(g_leftFrames.front().frame); g_leftFrames.erase(g_leftFrames.begin()); }
                        REX::INFO("[Activity] player {}: frame {:#x} left standing ({} pieces)", pid, left, g_lastPieces[pid].size());
                    }
                    bf.exitAt = {};
                    Appearance::RequestRespawn(pid);
                    REX::INFO("[Activity] player {} got out of power armor: body rebuilt, frame left standing", pid);
                }
                bf.tries = 0;
            } else if (want.in && in) {
                bf.tries = 0;
            }
            bf.in = RE::PowerArmor::ActorInPowerArmor(*body);
            if (!want.in && !bf.in) bf.exitAt = {};
        }
        std::erase_if(g_leftFrames, [&](const LeftFrame& f) {
            if (now < f.until) return false;
            DeleteRef(f.frame);
            return true;
        });
    }

    bool BodyInPowerArmor(uint32_t a_pid)
    {
        auto it = g_bodyFrame.find(a_pid);
        return it != g_bodyFrame.end() && it->second.in;
    }

    // ================= tick =================

    void Tick()
    {
        static uint32_t s_tick = 0;
        ++s_tick;
        auto& settings = Coop::GetSettings();
        if (settings.combatAnims) {
            if (s_tick % 3 == 0) SendNpcActivity();   // ~3/s
            ApplyNpcActivity();
        }
        if (settings.equipment) {
            SendPowerArmor();
            ApplyPowerArmor();
        }
    }

    void InstallHooks() { InstallIdleHook(); }

    void Reset()
    {
        g_sent.clear();
        g_want.clear();
        g_myArmor = {};
        g_myArmorSent = {};
        g_wantArmor.clear();
        for (auto& [pid, bf] : g_bodyFrame)
            if (bf.frame) DeleteRef(bf.frame);
        g_bodyFrame.clear();
        for (auto& f : g_leftFrames) DeleteRef(f.frame);
        g_leftFrames.clear();
    }
}
