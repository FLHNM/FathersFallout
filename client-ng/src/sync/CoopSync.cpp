// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/CoopSync.h"
#include "sync/GraphOverride.h"
#include "sync/PlayerEvents.h"
#include "network/NetworkClient.h"
#include "sync/Appearance.h"
#include "sync/Explosions.h"
#include "sync/DynamicActors.h"
#include "sync/Authority.h"
#include "sync/Range.h"
#include "sync/Workshop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace F4MP::Coop
{
    using namespace Network;
    using Clock = std::chrono::steady_clock;

    namespace
    {
        constexpr uint32_t kPlayerRefId = 0x14;
        constexpr uint32_t kDynamicFormIdStart = 0xFF000000;  // runtime refs: IDs differ per machine

        Settings g_settings;
        Stats g_stats;

        // ---------- helpers ----------

        template <class T>
        void Send(MessageType type, const T& msg)
        {
            NetworkClient::GetInstance().SendPacket(type, &msg, sizeof(T));
            g_stats.sent++;
        }

        template <class T>
        const T* As(const CoopInboxItem& item)
        {
            return item.payload.size() >= sizeof(T) ? reinterpret_cast<const T*>(item.payload.data()) : nullptr;
        }

        template <class T>
        T* LookupForm(uint32_t formId)
        {
            auto* form = formId ? RE::TESForm::GetFormByID(formId) : nullptr;
            return form ? form->As<T>() : nullptr;
        }

        // The object mods attached to an item reference (its instance data), for sending.
        uint8_t ReadMods(const RE::ExtraDataList* extra, CoopModEntry* out)
        {
            if (!extra) return 0;
            auto* inst = extra->GetByType<RE::BGSObjectInstanceExtra>();
            if (!inst || !inst->values) return 0;
            uint8_t n = 0;
            for (auto& d : inst->GetIndexData()) {
                if (n >= kMaxItemMods) break;
                if (d.disabled || !d.objectID) continue;
                out[n++] = { d.objectID, d.index, d.rank, {} };
            }
            return n;
        }

        // Puts the sender's mods on our copy (it was created with the base object's defaults).
        void ApplyMods(RE::TESObjectREFR* ref, const CoopModEntry* mods, uint8_t count)
        {
            if (!ref || !count) return;
            int ok = 0;
            for (uint8_t i = 0; i < std::min<uint8_t>(count, kMaxItemMods); ++i) {
                auto* mod = LookupForm<RE::BGSMod::Attachment::Mod>(mods[i].modFormId);
                if (mod && RE::BGSObjectInstanceExtra::AttachModToReference(*ref, *mod, mods[i].index, mods[i].rank)) ++ok;
            }
            REX::INFO("[Coop] copy {:#x}: {}/{} mods attached", ref->GetFormID(), ok, count);
        }

        RE::ActorValueInfo* HealthAV()
        {
            auto* avs = RE::ActorValue::GetSingleton();
            return avs ? avs->health : nullptr;
        }

        bool IsSyncableRef(uint32_t formId)
        {
            return formId != 0 && formId != kPlayerRefId && formId < kDynamicFormIdStart &&
                   GetPlayerIdForBody(formId) == 0;
        }

        // A container/corpse as every game can name it: plugin references by FormID (owner 0); a spawned
        // actor's corpse (runtime FormIDs differ per game) by the game that spawned it + its FormID there.
        // Without this, random encounters' corpses weren't synced at all: both players looted everything.
        bool NameRef(uint32_t localFid, uint32_t& owner, uint32_t& ref)
        {
            if (IsSyncableRef(localFid)) { owner = 0; ref = localFid; return true; }
            if (localFid < kDynamicFormIdStart || GetPlayerIdForBody(localFid)) return false;
            if (DynamicActors::OwnerOf(localFid, owner, ref)) return true;
            auto* a = LookupForm<RE::Actor>(localFid);   // one of OUR spawns (announced as ours)
            if (!a) return false;
            owner = NetworkClient::GetInstance().GetPlayerId();
            ref = localFid;
            return true;
        }

        RE::TESObjectREFR* ResolveRef(uint32_t owner, uint32_t ref)
        {
            if (owner == 0 || owner == NetworkClient::GetInstance().GetPlayerId()) return LookupForm<RE::TESObjectREFR>(ref);
            return DynamicActors::Resolve(owner, ref);
        }

        // Base FormIDs of equipped weapons and armor, sorted.
        std::vector<uint32_t> CollectEquipped(RE::TESObjectREFR* ref)
        {
            std::vector<uint32_t> out;
            auto* inv = ref ? ref->inventoryList : nullptr;
            if (!inv) return out;
            for (auto& item : inv->data) {
                if (!item.object) continue;
                const auto type = item.object->GetFormType();
                if (type != RE::ENUM_FORM_ID::kWEAP && type != RE::ENUM_FORM_ID::kARMO) continue;
                for (auto* s = item.stackData.get(); s; s = s->nextStack.get()) {
                    if (s->IsEquipped()) {
                        out.push_back(item.object->GetFormID());
                        break;
                    }
                }
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

        std::unordered_map<uint32_t, int32_t> CollectContents(RE::TESObjectREFR* ref)
        {
            std::unordered_map<uint32_t, int32_t> out;
            auto* inv = ref ? ref->inventoryList : nullptr;
            if (!inv) return out;
            for (auto& item : inv->data) {
                if (!item.object) continue;
                int32_t n = 0;
                for (auto* s = item.stackData.get(); s; s = s->nextStack.get()) n += static_cast<int32_t>(s->count);
                if (n > 0) out[item.object->GetFormID()] += n;
            }
            return out;
        }

        void AddItems(RE::TESObjectREFR* container, RE::TESBoundObject* obj, int32_t count)
        {
            container->AddObjectToContainer(obj, nullptr, count, nullptr, RE::ITEM_REMOVE_REASON::kNone);
        }

        void RemoveItems(RE::TESObjectREFR* container, RE::TESBoundObject* obj, int32_t count)
        {
            RE::TESObjectREFR::RemoveItemData data(obj, count);  // no otherContainer: item is destroyed
            container->RemoveItem(data);
        }

        // ---------- equipment ----------

        // The equipped weapon and its mods (stack instance data), or {0, 0} when none / unmodded.
        uint32_t ReadEquippedWeaponMods(RE::TESObjectREFR* ref, CoopModEntry* out, uint8_t& count)
        {
            count = 0;
            auto* inv = ref ? ref->inventoryList : nullptr;
            if (!inv) return 0;
            for (auto& item : inv->data) {
                if (!item.object || item.object->GetFormType() != RE::ENUM_FORM_ID::kWEAP) continue;
                for (auto* s = item.stackData.get(); s; s = s->nextStack.get())
                    if (s->IsEquipped()) {
                        count = ReadMods(s->extra.get(), out);
                        return item.object->GetFormID();
                    }
            }
            return 0;
        }

        // Mods as a comparable list (resend / re-equip when they change).
        std::vector<uint32_t> ModKey(uint32_t weapon, const CoopModEntry* mods, uint8_t count)
        {
            std::vector<uint32_t> k{ weapon };
            for (uint8_t i = 0; i < count; ++i) k.push_back(mods[i].modFormId ^ (uint32_t(mods[i].index) << 24));
            return k;
        }

        std::vector<uint32_t> g_lastSentEquip;
        std::vector<uint32_t> g_lastSentMods;
        Clock::time_point g_lastEquipSend{};
        std::unordered_map<uint32_t, std::vector<uint32_t>> g_wantEquip;  // playerId -> forms
        struct WantMods { uint32_t weapon = 0; uint8_t count = 0; CoopModEntry mods[kMaxItemMods]{}; };
        std::unordered_map<uint32_t, WantMods> g_wantMods;                   // playerId -> weapon mods
        std::unordered_map<uint32_t, std::vector<uint32_t>> g_appliedMods;   // body FormID -> ModKey applied

        // What we last put on each body (body FormID -> sorted forms). The engine's equipped
        // flags on inventory stacks are not set for NPC bodies (verified in game: always empty),
        // so we track our own changes instead of reading equip state back.
        std::unordered_map<uint32_t, std::vector<uint32_t>> g_appliedEquip;

        void PollLocalEquipment()
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!pl) return;
            auto equipped = CollectEquipped(pl);
            CoopEquipmentMsg msg{};
            msg.weaponFormId = ReadEquippedWeaponMods(pl, msg.weaponMods, msg.numWeaponMods);
            auto mods = ModKey(msg.weaponFormId, msg.weaponMods, msg.numWeaponMods);
            const auto now = Clock::now();
            // Resend every 5 s so late joiners and respawned bodies catch up.
            if (equipped == g_lastSentEquip && mods == g_lastSentMods && now - g_lastEquipSend < std::chrono::seconds(5)) return;
            if (mods != g_lastSentMods)
                REX::INFO("[Coop] our weapon {:08X}: {} mod(s) sent", msg.weaponFormId, msg.numWeaponMods);
            g_lastSentEquip = equipped;
            g_lastSentMods = std::move(mods);
            g_lastEquipSend = now;

            msg.count = static_cast<uint8_t>(std::min<size_t>(equipped.size(), std::size(msg.forms)));
            for (uint8_t i = 0; i < msg.count; ++i) msg.forms[i] = equipped[i];
            Send(MessageType::CoopEquipment, msg);
        }

        // A weapon with its player's mods (pipe pistol, not the base pipe gun's SMG). Same route as
        // dropped items: a world copy gets the mods, then the body picks it up like an NPC would.
        bool GiveModdedWeapon(RE::Actor* body, RE::TESBoundObject* obj, const WantMods& wm)
        {
            AddItems(body, obj, 1);
            RE::TESObjectREFR::RemoveItemData data(obj, 1);
            data.reason = RE::ITEM_REMOVE_REASON::KDropping;
            const RE::NiPoint3 at = body->GetPosition();
            data.dropLoc = &at;
            auto ref = body->RemoveItem(data).get();
            if (!ref) {
                REX::WARN("[Coop] body {:#x}: couldn't place {:08X} to put its mods on", body->GetFormID(), obj->GetFormID());
                AddItems(body, obj, 1);
                return false;
            }
            ApplyMods(ref.get(), wm.mods, wm.count);
            const bool taken = ref->ActivateRef(body, nullptr, 1, false, false, false);
            if (!taken || body->GetInventoryObjectCount(obj) == 0) {
                REX::WARN("[Coop] body {:#x}: picking up modded {:08X} failed; plain one instead", body->GetFormID(), obj->GetFormID());
                ref->Disable();
                ref->SetDelete(true);
                AddItems(body, obj, 1);
                return false;
            }
            return true;
        }

        // The instance data of a body's stack of this object (a modded weapon's), so EquipObject
        // picks that stack: equipping the bare base object found no match and left the hands empty.
        RE::TBO_InstanceData* StackInstance(RE::Actor* body, RE::TESBoundObject* obj, uint32_t& stackId)
        {
            auto* inv = body ? body->inventoryList : nullptr;
            if (!inv) return nullptr;
            for (auto& item : inv->data) {
                if (item.object != obj) continue;
                uint32_t i = 0;
                for (auto* s = item.stackData.get(); s; s = s->nextStack.get(), ++i) {
                    if (!s->extra) continue;
                    auto* d = s->extra->GetByType<RE::ExtraInstanceData>();
                    if (!d || !d->data) {   // not built yet: the game makes it from the stack's mods
                        RE::BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra(*obj, *s->extra, nullptr, false);
                        d = s->extra->GetByType<RE::ExtraInstanceData>();
                    }
                    if (d && d->data) { stackId = i; return d->data.get(); }
                }
            }
            return nullptr;
        }

        void ApplyEquipmentToBodies()
        {
            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return;
            for (auto& [pid, bodyId] : GetBodies()) {
                auto want = g_wantEquip.find(pid);
                if (want == g_wantEquip.end()) continue;
                auto* body = LookupForm<RE::Actor>(bodyId);
                if (!body || !body->Get3D()) continue;

                const WantMods wm = g_wantMods[pid];
                const auto modKey = ModKey(wm.weapon, wm.mods, wm.count);
                auto applied = g_appliedEquip.find(bodyId);
                const bool modsChanged = g_appliedMods[bodyId] != modKey;
                if (applied != g_appliedEquip.end() && applied->second == want->second && !modsChanged) continue;
                const auto& w = want->second;
                auto wanted = [&](uint32_t fid) { return std::binary_search(w.begin(), w.end(), fid); };

                // Swapping a weapon while one is drawn can leave the old model in the hands (bat and
                // pistol at once). Put it away first; the body sync draws again if its player has
                // a weapon out.
                GraphOverride::AllowWeaponSwap(body);
                if (body->GetWeaponMagicDrawn()) body->DrawWeaponMagicHands(false);

                // Clothing taken off with nothing put on in its place: the item left the inventory but
                // the body kept wearing its model until the next change (the host
                // stripped, the guest still saw the jumpsuit). A 3D rebuild shows what's really worn.
                bool armorRemoved = false;
                if (applied == g_appliedEquip.end()) {
                    // First time: the body is a copy of the local player and wears the local
                    // player's outfit. Strip every weapon/armor it shouldn't have; taking an
                    // item out of the inventory also unequips it.
                    for (auto& [fid, n] : CollectContents(body)) {
                        auto* obj = LookupForm<RE::TESBoundObject>(fid);
                        if (!obj || wanted(fid)) continue;
                        const auto type = obj->GetFormType();
                        if (type == RE::ENUM_FORM_ID::kWEAP || type == RE::ENUM_FORM_ID::kARMO) RemoveItems(body, obj, n);
                    }
                } else {
                    for (uint32_t fid : applied->second) {
                        if (wanted(fid)) continue;
                        auto* obj = LookupForm<RE::TESBoundObject>(fid);
                        if (!obj) continue;
                        const auto n = static_cast<int32_t>(body->GetInventoryObjectCount(obj));
                        if (n > 0) RemoveItems(body, obj, n);
                        if (obj->GetFormType() == RE::ENUM_FORM_ID::kARMO) armorRemoved = true;
                    }
                }

                std::vector<uint32_t> before = applied == g_appliedEquip.end() ? std::vector<uint32_t>{} : applied->second;
                // Same weapon, other mods: take the old one away so it's given again with the new ones.
                const bool remod = modsChanged && wm.weapon && std::binary_search(w.begin(), w.end(), wm.weapon);
                if (remod && std::binary_search(before.begin(), before.end(), wm.weapon)) {
                    if (auto* old = LookupForm<RE::TESBoundObject>(wm.weapon)) {
                        const auto n = static_cast<int32_t>(body->GetInventoryObjectCount(old));
                        if (n > 0) RemoveItems(body, old, n);
                    }
                    std::erase(before, wm.weapon);
                }
                // Did the weapon change? Then the hand model has to be rebuilt (EquipObject alone
                // leaves the old weapon's 3D in the hand).
                auto weapons = [](const std::vector<uint32_t>& v) {
                    std::vector<uint32_t> out;
                    for (auto f : v)
                        if (auto* o = LookupForm<RE::TESForm>(f); o && o->GetFormType() == RE::ENUM_FORM_ID::kWEAP) out.push_back(f);
                    return out;
                };
                bool weaponChanged = applied != g_appliedEquip.end() && weapons(before) != weapons(w);
                for (uint32_t fid : w) {
                    if (std::binary_search(before.begin(), before.end(), fid)) continue;
                    auto* obj = LookupForm<RE::TESBoundObject>(fid);
                    if (!obj) continue;
                    if (fid == wm.weapon && wm.count > 0) {
                        const auto n = static_cast<int32_t>(body->GetInventoryObjectCount(obj));
                        if (n > 0) RemoveItems(body, obj, n);   // an unmodded copy from before
                        const bool ok = GiveModdedWeapon(body, obj, wm);
                        REX::INFO("[Coop] body {:#x} (player {}): weapon {:08X} with {} mod(s): {}", bodyId, pid, fid, wm.count,
                            ok ? "given" : "fell back to the plain one");
                        weaponChanged = true;   // rebuild the hand model with the mods
                    } else if (body->GetInventoryObjectCount(obj) == 0) {
                        AddItems(body, obj, 1);
                    }
                    uint32_t stackId = 0;
                    RE::TBO_InstanceData* data = (fid == wm.weapon && wm.count > 0) ? StackInstance(body, obj, stackId) : nullptr;
                    RE::BGSObjectInstance inst(obj, data);
                    const bool equipped = mgr->EquipObject(body, inst, stackId, 1, nullptr, false, true, false, true, false);
                    if (fid == wm.weapon && wm.count > 0)
                        REX::INFO("[Coop] body {:#x}: modded {:08X} equip (stack {}, instance {}) -> {}", bodyId, fid, stackId,
                            data != nullptr, equipped);
                }
                if (weaponChanged || armorRemoved) body->Reset3D(false, 0, true, 0);
                g_appliedEquip[bodyId] = w;
                g_appliedMods[bodyId] = modKey;
                g_stats.applied++;
                REX::INFO("[Coop] body {:#x} (player {}) equipment applied: {} items", bodyId, pid, w.size());
                auto list = [](const std::vector<uint32_t>& v) {
                    std::string out;
                    for (auto f : v) { char b[12]; std::snprintf(b, sizeof(b), " %08X", f); out += b; }
                    return out;
                };
                REX::INFO("[Coop]   wanted:{} | body now has equipped:{}", list(w), list(CollectEquipped(body)));
            }
        }

        // ---------- combat ----------

        // Client: the health we last forced onto each driven NPC.
        std::unordered_map<uint32_t, float> g_npcAppliedHealth;
        // Client: driven NPC (local FormID) -> its owner and its FormID in the owner's game.
        struct NpcOwner { uint32_t ownerId; uint32_t formId; Clock::time_point at; };
        std::unordered_map<uint32_t, NpcOwner> g_npcOwners;
        // Keeps bodies alive and full: their player's health lives in their own game. The damage
        // itself is forwarded per hit by HitSink (measuring health drops counted physics bumps and,
        // whenever the body's maximum moved, the same "loss" again every tick).
        // A borrowed FaceGen head brings its neck gore cap (shown only after a decapitation on
        // real NPCs); on a body it drew as a red band around the neck. Hide it whenever a body's
        // 3D is (re)built.
        std::unordered_map<uint32_t, RE::NiAVObject*> g_bodyGoreChecked;

        void HideGoreCaps(RE::Actor* body)
        {
            auto* root = body ? body->Get3D() : nullptr;
            if (!root) return;
            auto& seen = g_bodyGoreChecked[body->GetFormID()];
            if (seen == root) return;
            seen = root;
            int hidden = 0;
            std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* o, int depth) {
                if (!o || depth > 40) return;
                const char* nm = o->GetName().c_str();
                if (nm && std::strstr(nm, "Gore") && !o->GetAppCulled()) { o->SetAppCulled(true); ++hidden; }
                if (auto* node = o->IsNode())
                    for (auto& child : node->children) walk(child.get(), depth + 1);
            };
            walk(root, 0);
            if (hidden) REX::INFO("[Coop] body {:#x}: hid {} gore cap(s)", body->GetFormID(), hidden);
        }

        // A body that exists (it can be targeted) but is never drawn: the joiner in the first real
        // test saw only the host's name tag after spawning it right at the end of a fast-travel
        // load. Log each body's render state now and then, undo a stuck fade-in or cull, and
        // replace a body whose 3D never shows up.
        struct BodyVis { Clock::time_point firstSeen{}, no3DSince{}, lastLog{}; bool no3D = false; };
        std::unordered_map<uint32_t, BodyVis> g_bodyVis;

        void WatchBodyVisibility(uint32_t pid, RE::Actor* body)
        {
            if (!body) return;
            const auto now = Clock::now();
            auto& v = g_bodyVis[body->GetFormID()];
            if (v.firstSeen == Clock::time_point{}) v.firstSeen = now;
            auto* root = body->Get3D();
            auto* fade = root ? root->IsFadeNode() : nullptr;
            auto* cell = body->GetParentCell();
            if (now - v.lastLog > std::chrono::seconds(10)) {
                v.lastLog = now;
                REX::INFO("[Body] player {} {:#x}: 3D {} culled {} fade {:.2f} disabled {} cell {:#x} attached {}", pid,
                    body->GetFormID(), root != nullptr, root && root->GetAppCulled(), fade ? fade->currentFade : -1.0f,
                    body->IsDisabled(), cell ? cell->GetFormID() : 0u, cell && cell->IsAttached());
            }
            if (!root) {
                if (!v.no3D) { v.no3D = true; v.no3DSince = now; }
                if (now - v.no3DSince > std::chrono::seconds(6) && cell && cell->IsAttached()) {
                    REX::WARN("[Body] player {}: body {:#x} has had no 3D for 6 s in a loaded cell, replacing it", pid, body->GetFormID());
                    v = {};
                    Appearance::RequestRespawn(pid);
                }
                return;
            }
            v.no3D = false;
            if (root->GetAppCulled()) {
                root->SetAppCulled(false);
                REX::WARN("[Body] player {}: body 3D was culled, shown again", pid);
            }
            if (fade && fade->currentFade < 0.5f && now - v.firstSeen > std::chrono::seconds(4)) {
                REX::WARN("[Body] player {}: body stuck faded out ({:.2f}), forced visible", pid, fade->currentFade);
                fade->currentFade = 1.0f;
            }
        }

        void WatchBodyDamage()
        {
            auto* hp = HealthAV();
            if (!hp) return;
            for (auto& [pid, bodyId] : GetBodies()) {
                auto* body = LookupForm<RE::Actor>(bodyId);
                if (!body) continue;
                if (body->IsDead(false)) continue;   // main.cpp replaces it with a fresh body
                if (body->GetActorValue(*hp) < 50000.0f)
                    body->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, 100000.0f);  // capped at full
            }
        }

        void OnPlayerDamage(const CoopPlayerDamageMsg& m)
        {
            auto& net = NetworkClient::GetInstance();
            if (m.targetPlayerId != net.GetPlayerId()) return;
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* hp = HealthAV();
            if (!pl || !hp || m.damage <= 0.0f || m.damage > 100000.0f) return;
            pl->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -m.damage);
            g_stats.applied++;
            const float left = pl->GetActorValue(*hp);
            REX::INFO("[Coop] player {} hit us for {:.1f} ({:.0f} health left)", m.senderId, m.damage, left);
            // Health alone can sit at 0 without the player dying; the real hit would have killed.
            if (left <= 0.0f && !pl->IsDead(false)) {
                RE::Actor* attacker = nullptr;
                for (auto& [pid, bodyId] : GetBodies())
                    if (pid == m.senderId) attacker = LookupForm<RE::Actor>(bodyId);
                pl->KillImpl(attacker, m.damage, true, false);
                REX::INFO("[Coop] killed by player {}", m.senderId);
            }
        }

        void OnNpcDamage(const CoopNpcDamageMsg& m)
        {
            if (m.ownerId != Authority::MyId()) return;   // the NPC's owner applies hits
            auto* npc = LookupForm<RE::Actor>(m.npcFormId);
            auto* hp = HealthAV();
            if (!npc || !hp || npc->IsDead(false) || m.damage <= 0.0f || m.damage > 100000.0f) return;
            npc->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -m.damage);
            if (npc->GetActorValue(*hp) <= 0.0f && !npc->IsDead(false)) {
                // Credit the attacking player's body so aggro/kill credit points at someone real.
                RE::Actor* attacker = nullptr;
                for (auto& [pid, bodyId] : GetBodies())
                    if (pid == m.senderId) attacker = LookupForm<RE::Actor>(bodyId);
                npc->KillImpl(attacker, m.damage, true, false);
            }
            g_stats.applied++;
            REX::INFO("[Coop] player {} hit our NPC {:#x} for {:.1f}", m.senderId, m.npcFormId, m.damage);
        }

        // ---------- items ----------

        std::unordered_set<uint32_t> g_pendingSnapshot;  // containers we asked the host about

        void OnContainerChange(const CoopContainerChangeMsg& m)
        {
            auto* cont = ResolveRef(m.ownerId, m.containerFormId);
            auto* obj = LookupForm<RE::TESBoundObject>(m.itemFormId);
            if (!cont || !obj || m.count == 0) return;
            if (m.count > 0) {
                AddItems(cont, obj, m.count);
            } else {
                const int32_t have = static_cast<int32_t>(cont->GetInventoryObjectCount(obj));
                const int32_t take = std::min(have, -m.count);
                if (take > 0) RemoveItems(cont, obj, take);
            }
            g_stats.containerOps++;
            g_stats.applied++;
        }

        void OnWorldRefTaken(const CoopWorldRefTakenMsg& m)
        {
            auto* ref = LookupForm<RE::TESObjectREFR>(m.refFormId);
            if (!ref || ref->IsDisabled()) return;
            // The host's world is the real one: an item taken there is gone. A guest only hides it while
            // connected (its own save gets it back), like the host's scrapped objects.
            if (IsHost()) ref->Disable();
            else Workshop::HideRef(ref);
            g_stats.applied++;
        }

        void OnSnapshotRequest(const CoopContainerSnapshotReqMsg& m)
        {
            auto* cont = ResolveRef(m.ownerId, m.containerFormId);
            if (!cont || !cont->Get3D() || !Authority::IOwn(cont)) return;   // the container's owner answers
            CoopContainerSnapshotMsg out{};
            out.containerFormId = m.containerFormId;
            out.ownerId = m.ownerId;
            for (auto& [fid, n] : CollectContents(cont)) {
                if (out.count >= std::size(out.entries)) break;
                out.entries[out.count++] = { fid, n };
            }
            Send(MessageType::CoopContainerSnapshot, out);
        }

        void OnSnapshot(const CoopContainerSnapshotMsg& m)
        {
            auto* cont = ResolveRef(m.ownerId, m.containerFormId);
            if (!cont || Authority::IOwn(cont)) return;   // the owner already has the real contents
            std::unordered_map<uint32_t, int32_t> want;
            const uint16_t n = std::min<uint16_t>(m.count, static_cast<uint16_t>(std::size(m.entries)));
            for (uint16_t i = 0; i < n; ++i) want[m.entries[i].itemFormId] += m.entries[i].count;

            const auto have = CollectContents(cont);
            for (auto& [fid, cnt] : have) {
                auto* obj = LookupForm<RE::TESBoundObject>(fid);
                const auto w = want.find(fid);
                const int32_t target = w == want.end() ? 0 : w->second;
                if (obj && cnt > target) RemoveItems(cont, obj, cnt - target);
            }
            for (auto& [fid, cnt] : want) {
                auto* obj = LookupForm<RE::TESBoundObject>(fid);
                const auto h = have.find(fid);
                const int32_t cur = h == have.end() ? 0 : h->second;
                if (obj && cnt > cur) AddItems(cont, obj, cnt - cur);
            }
            g_pendingSnapshot.erase(cont->GetFormID());
            g_stats.containerOps++;
            g_stats.applied++;
            REX::INFO("[Coop] container {:#x} matched to host ({} stacks)", m.containerFormId, n);
        }

        // ---------- load-order fingerprint ----------

        bool g_helloSent = false;
        bool g_hostMismatch = false;                  // client: my plugin list differs from the host's
        std::unordered_set<uint32_t> g_badSenders;    // host: clients whose plugin list differs

        struct Fingerprint { uint32_t hash = 0; uint16_t full = 0; uint16_t light = 0; };

        const Fingerprint& MyFingerprint()
        {
            static Fingerprint fp;
            static bool done = false;
            if (done) return fp;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return fp;
            done = true;
            uint32_t h = 2166136261u;
            auto mix = [&](std::string_view name) {
                for (char c : name) { h ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c))); h *= 16777619u; }
                h ^= '|'; h *= 16777619u;
            };
            std::string list;
            for (auto* f : dh->compiledFileCollection.files) {
                if (!f) continue;
                mix(f->GetFilename()); fp.full++;
                list += std::format("\n  [{:02X}] {}", f->GetCompileIndex(), f->GetFilename());
            }
            h ^= '#'; h *= 16777619u;
            for (auto* f : dh->compiledFileCollection.smallFiles) {
                if (!f) continue;
                mix(f->GetFilename()); fp.light++;
                list += std::format("\n  [FE {:03X}] {}", f->GetSmallFileCompileIndex(), f->GetFilename());
            }
            fp.hash = h;
            REX::INFO("[Coop] load order {:08X} ({} plugins, {} light):{}", fp.hash, fp.full, fp.light, list);
            return fp;
        }

        void SendHello(bool reply)
        {
            const auto& fp = MyFingerprint();
            CoopHelloMsg msg{};
            msg.loadOrderHash = fp.hash;
            msg.fullCount = fp.full;
            msg.lightCount = fp.light;
            msg.isHost = IsHost() ? 1 : 0;
            msg.isReply = reply ? 1 : 0;
            msg.loadedRadius = Range::Loaded();
            Send(MessageType::CoopHello, msg);
        }

        void OnHello(const CoopHelloMsg& m)
        {
            const auto& fp = MyFingerprint();
            const bool same = m.loadOrderHash == fp.hash;
            Authority::SetRemoteRadius(m.senderId, m.loadedRadius);
            if (m.isHost) Authority::SetHost(m.senderId);
            if (IsHost()) {
                if (same) {
                    g_badSenders.erase(m.senderId);
                } else {
                    g_badSenders.insert(m.senderId);
                    REX::WARN("[Coop] player {} has a DIFFERENT load order ({:08X}, {}+{} plugins; host {:08X}, {}+{}): "
                              "ignoring their item/quest/combat sync", m.senderId, m.loadOrderHash, m.fullCount,
                              m.lightCount, fp.hash, fp.full, fp.light);
                }
                if (!m.isReply) SendHello(true);  // tell the newcomer what the host runs
            } else if (m.isHost) {
                g_hostMismatch = !same;
                if (same)
                    REX::INFO("[Coop] load order matches the host ({:08X})", fp.hash);
                else
                    REX::WARN("[Coop] load order DIFFERS from the host ({:08X}, {}+{} plugins; mine {:08X}, {}+{}): "
                              "item/quest/NPC sync paused", m.loadOrderHash, m.fullCount, m.lightCount, fp.hash, fp.full, fp.light);
            }
        }

        // FormID-based messages are unsafe across different plugin lists.
        bool FormIdSyncAllowed(uint32_t senderId)
        {
            if (g_hostMismatch) return false;
            return !(IsHost() && g_badSenders.contains(senderId));
        }

        // ---------- quests ----------

        std::unordered_map<uint32_t, uint16_t> g_questStages;  // questFormId -> last seen stage
        bool g_questBaseline = false;
        bool g_snapshotRequested = false;
        Clock::time_point g_lastQuestPoll{};

        bool IsTrackedQuest(RE::TESQuest* q)
        {
            if (!q) return false;
            const char* name = q->GetFullName();
            if (!name || !*name) return false;  // player-facing quests only; radiant/system quests stay local
            // Random encounters (REObjectCT05, RECampKMK01...) belong to the place they fired in: a guest's
            // game can't start them and must not spawn them twice.
            static const char* kEncounters[] = { "REObject", "RECamp", "REScene", "REAssault", "RETravel", "REChokepoint" };
            const char* edid = q->formEditorID.c_str();
            if (edid)
                for (const char* p : kEncounters)
                    if (_strnicmp(edid, p, std::strlen(p)) == 0) return false;
            return true;
        }

        void SendQuestStage(uint32_t fid, uint16_t stage, bool request, bool snapshot)
        {
            CoopQuestStageMsg msg{};
            msg.questFormId = fid;
            msg.stage = stage;
            msg.isRequest = request ? 1 : 0;
            msg.isSnapshot = snapshot ? 1 : 0;
            Send(MessageType::CoopQuestStage, msg);
        }

        void PollQuests()
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            const bool host = IsHost();
            uint32_t tracked = 0;
            for (auto* q : dh->GetFormArray<RE::TESQuest>()) {
                if (!IsTrackedQuest(q)) continue;
                tracked++;
                const uint32_t fid = q->GetFormID();
                const uint16_t stage = q->currentStage;
                auto it = g_questStages.find(fid);
                if (it == g_questStages.end()) {
                    g_questStages[fid] = stage;  // first sight: baseline only
                    continue;
                }
                if (it->second == stage) continue;
                it->second = stage;
                if (g_questBaseline && (host || g_settings.shareMyQuests)) {
                    // Host announces; a client asks the host to make it official (opt-in: in own-save
                    // co-op a joiner's quests are their own world's, e.g. a new game's intro stages).
                    SendQuestStage(fid, stage, !host, false);
                    REX::INFO("[Coop] quest {:#x} -> stage {} ({})", fid, stage, host ? "broadcast" : "request");
                }
            }
            g_stats.trackedQuests = tracked;

            // Join-time catch-up: while playing as a guest, the host's story is the story (it
            // rewrites quest progress in the joiner's own save). Asked once they've arrived.
            static Clock::time_point s_connectedAt{};
            if (s_connectedAt == Clock::time_point{} || !g_questBaseline) s_connectedAt = Clock::now();
            if (!host && !g_snapshotRequested && g_settings.questCatchUp && Clock::now() - s_connectedAt > std::chrono::seconds(10)) {
                g_snapshotRequested = true;
                Send(MessageType::CoopQuestSnapshotReq, CoopQuestSnapshotReqMsg{});
                REX::INFO("[Coop] asked the host for its quest progress");
            }
            g_questBaseline = true;
        }

        // Applied stages are checked a moment later (the console runs the command on its own time).
        struct QuestCheck { uint32_t quest; uint16_t stage; Clock::time_point at; };
        std::vector<QuestCheck> g_pendingQuestCheck;

        void CheckAppliedQuests()
        {
            const auto now = Clock::now();
            for (auto it = g_pendingQuestCheck.begin(); it != g_pendingQuestCheck.end();) {
                if (now - it->at < std::chrono::seconds(2)) { ++it; continue; }
                if (auto* q = LookupForm<RE::TESQuest>(it->quest)) {
                    g_questStages[it->quest] = q->currentStage;   // what the console left: no echo
                    REX::INFO("[Coop] quest {:#x}: now at stage {} (wanted {}){}", it->quest, q->currentStage, it->stage,
                        q->currentStage == it->stage ? "" : " - NOT applied");
                }
                it = g_pendingQuestCheck.erase(it);
            }
        }

        void ApplyQuestStage(const CoopQuestStageMsg& m);
        // The join catch-up brings the host's stage for every quest at once (hundreds): applied a few per
        // tick so the console and quest scripts aren't flooded in one frame.
        std::deque<CoopQuestStageMsg> g_questBacklog;

        void OnQuestStage(const CoopQuestStageMsg& m)
        {
            if (m.isSnapshot) { if (g_questBacklog.size() < 4096) g_questBacklog.push_back(m); return; }
            ApplyQuestStage(m);
        }

        void DrainQuestBacklog()
        {
            for (int i = 0; i < 4 && !g_questBacklog.empty(); ++i) {
                const auto m = g_questBacklog.front();
                g_questBacklog.pop_front();
                ApplyQuestStage(m);
            }
        }

        void ApplyQuestStage(const CoopQuestStageMsg& m)
        {
            auto* q = LookupForm<RE::TESQuest>(m.questFormId);
            if (!q) return;
            if (!IsTrackedQuest(q)) {   // e.g. a random encounter from an older host
                static int s_skipped = 0;
                if (s_skipped++ < 20) REX::INFO("[Coop] quest {:#x} ({}) not synced: local only", m.questFormId, q->formEditorID.c_str());
                return;
            }
            const bool host = IsHost();
            if (m.isRequest && !host) return;   // only the host acts on requests
            if (!m.isRequest && host) return;   // the host is the source of truth
            if (q->currentStage == m.stage) return;
            // Join snapshot: only move forward. Rewinding a stage re-runs its fragment.
            if (m.isSnapshot && m.stage <= q->currentStage) return;

            const uint16_t before = q->currentStage;
            bool ok = q->SetStage(m.stage);
            const char* how = "SetStage";
            if (!ok) {
                // The native call refused every stage on guests, e.g. for
                // a quest their own story never started. The console's setstage starts it if needed.
                char cmd[48];
                std::snprintf(cmd, sizeof(cmd), "setstage %X %u", m.questFormId, static_cast<unsigned>(m.stage));
                RE::Console::ExecuteCommand(cmd);
                how = "console setstage";
            }
            g_questStages[m.questFormId] = q->currentStage;  // don't echo what we just applied
            g_pendingQuestCheck.push_back({ m.questFormId, m.stage, Clock::now() });
            g_stats.questStagesApplied++;
            g_stats.applied++;
            REX::INFO("[Coop] quest {:#x} ({}) stage {} -> {} via {} (native {}, from player {}{})", m.questFormId, q->formEditorID.c_str(), before, m.stage, how,
                ok, m.senderId, m.isRequest ? ", request" : "");
        }

        void OnQuestSnapshotRequest()
        {
            if (!IsHost()) return;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            uint32_t n = 0;
            for (auto* q : dh->GetFormArray<RE::TESQuest>()) {
                if (!IsTrackedQuest(q) || q->currentStage == 0) continue;
                SendQuestStage(q->GetFormID(), q->currentStage, false, true);
                n++;
            }
            REX::INFO("[Coop] host: sent quest snapshot ({} quests)", n);
        }

        // ---------- combat animations (player -> all) ----------

        // Graph events worth replaying on a body: attacks, blocks, aiming, reloads, throws.
        // Movement events are left out (bodies get speed/direction instead).
        bool IsCombatEvent(const std::string& a_ev)
        {
            static const char* kPrefixes[] = { "attack", "meleeattack", "powerattack", "block", "bash", "rifleSighted",
                                               "reload", "throw", "grenade", "Weap" };
            for (const char* p : kPrefixes)
                if (_strnicmp(a_ev.c_str(), p, std::strlen(p)) == 0) return true;
            return false;
        }

        void SendCombatEvents()
        {
            for (auto& ev : PlayerEvents::Drain()) {
                if (!g_settings.combatAnims || !IsCombatEvent(ev)) continue;
                CoopAnimEventMsg msg{};
                strncpy_s(msg.name, ev.c_str(), sizeof(msg.name) - 1);
                Send(MessageType::CoopAnimEvent, msg);
            }
        }

        // Aim/shot/reload starts the body's graph refused (busy drawing the weapon, finishing a
        // landing...): the weapon is put out and the event retried for a moment, or the other
        // player sees no shot and no bullet impact.
        struct AnimRetry { uint32_t pid; std::string name; Clock::time_point since; std::chrono::milliseconds window; };
        std::vector<AnimRetry> g_animRetry;
        constexpr auto kAnimRetryFor = std::chrono::milliseconds(600);

        bool WorthRetry(const char* a_ev)
        {
            return _strnicmp(a_ev, "attackStart", 11) == 0 || _stricmp(a_ev, "rifleSightedStart") == 0 ||
                   _strnicmp(a_ev, "reloadStart", 11) == 0;
        }

        RE::Actor* BodyOfPlayer(uint32_t a_pid)
        {
            for (auto& [pid, bodyId] : GetBodies())
                if (pid == a_pid) return LookupForm<RE::Actor>(bodyId);
            return nullptr;
        }

        void OnAnimEvent(const CoopAnimEventMsg& m)
        {
            char name[sizeof(m.name) + 1]{};
            std::memcpy(name, m.name, sizeof(m.name));
            auto* body = BodyOfPlayer(m.senderId);
            if (!body) return;
            // A newer aim/shot replaces one still waiting (the player moved on).
            if (WorthRetry(name)) std::erase_if(g_animRetry, [&](auto& r) { return r.pid == m.senderId; });
            const bool ok = GraphOverride::SendEvent(body, name);
            static int s_logged = 0, s_refused = 0;
            if (s_logged++ < 600) REX::INFO("[Coop] body of player {}: {} -> {}", m.senderId, name, ok);
            if (!ok && WorthRetry(name)) {
                // Draw-and-shoot in one press: the shot comes while the body is still drawing, which
                // takes longer than 0.6 s - that first shot was lost.
                const bool drawing = !body->GetWeaponMagicDrawn();
                if (drawing) body->DrawWeaponMagicHands(true);
                g_animRetry.push_back({ m.senderId, name, Clock::now(), drawing ? std::chrono::milliseconds(1500) : kAnimRetryFor });
                if (s_refused++ < 300) REX::INFO("[Coop] body of player {} refused {} (weapon out {}): retrying", m.senderId, name,
                    body->GetWeaponMagicDrawn());
            }
        }

        void RetryAnimEvents()
        {
            const auto now = Clock::now();
            for (auto it = g_animRetry.begin(); it != g_animRetry.end();) {
                auto* body = BodyOfPlayer(it->pid);
                const bool ok = body && GraphOverride::SendEvent(body, it->name.c_str());
                const bool expired = now - it->since > it->window;
                if (ok || expired || !body) {
                    static int s_logged = 0;
                    if (s_logged++ < 300) REX::INFO("[Coop] body of player {}: {} {}", it->pid, it->name, ok ? "accepted on retry" : "dropped");
                    it = g_animRetry.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // ---------- NPC combat animations (owner -> all) ----------
        // NPCs this game runs: their attacks/reloads go out so the copies on other screens play
        // them (they used to glide at the player without biting or shooting). Copies hit for 0.
        void SendNpcCombatEvents()
        {
            auto events = GraphOverride::DrainNpcEvents();
            if (events.empty() || !g_settings.combatAnims) return;
            static int s_logged = 0;
            static std::unordered_map<std::string, Clock::time_point> s_lastIdle;   // "fid/event" -> sent
            const auto now = Clock::now();
            for (auto& [fid, ev] : events) {
                auto* actor = LookupForm<RE::Actor>(fid);
                if (!actor || GetPlayerIdForBody(fid) || actor->IsDisabled() || !Authority::IOwn(actor)) continue;
                if (fid >= DynamicActors::kRuntimeIdStart && DynamicActors::IsMirrorCopy(fid)) continue;
                // Idles: the AI can repeat the same one every frame; once per second per NPC is plenty.
                if (GraphOverride::IsNpcIdleEvent(ev.c_str())) {
                    auto& last = s_lastIdle[std::to_string(fid) + "/" + ev];
                    if (now - last < std::chrono::seconds(1)) continue;
                    last = now;
                    if (s_lastIdle.size() > 2048) s_lastIdle.clear();
                }
                CoopNpcAnimMsg msg{};
                msg.npcFormId = fid;
                strncpy_s(msg.name, ev.c_str(), sizeof(msg.name) - 1);
                Send(MessageType::CoopNpcAnim, msg);
                static int s_idles = 0;
                if (GraphOverride::IsNpcIdleEvent(ev.c_str()) ? s_idles++ < 60 : s_logged++ < 30)
                    REX::INFO("[Coop] our NPC {:#x}: {} sent", fid, ev);
            }
        }

        void OnNpcAnim(const CoopNpcAnimMsg& m)
        {
            char name[sizeof(m.name) + 1]{};
            std::memcpy(name, m.name, sizeof(m.name));
            RE::Actor* npc = m.npcFormId >= DynamicActors::kRuntimeIdStart ? DynamicActors::Resolve(m.senderId, m.npcFormId)
                                                                           : LookupForm<RE::Actor>(m.npcFormId);
            if (!npc || !npc->Get3D() || npc->IsDead(false) || Authority::IOwn(npc)) return;
            const bool ok = GraphOverride::SendEvent(npc, name);
            static int s_logged = 0, s_idles = 0;
            if (GraphOverride::IsNpcIdleEvent(name) ? s_idles++ < 60 : s_logged++ < 30)
                REX::INFO("[Coop] player {}'s NPC {:#x}: {} -> {}", m.senderId, npc->GetFormID(), name, ok);
        }

        // ---------- locks (player -> all) ----------
        // A door or container someone picked open stayed locked for the other player (and the replayed
        // door opening did nothing). Locks the player touches are watched for a while; a change goes out.
        struct LockWatch { bool locked; Clock::time_point until; };
        std::unordered_map<uint32_t, LockWatch> g_lockWatch;

        bool IsLockedNow(RE::TESObjectREFR* ref)
        {
            auto* lock = ref ? ref->GetLock() : nullptr;
            return lock && (lock->flags & 1) != 0;   // REFR_LOCK::Flags::kLocked
        }

        void WatchLock(RE::TESObjectREFR* ref)
        {
            if (!ref || !ref->GetLock() || !IsSyncableRef(ref->GetFormID())) return;
            g_lockWatch[ref->GetFormID()] = { IsLockedNow(ref), Clock::now() + std::chrono::minutes(2) };
        }

        void PollLocks()
        {
            const auto now = Clock::now();
            for (auto it = g_lockWatch.begin(); it != g_lockWatch.end();) {
                auto* ref = LookupForm<RE::TESObjectREFR>(it->first);
                if (!ref || now > it->second.until) { it = g_lockWatch.erase(it); continue; }
                const bool locked = IsLockedNow(ref);
                if (locked != it->second.locked) {
                    it->second.locked = locked;
                    CoopLockChangedMsg msg{};
                    msg.refFormId = it->first;
                    msg.locked = locked ? 1 : 0;
                    Send(MessageType::CoopLockChanged, msg);
                    REX::INFO("[Coop] {:#x} {}: sent", it->first, locked ? "locked" : "unlocked");
                }
                ++it;
            }
        }

        void OnLockChanged(const CoopLockChangedMsg& m)
        {
            auto* ref = LookupForm<RE::TESObjectREFR>(m.refFormId);
            auto* lock = ref ? ref->GetLock() : nullptr;
            if (!lock) return;
            const bool want = m.locked != 0;
            if (IsLockedNow(ref) != want) lock->SetLocked(want);
            if (auto it = g_lockWatch.find(m.refFormId); it != g_lockWatch.end()) it->second.locked = want;   // no echo
            g_stats.applied++;
            REX::INFO("[Coop] player {} {} {:#x} (locked here now: {})", m.senderId, want ? "locked" : "unlocked", m.refFormId, IsLockedNow(ref));
        }

        // ---------- world time & weather (host -> all) ----------

        void SendWorldTime()
        {
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal || !cal->gameHour || !cal->gameDay || !cal->gameMonth || !cal->gameYear || !cal->timeScale) return;
            CoopWorldTimeMsg msg{};
            msg.gameHour = cal->gameHour->GetValue();
            msg.gameDay = cal->gameDay->GetValue();
            msg.gameMonth = cal->gameMonth->GetValue();
            msg.gameYear = cal->gameYear->GetValue();
            msg.timeScale = cal->timeScale->GetValue();
            auto* sky = RE::Sky::GetSingleton();
            msg.weatherFormId = sky && sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
            Send(MessageType::CoopWorldTime, msg);
        }

        void OnWorldTime(const CoopWorldTimeMsg& m)
        {
            if (IsHost() || m.senderId == Authority::MyId()) return;
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal || !cal->gameHour || !cal->gameDay || !cal->gameMonth || !cal->gameYear || !cal->timeScale) return;

            // Only step the clock when it has drifted (> 3 game minutes): both games run the same
            // time scale, so small differences are just message latency.
            float diff = std::fabs(cal->gameHour->value - m.gameHour);
            diff = std::min(diff, 24.0f - diff);   // 23:59 vs 00:01 is 2 minutes apart
            const bool newDate = cal->gameDay->value != m.gameDay || cal->gameMonth->value != m.gameMonth ||
                                 cal->gameYear->value != m.gameYear;
            if (diff > 0.05f || newDate) {
                static bool s_logged = false;
                if (!s_logged || diff > 0.5f) {
                    s_logged = true;
                    REX::INFO("[Coop] clock {:.2f}h -> host {:.2f}h (day {}/{}/{})", cal->gameHour->value, m.gameHour,
                        m.gameMonth + 1, m.gameDay, m.gameYear);
                }
                cal->gameHour->value = m.gameHour;
                cal->gameDay->value = m.gameDay;
                cal->gameMonth->value = m.gameMonth;
                cal->gameYear->value = m.gameYear;
            }
            cal->timeScale->value = m.timeScale;

            // Weather: only outdoors (interiors have their own lighting); fade to the host's.
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->parentCell : nullptr;
            auto* sky = RE::Sky::GetSingleton();
            if (!m.weatherFormId || !sky || !cell || cell->IsInterior()) return;
            if (sky->currentWeather && sky->currentWeather->GetFormID() == m.weatherFormId) return;
            if (auto* weather = LookupForm<RE::TESWeather>(m.weatherFormId)) {
                sky->ForceWeather(weather, false);
                REX::INFO("[Coop] weather -> host's {:#x}", m.weatherFormId);
            }
        }

        // ---------- event sinks ----------

        // ---------- dropped items (trading by dropping) ----------
        // A drop is named by (owner player id, the dropped ref's FormID in the owner's game).
        // Each other game puts its own copy on the ground; whoever picks one up gets the item and
        // every other copy, the owner's original included, is removed. No duplicates.
        struct DropKey {
            uint32_t owner, ref;
            bool operator==(const DropKey&) const = default;
        };
        struct DropKeyHash {
            size_t operator()(const DropKey& k) const { return (size_t(k.owner) << 32) ^ k.ref; }
        };
        std::unordered_map<DropKey, uint32_t, DropKeyHash> g_dropCopies;   // remote drop -> our copy
        std::unordered_map<uint32_t, DropKey> g_dropByLocalRef;            // our copy -> remote drop
        std::unordered_set<uint32_t> g_myDrops;                            // refs WE dropped (originals)
        bool g_placingDrop = false;                                        // our own placement, not a drop

        void OnItemDropped(const CoopItemDroppedMsg& m)
        {
            if (m.space && Authority::LocalSpace() && m.space != Authority::LocalSpace()) return;  // elsewhere
            auto* obj = LookupForm<RE::TESBoundObject>(m.itemFormId);
            const DropKey key{ m.senderId, m.refFormId };
            // The dropper's body carries the items for a moment (the local player would show
            // "Added/Removed" messages); RemoveItem with a drop location makes a normal world item.
            RE::TESObjectREFR* carrier = nullptr;
            for (auto& [pid, bodyId] : GetBodies())
                if (pid == m.senderId) carrier = LookupForm<RE::TESObjectREFR>(bodyId);
            if (!carrier) carrier = RE::PlayerCharacter::GetSingleton();
            if (!obj || !carrier || m.count <= 0 || g_dropCopies.count(key)) return;
            const RE::NiPoint3 at{ m.x, m.y, m.z };
            g_placingDrop = true;
            AddItems(carrier, obj, m.count);
            RE::TESObjectREFR::RemoveItemData data(obj, m.count);
            data.reason = RE::ITEM_REMOVE_REASON::KDropping;
            data.dropLoc = &at;
            auto handle = carrier->RemoveItem(data);
            g_placingDrop = false;
            auto ref = handle.get();
            if (!ref) {
                REX::WARN("[Coop] drop {:08X} x{} from player {}: could not place a copy", m.itemFormId, m.count, m.senderId);
                return;
            }
            g_myDrops.erase(ref->GetFormID());   // FormID reused: our old drop there is gone
            if (auto it = g_dropByLocalRef.find(ref->GetFormID()); it != g_dropByLocalRef.end()) {
                g_dropCopies.erase(it->second);
                g_dropByLocalRef.erase(it);
            }
            ApplyMods(ref.get(), m.mods, m.numMods);
            g_dropCopies[key] = ref->GetFormID();
            g_dropByLocalRef[ref->GetFormID()] = key;
            g_stats.applied++;
            REX::INFO("[Coop] player {} dropped {:08X} x{}: copy {:#x} placed", m.senderId, m.itemFormId, m.count, ref->GetFormID());
        }

        void RemoveDropRef(uint32_t fid)
        {
            auto* ref = LookupForm<RE::TESObjectREFR>(fid);
            if (!ref || ref->IsDisabled()) return;
            ref->Disable();
            ref->SetDelete(true);
        }

        void OnDropTaken(const CoopDropTakenMsg& m)
        {
            const DropKey key{ m.ownerId, m.ownerRefFormId };
            if (m.ownerId == NetworkClient::GetInstance().GetPlayerId()) {
                if (g_myDrops.erase(m.ownerRefFormId)) RemoveDropRef(m.ownerRefFormId);
                REX::INFO("[Coop] player {} picked up our drop {:#x}", m.senderId, m.ownerRefFormId);
            } else if (auto it = g_dropCopies.find(key); it != g_dropCopies.end()) {
                RemoveDropRef(it->second);
                g_dropByLocalRef.erase(it->second);
                g_dropCopies.erase(it);
                REX::INFO("[Coop] player {} picked up player {}'s drop: our copy removed", m.senderId, m.ownerId);
            }
            g_stats.applied++;
        }

        class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent& e,
                RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
            {
                auto& net = NetworkClient::GetInstance();
                if (!net.IsConnected() || !g_settings.items || g_hostMismatch) return RE::BSEventNotifyControl::kContinue;

                if (g_placingDrop) return RE::BSEventNotifyControl::kContinue;
                if (e.newContainerFormID == kPlayerRefId && e.oldContainerFormID == 0)
                    REX::INFO("[Coop] picked up {:08X} x{} (ref {:#x})", e.baseObjectFormID, e.itemCount, e.referenceFormID);
                if (e.oldContainerFormID == kPlayerRefId && e.newContainerFormID == 0 && e.referenceFormID) {
                    // We dropped something: tell the others where it lies.
                    auto* ref = LookupForm<RE::TESObjectREFR>(e.referenceFormID);
                    auto* pl = RE::PlayerCharacter::GetSingleton();
                    const RE::NiPoint3 at = ref ? ref->GetPosition() : (pl ? pl->GetPosition() : RE::NiPoint3{});
                    CoopItemDroppedMsg msg{};
                    msg.refFormId = e.referenceFormID;
                    msg.itemFormId = e.baseObjectFormID;
                    msg.count = e.itemCount;
                    msg.x = at.x; msg.y = at.y; msg.z = at.z;
                    msg.space = Authority::LocalSpace();
                    msg.numMods = ref ? ReadMods(ref->extraList.get(), msg.mods) : 0;
                    Send(MessageType::CoopItemDropped, msg);
                    if (auto it = g_dropByLocalRef.find(e.referenceFormID); it != g_dropByLocalRef.end()) {
                        g_dropCopies.erase(it->second);   // FormID reused: that copy is long gone
                        g_dropByLocalRef.erase(it);
                    }
                    g_myDrops.insert(e.referenceFormID);
                    REX::INFO("[Coop] dropped {:08X} x{} as {:#x} ({} mods): sent", e.baseObjectFormID, e.itemCount, e.referenceFormID, msg.numMods);
                } else if (e.newContainerFormID == kPlayerRefId && e.oldContainerFormID == 0 &&
                    IsSyncableRef(e.referenceFormID)) {
                    CoopWorldRefTakenMsg msg{};
                    msg.refFormId = e.referenceFormID;
                    Send(MessageType::CoopWorldRefTaken, msg);
                } else if (uint32_t owner = 0, ref = 0; e.newContainerFormID == kPlayerRefId && e.oldContainerFormID &&
                                                         NameRef(e.oldContainerFormID, owner, ref)) {
                    CoopContainerChangeMsg msg{};
                    msg.containerFormId = ref;
                    msg.ownerId = owner;
                    msg.itemFormId = e.baseObjectFormID;
                    msg.count = -e.itemCount;
                    Send(MessageType::CoopContainerChange, msg);
                } else if (e.oldContainerFormID == kPlayerRefId && e.newContainerFormID && NameRef(e.newContainerFormID, owner, ref)) {
                    CoopContainerChangeMsg msg{};
                    msg.containerFormId = ref;
                    msg.ownerId = owner;
                    msg.itemFormId = e.baseObjectFormID;
                    msg.count = e.itemCount;
                    Send(MessageType::CoopContainerChange, msg);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // A client opening a container (or a corpse) asks the host for its real contents:
        // leveled loot is rolled per machine when a cell first loads, so contents can differ.
        class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& e,
                RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                auto& net = NetworkClient::GetInstance();
                if (!net.IsConnected()) return RE::BSEventNotifyControl::kContinue;
                auto* target = e.objectActivated.get();
                auto* by = e.actionRef.get();
                if (!target || !by || by->GetFormID() != kPlayerRefId) return RE::BSEventNotifyControl::kContinue;
                if (g_settings.building && !g_hostMismatch) Workshop::OnLocalActivate(target);
                if (g_settings.items && !g_hostMismatch) WatchLock(target);
                if (!g_settings.items) return RE::BSEventNotifyControl::kContinue;
                const uint32_t fid = target->GetFormID();
                // Picking up a dropped item: the container event that follows has no reference
                // (the item merges into the inventory), so the activation is where we know which.
                if (g_myDrops.count(fid) || g_dropByLocalRef.count(fid)) {
                    CoopDropTakenMsg msg{};
                    if (auto it = g_dropByLocalRef.find(fid); it != g_dropByLocalRef.end()) {
                        msg.ownerId = it->second.owner;
                        msg.ownerRefFormId = it->second.ref;
                        g_dropCopies.erase(it->second);
                        g_dropByLocalRef.erase(it);
                    } else {
                        msg.ownerId = net.GetPlayerId();
                        msg.ownerRefFormId = fid;
                        g_myDrops.erase(fid);
                    }
                    Send(MessageType::CoopDropTaken, msg);
                    REX::INFO("[Coop] picked up player {}'s drop {:#x}: sent", msg.ownerId, msg.ownerRefFormId);
                    return RE::BSEventNotifyControl::kContinue;
                }
                uint32_t owner = 0, ref = 0;
                if (!NameRef(fid, owner, ref)) return RE::BSEventNotifyControl::kContinue;

                auto* base = target->GetObjectReference();
                const bool isContainer = base && base->GetFormType() == RE::ENUM_FORM_ID::kCONT;
                auto* actor = target->As<RE::Actor>();
                const bool isCorpse = actor && actor->IsDead(false);
                if (!isContainer && !isCorpse) return RE::BSEventNotifyControl::kContinue;

                if (g_pendingSnapshot.insert(fid).second) {
                    CoopContainerSnapshotReqMsg msg{};
                    msg.containerFormId = ref;
                    msg.ownerId = owner;
                    Send(MessageType::CoopContainerSnapshotReq, msg);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ---------- player vitals (name tags) ----------
        struct Vitals { float health = 0, maxHealth = 0; bool dead = false; };
        std::mutex g_vitalsMutex;   // read by the HUD on the render thread
        std::unordered_map<uint32_t, Vitals> g_vitals;

        void SendOwnVitals()
        {
            static Clock::time_point s_last{};
            static float s_lastHealth = -1.0f;
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* hp = HealthAV();
            if (!pl || !hp) return;
            const float cur = pl->GetActorValue(*hp), max = pl->GetPermanentActorValue(*hp);
            const auto now = Clock::now();
            // Twice a second, and at once when it changes by a point or more.
            if (now - s_last < std::chrono::milliseconds(500) && std::fabs(cur - s_lastHealth) < 1.0f) return;
            s_last = now;
            s_lastHealth = cur;
            CoopPlayerVitalsMsg msg{};
            msg.health = cur;
            msg.maxHealth = max;
            msg.dead = pl->IsDead(false) ? 1 : 0;
            Send(MessageType::CoopPlayerVitals, msg);
        }

        // A real attack on a remote player's body (an actor's hit, not a physics bump): forward the
        // damage the engine computed for it to that player, once per hit.
        

        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& e, RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                if (!g_settings.combat || !NetworkClient::GetInstance().IsConnected()) return RE::BSEventNotifyControl::kContinue;
                // On this runtime bullet hits arrive with empty target/cause pointers; who hit whom
                // is in the hit data's handles (aggressor / target) instead.
                RE::NiPointer<RE::TESObjectREFR> targetPtr = e.target;
                RE::NiPointer<RE::TESObjectREFR> causePtr = e.cause;
                RE::NiPointer<RE::Actor> hdTarget, hdAggressor;
                if (e.usesHitData) {
                    if (!targetPtr) { hdTarget = e.hitData.target.get(); targetPtr.reset(hdTarget.get()); }
                    if (!causePtr) { hdAggressor = e.hitData.aggressor.get(); causePtr.reset(hdAggressor.get()); }
                }
                auto* target = targetPtr.get();
                auto* cause = causePtr.get();
                if (g_settings.hitTrace && (cause == RE::PlayerCharacter::GetSingleton() ||
                        (target && GetPlayerIdForBody(target->GetFormID())))) {
                    REX::INFO("[HitTrace] target {:#x} cause {:#x} usesHitData {} health {:.1f} flags {:#x}",
                        target ? target->GetFormID() : 0u, cause ? cause->GetFormID() : 0u, e.usesHitData,
                        e.usesHitData ? e.hitData.healthDamage : -1.0f, e.usesHitData ? static_cast<uint32_t>(e.hitData.flags.underlying()) : 0u);
                }
                // A body's replayed shot hit our player. The real hit (if any) is the shooter's own and
                // reaches us as CoopPlayerDamage; this copy is for show: give the health back.
                if (auto* pl = RE::PlayerCharacter::GetSingleton(); target == pl && cause && GetPlayerIdForBody(cause->GetFormID())) {
                    const float back = e.usesHitData ? e.hitData.healthDamage : 0.0f;
                    if (back > 0.0f) if (auto* hp = HealthAV()) pl->RestoreActorValue(*hp, back);
                    static int s_logged = 0;
                    if (s_logged++ < 30) REX::INFO("[Coop] body {:#x}'s replayed shot hit us for {:.1f}: given back", cause->GetFormID(), back);
                    return RE::BSEventNotifyControl::kContinue;
                }
                // Explosions are set off again in every nearby game (Explosions.cpp), and each game
                // hurts its own player with them: a blast on a body here must not count again.
                const bool blast = !e.usesHitData || e.hitData.flags.all(RE::HitData::Flag::kExplosion);
                if (target && blast && GetPlayerIdForBody(target->GetFormID())) return RE::BSEventNotifyControl::kContinue;
                if (!target || !cause || cause == target || !cause->As<RE::Actor>()) return RE::BSEventNotifyControl::kContinue;
                const float dmg = e.usesHitData ? e.hitData.healthDamage : 0.0f;
                const uint32_t pid = GetPlayerIdForBody(target->GetFormID());
                if (!pid) {
                    // Our player hit an NPC someone else's game runs: that game applies the hit.
                    // (Measuring the copy's health drops sent phantom hits whenever the copy
                    // couldn't reach the owner's health, e.g. a different max.)
                    // Not mirrored any more (its area became ours, or the owner stopped sending it): the hit
                    // already counted here, forwarding it to the old owner would land on a copy.
                    auto it = g_npcOwners.find(target->GetFormID());
                    if (it == g_npcOwners.end() || Clock::now() - it->second.at > std::chrono::seconds(1) ||
                        cause->GetFormID() != kPlayerRefId || dmg <= 0.0f)
                        return RE::BSEventNotifyControl::kContinue;
                    // A blast is set off again in the owner's game (Explosions.cpp) and hurts the real NPC
                    // there: forwarding our copy's blast damage too counted every grenade twice.
                    if (blast) return RE::BSEventNotifyControl::kContinue;
                    CoopNpcDamageMsg msg{};
                    msg.ownerId = it->second.ownerId;
                    msg.npcFormId = it->second.formId;
                    msg.damage = dmg;
                    Send(MessageType::CoopNpcDamage, msg);
                    g_stats.damageForwarded++;
                    REX::INFO("[Coop] we hit player {}'s NPC {:#x} for {:.1f}: forwarded", msg.ownerId, msg.npcFormId, dmg);
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (dmg <= 0.0f) return RE::BSEventNotifyControl::kContinue;
                CoopPlayerDamageMsg msg{};
                msg.targetPlayerId = pid;
                msg.damage = dmg;
                Send(MessageType::CoopPlayerDamage, msg);
                g_stats.damageForwarded++;
                REX::INFO("[Coop] body of player {} hit by {:#x} for {:.1f}: forwarded", pid, cause->GetFormID(), dmg);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        ContainerSink g_containerSink;
        ActivateSink g_activateSink;
        HitSink g_hitSink;

        void Dispatch(const CoopInboxItem& item)
        {
            if (item.type == MessageType::CoopHello) {
                if (auto* m = As<CoopHelloMsg>(item)) OnHello(*m);
                return;
            }
            const uint32_t sender = item.payload.size() >= 4 ? *reinterpret_cast<const uint32_t*>(item.payload.data()) : 0;
            if (item.type != MessageType::CoopPlayerDamage && item.type != MessageType::CoopPlayerVitals &&
                !FormIdSyncAllowed(sender)) return;

            switch (item.type) {
            case MessageType::CoopDynamicActor:
                if (auto* m = As<CoopDynamicActorMsg>(item); m && m->senderId != Authority::MyId())
                    DynamicActors::OnAnnounce(m->senderId, m->hostRefId, m->spawnFormId, m->x, m->y, m->z, m->angleZ);
                break;
            case MessageType::CoopAppearance:
                Appearance::OnReceived(sender, item.payload.data(), item.payload.size());
                break;
            case MessageType::CoopEquipment:
                if (auto* m = As<CoopEquipmentMsg>(item); m && g_settings.equipment) {
                    const uint8_t n = std::min<uint8_t>(m->count, static_cast<uint8_t>(std::size(m->forms)));
                    g_wantEquip[m->senderId].assign(m->forms, m->forms + n);
                    auto& wm = g_wantMods[m->senderId];
                    wm.weapon = m->weaponFormId;
                    wm.count = std::min<uint8_t>(m->numWeaponMods, static_cast<uint8_t>(kMaxItemMods));
                    std::copy_n(m->weaponMods, wm.count, wm.mods);
                }
                break;
            case MessageType::CoopPlayerDamage:
                if (auto* m = As<CoopPlayerDamageMsg>(item); m && g_settings.combat) OnPlayerDamage(*m);
                break;
            case MessageType::CoopWorldReq:
                if (auto* m = As<CoopWorldReqMsg>(item); m && g_settings.building) Workshop::OnWorldReq(*m);
                break;
            case MessageType::CoopWorldSnapshot:
                if (auto* m = As<CoopWorldSnapshotMsg>(item); m && g_settings.building) Workshop::OnWorldSnapshot(*m);
                break;
            case MessageType::CoopNpcAnim:
                if (auto* m = As<CoopNpcAnimMsg>(item); m && g_settings.combatAnims) OnNpcAnim(*m);
                break;
            case MessageType::CoopFurniture:
                if (auto* m = As<CoopRefActivatedMsg>(item)) Workshop::OnFurniture(*m);
                break;
            case MessageType::CoopRefsHidden:
                if (auto* m = As<CoopRefsHiddenMsg>(item); m && g_settings.building) Workshop::OnRefsHidden(*m);
                break;
            case MessageType::CoopExplosion:
                if (auto* m = As<CoopExplosionMsg>(item); m && g_settings.combat) Explosions::OnReceived(*m);
                break;
            case MessageType::CoopNpcDamage:
                if (auto* m = As<CoopNpcDamageMsg>(item); m && g_settings.combat) OnNpcDamage(*m);
                break;
            case MessageType::CoopContainerChange:
                if (auto* m = As<CoopContainerChangeMsg>(item); m && g_settings.items) OnContainerChange(*m);
                break;
            case MessageType::CoopItemDropped:
                if (auto* m = As<CoopItemDroppedMsg>(item); m && g_settings.items) OnItemDropped(*m);
                break;
            case MessageType::CoopDropTaken:
                if (auto* m = As<CoopDropTakenMsg>(item); m && g_settings.items) OnDropTaken(*m);
                break;
            case MessageType::CoopBuildPlaced:
                if (auto* m = As<CoopBuildPlacedMsg>(item); m && g_settings.building) Workshop::OnPlaced(*m);
                break;
            case MessageType::CoopBuildMoved:
                if (auto* m = As<CoopBuildMovedMsg>(item); m && g_settings.building) Workshop::OnMoved(*m);
                break;
            case MessageType::CoopBuildRemoved:
                if (auto* m = As<CoopBuildRemovedMsg>(item); m && g_settings.building) Workshop::OnRemoved(*m);
                break;
            case MessageType::CoopRefActivated:
                if (auto* m = As<CoopRefActivatedMsg>(item); m && g_settings.building) Workshop::OnActivated(*m);
                break;
            case MessageType::CoopPlayerVitals:
                if (auto* m = As<CoopPlayerVitalsMsg>(item)) {
                    std::lock_guard lk(g_vitalsMutex);
                    g_vitals[m->senderId] = { m->health, m->maxHealth, m->dead != 0 };
                }
                break;
            case MessageType::CoopPropMoved:
                if (auto* m = As<CoopPropMovedMsg>(item); m && g_settings.building) Workshop::OnPropMoved(*m);
                break;
            case MessageType::CoopWorldRefTaken:
                if (auto* m = As<CoopWorldRefTakenMsg>(item); m && g_settings.items) OnWorldRefTaken(*m);
                break;
            case MessageType::CoopContainerSnapshotReq:
                if (auto* m = As<CoopContainerSnapshotReqMsg>(item); m && g_settings.items) OnSnapshotRequest(*m);
                break;
            case MessageType::CoopContainerSnapshot:
                if (auto* m = As<CoopContainerSnapshotMsg>(item); m && g_settings.items) OnSnapshot(*m);
                break;
            case MessageType::CoopQuestStage:
                if (auto* m = As<CoopQuestStageMsg>(item); m && g_settings.quests) OnQuestStage(*m);
                break;
            case MessageType::CoopQuestSnapshotReq:
                if (g_settings.quests) OnQuestSnapshotRequest();
                break;
            case MessageType::CoopAnimEvent:
                if (auto* m = As<CoopAnimEventMsg>(item); m && g_settings.combatAnims) OnAnimEvent(*m);
                break;
            case MessageType::CoopLockChanged:
                if (auto* m = As<CoopLockChangedMsg>(item); m && g_settings.items) OnLockChanged(*m);
                break;
            case MessageType::CoopWorldTime:
                if (auto* m = As<CoopWorldTimeMsg>(item); m && g_settings.worldTime) OnWorldTime(*m);
                break;
            default:
                break;
            }
        }
    }

    Settings& GetSettings() { return g_settings; }
    bool IsLoadOrderMismatch() { return g_hostMismatch; }
    size_t MismatchedPlayers() { return g_badSenders.size(); }
    const Stats& GetStats() { return g_stats; }

    void RegisterSinks()
    {
        if (auto* src = RE::TESContainerChangedEvent::GetEventSource()) src->RegisterSink(&g_containerSink);
        if (auto* src = RE::TESActivateEvent::GetEventSource()) src->RegisterSink(&g_activateSink);
        auto* hitSrc = RE::TESHitEvent::GetEventSource();
        if (hitSrc) hitSrc->RegisterSink(&g_hitSink);
        REX::INFO("[Coop] event sinks registered (container, activate, hit {})", static_cast<void*>(hitSrc));
    }

    void Tick()
    {
        auto& net = NetworkClient::GetInstance();
        if (!net.IsConnected()) return;

        if (!g_helloSent) {
            g_helloSent = true;
            SendHello(false);
        }
        // Our own character, on connect and every 10 s (late joiners; receivers ignore repeats).
        static Clock::time_point s_lastLook{};
        if (Clock::now() - s_lastLook >= std::chrono::seconds(10)) {
            s_lastLook = Clock::now();
            auto blob = Appearance::LookToSend();   // saved "co-op look" if any, else this character
            if (!blob.empty()) {
                net.SendPacket(MessageType::CoopAppearance, blob.data(), static_cast<uint32_t>(blob.size()));
                g_stats.sent++;
                static bool s_dumped = false;   // debugging/test aid ([Debug] BodyTrace): the exact bytes we send
                if (!s_dumped && g_settings.hitTrace) {
                    s_dumped = true;
                    if (FILE* f = std::fopen("Data/F4SE/Plugins/F4MP_appearance.bin", "wb")) {
                        std::fwrite(blob.data(), 1, blob.size(), f);
                        std::fclose(f);
                    }
                    REX::INFO("[Appearance] local appearance captured ({} bytes)", blob.size());
                }
            }
        }
        Authority::Refresh();
        for (auto& item : net.DrainCoopInbox()) Dispatch(item);
        SendOwnVitals();   // health on name tags works whatever the plugin lists
        if (g_hostMismatch) return;
        if (g_settings.building) Workshop::Tick();

        if (g_settings.equipment) {
            PollLocalEquipment();
            ApplyEquipmentToBodies();
        }
        // A body is only ever hit by NPCs running on our game (mirrored ones have their AI off),
        // so every game forwards hits on bodies to their players.
        for (auto& [pid, bodyId] : GetBodies()) {
            auto* body = LookupForm<RE::Actor>(bodyId);
            HideGoreCaps(body);
            WatchBodyVisibility(pid, body);
        }
        if (g_settings.combat) {
            WatchBodyDamage();
            Explosions::Tick();
        }
        DynamicActors::ClientTick();

        const auto now = Clock::now();
        if (g_settings.quests && now - g_lastQuestPoll >= std::chrono::milliseconds(500)) {
            g_lastQuestPoll = now;
            CheckAppliedQuests();
            PollQuests();
        }
        if (g_settings.quests) {
            DrainQuestBacklog();
        }

        if (g_settings.items) PollLocks();
        SendCombatEvents();
        RetryAnimEvents();
        GraphOverride::CaptureNpcEvents(g_settings.combatAnims);
        SendNpcCombatEvents();

        static Clock::time_point s_lastTime{};
        if (IsHost() && g_settings.worldTime && now - s_lastTime >= std::chrono::seconds(2)) {
            s_lastTime = now;
            SendWorldTime();
        }
    }

    void OnDisconnected()
    {
        g_lastSentEquip.clear();
        g_lastSentMods.clear();
        g_wantEquip.clear();
        g_wantMods.clear();
        g_appliedEquip.clear();
        g_appliedMods.clear();
        g_animRetry.clear();
        GraphOverride::ClearMirrors();
        DynamicActors::Reset();
        Workshop::Reset();
        Authority::Reset();
        g_npcAppliedHealth.clear();
        g_npcOwners.clear();
        Explosions::Reset();
        g_pendingSnapshot.clear();
        g_questStages.clear();
        g_questBacklog.clear();
        g_lockWatch.clear();
        g_questBaseline = false;
        g_snapshotRequested = false;
        g_helloSent = false;
        g_hostMismatch = false;
        g_badSenders.clear();
        NetworkClient::GetInstance().DrainCoopInbox();
    }

    bool GetPlayerVitals(uint32_t playerId, float& health, float& maxHealth, bool& dead)
    {
        std::lock_guard lk(g_vitalsMutex);
        auto it = g_vitals.find(playerId);
        if (it == g_vitals.end()) return false;
        health = it->second.health;
        maxHealth = it->second.maxHealth;
        dead = it->second.dead;
        return true;
    }

    float GetHealth(RE::Actor* actor)
    {
        auto* hp = HealthAV();
        return actor && hp ? actor->GetActorValue(*hp) : 0.0f;
    }

    bool EquipmentSettled(uint32_t playerId, uint32_t bodyId)
    {
        auto want = g_wantEquip.find(playerId);
        if (want == g_wantEquip.end()) return true;   // nothing heard yet: the body keeps what it has
        auto applied = g_appliedEquip.find(bodyId);
        if (applied == g_appliedEquip.end() || applied->second != want->second) return false;
        const WantMods& wm = g_wantMods[playerId];
        auto mods = g_appliedMods.find(bodyId);
        return mods != g_appliedMods.end() && mods->second == ModKey(wm.weapon, wm.mods, wm.count);
    }

    void ApplyNpcVitals(RE::Actor* actor, uint32_t ownerId, uint32_t formId, float hostHealth, bool hostDead)
    {
        auto* hp = HealthAV();
        if (!actor || !hp) return;
        const float cur = actor->GetActorValue(*hp);
        const uint32_t local = actor->GetFormID();

        g_npcOwners[local] = { ownerId, formId, Clock::now() };   // our hits on it go to the owner (HitSink)

        if (hostDead) {
            if (!actor->IsDead(false)) actor->KillImpl(nullptr, 0.0f, true, false);
            g_npcAppliedHealth.erase(local);
            return;
        }
        if (actor->IsDead(false)) return;  // our killing blow is on its way to the host

        if (std::fabs(cur - hostHealth) > 0.5f)
            actor->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, hostHealth - cur);
        g_npcAppliedHealth[local] = hostHealth;
    }
}
