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
#include "sync/LoadOrder.h"
#include "sync/Range.h"
#include "sync/Workshop.h"

#include <algorithm>
#include <atomic>
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
        // CoopPlayerDamageMsg::byNpc: 0 = PvP, 1 = an NPC (0.19 games), else the NPC's FormID in the sender's game.
        constexpr uint32_t kNpcCauseMin = 0x800;

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
        uint8_t ReadInstanceMods(const RE::BGSObjectInstanceExtra* inst, CoopModEntry* out)
        {
            if (!inst || !inst->values) return 0;
            uint8_t n = 0;
            for (auto& d : inst->GetIndexData()) {
                if (n >= kMaxItemMods) break;
                if (d.disabled || !d.objectID) continue;
                out[n++] = { d.objectID, d.index, d.rank, {} };
            }
            return n;
        }

        uint8_t ReadMods(const RE::ExtraDataList* extra, CoopModEntry* out)
        {
            return extra ? ReadInstanceMods(extra->GetByType<RE::BGSObjectInstanceExtra>(), out) : 0;
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

        // One item with the sender's mods, straight into a container: a fresh stack whose instance data
        // carries them (a modded gun put in a box came out as the plain base gun on the other side).
        bool AddModdedItem(RE::TESObjectREFR* container, RE::TESBoundObject* obj, const CoopModEntry* mods, uint8_t count)
        {
            RE::BSTSmartPointer<RE::ExtraDataList> list{ new RE::ExtraDataList() };
            auto* inst = new RE::BGSObjectInstanceExtra();
            int ok = 0;
            for (uint8_t i = 0; i < std::min<uint8_t>(count, static_cast<uint8_t>(kMaxItemMods)); ++i)
                if (auto* mod = LookupForm<RE::BGSMod::Attachment::Mod>(mods[i].modFormId)) {
                    inst->AddMod(*mod, mods[i].index, mods[i].rank, false);
                    ++ok;
                }
            if (!ok) { delete inst; return false; }
            list->AddExtra(inst);
            container->AddObjectToContainer(obj, list, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
            return true;
        }

        // The mods on the stack that just moved into a container. The event's unique ID is 0 for the
        // player's items (seen in game), so: the event's stack when it names one, else the first stack of
        // that item we haven't reported yet (two pipe pistols put in one after another are two stacks).
        std::unordered_map<uint64_t, std::unordered_set<const RE::ExtraDataList*>> g_reportedStacks;   // (container, item)

        uint8_t ReadMovedStackMods(RE::TESObjectREFR* container, uint32_t baseId, uint16_t uniqueId, CoopModEntry* out)
        {
            auto* inv = container ? container->inventoryList : nullptr;
            if (!inv) return 0;
            auto& reported = g_reportedStacks[(static_cast<uint64_t>(container->GetFormID()) << 32) | baseId];
            for (auto& item : inv->data) {
                if (!item.object || item.object->GetFormID() != baseId) continue;
                for (auto* s = item.stackData.get(); s; s = s->nextStack.get()) {
                    auto* uid = s->extra ? s->extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                    if (uniqueId && uid && uid->uniqueID == uniqueId) return ReadMods(s->extra.get(), out);
                }
                for (auto* s = item.stackData.get(); s; s = s->nextStack.get())
                    if (s->extra && reported.insert(s->extra.get()).second) return ReadMods(s->extra.get(), out);
            }
            return 0;
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
        struct BodyVis { Clock::time_point firstSeen{}, no3DSince{}, lastLog{}, lastFadeLog{}; bool no3D = false; };
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
                // Some fades come back every frame (seen 10/s after a body rebuild): log the first few.
                if (now - v.lastFadeLog > std::chrono::seconds(5)) {
                    v.lastFadeLog = now;
                    REX::WARN("[Body] player {}: body stuck faded out ({:.2f}), forced visible", pid, fade->currentFade);
                }
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

        std::atomic<int64_t> g_loadedAtMs{ 0 };   // steady clock, last kPostLoadGame

        void OnPlayerDamage(const CoopPlayerDamageMsg& m)
        {
            auto& net = NetworkClient::GetInstance();
            if (m.targetPlayerId != net.GetPlayerId()) return;
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* hp = HealthAV();
            if (!pl || !hp || m.damage <= 0.0f || m.damage > 100000.0f) return;
            // Hits that queued up while we lay dead and the save reloaded landed on the fresh character
            //.
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            if (pl->IsDead(false) || nowMs - g_loadedAtMs.load() < 3000) {
                static int s_dropped = 0;
                if (s_dropped++ < 20) REX::INFO("[Coop] player {}'s hit for {:.1f} dropped (dead or just loaded)", m.senderId, m.damage);
                return;
            }
            // An NPC we can't see hit us (it isn't in our world, isn't loaded, or its plugin isn't installed):
            // no hits from enemies we can't fight back.
            if (m.byNpc >= kNpcCauseMin) {
                RE::Actor* npc = m.byNpc >= DynamicActors::kRuntimeIdStart ? DynamicActors::Resolve(m.senderId, m.byNpc)
                                                                           : LookupForm<RE::Actor>(m.byNpc);
                const bool visible = npc && npc->Get3D() && !npc->IsDisabled() && !npc->IsDead(false) &&
                                     npc->GetPosition().GetDistance(pl->GetPosition()) < 12000.0f;
                if (!visible) {
                    static int s_unseen = 0;
                    if (s_unseen++ < 30)
                        REX::WARN("[Coop] player {}'s NPC {:#x} hit us for {:.1f}, but it isn't here for us ({}): ignored", m.senderId,
                            m.byNpc, m.damage, !npc ? "no such NPC" : !npc->Get3D() ? "not loaded" : npc->IsDisabled() ? "disabled" : npc->IsDead(false) ? "dead here" : "far away");
                    return;
                }
            }
            pl->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -m.damage);
            g_stats.applied++;
            const float left = pl->GetActorValue(*hp);
            if (m.byNpc) REX::INFO("[Coop] an NPC in player {}'s game hit us for {:.1f} ({:.0f} health left)", m.senderId, m.damage, left);
            else REX::INFO("[Coop] player {} hit us for {:.1f} ({:.0f} health left)", m.senderId, m.damage, left);
            // Health alone can sit at 0 without the player dying; the real hit would have killed.
            if (left <= 0.0f && !pl->IsDead(false)) {
                // Only a real PvP shot names the other player's body as the killer (an NPC's kill credited to
                // the friend's body: 's "killed by player 1").
                RE::Actor* attacker = nullptr;
                if (!m.byNpc)
                    for (auto& [pid, bodyId] : GetBodies())
                        if (pid == m.senderId) attacker = LookupForm<RE::Actor>(bodyId);
                pl->KillImpl(attacker, m.damage, true, false);
                REX::INFO("[Coop] killed by {} {}", m.byNpc ? "an NPC in player" : "player", m.senderId);
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

        // ---------- NPC equipment ----------
        // What an NPC wears and holds as its own game shows it. Armor from the biped (the models really
        // on it; skin and race parts aren't in its inventory), the weapon from its equipped items, that
        // weapon's mods from its inventory stack or, drawn, from the model in its hands.
        struct Worn
        {
            std::vector<uint32_t> forms;   // sorted: armor + weapon
            uint32_t weapon = 0;
            uint8_t numMods = 0;
            CoopModEntry mods[kMaxItemMods]{};
        };

        bool IsWeapon(const RE::TESForm* f) { return f && f->GetFormType() == RE::ENUM_FORM_ID::kWEAP; }

        Worn ReadWorn(RE::Actor* a)
        {
            Worn w;
            if (!a) return w;
            auto* biped = a->biped.get();
            if (biped) {
                for (std::size_t i = 0; i < std::to_underlying(RE::BIPED_OBJECT::kEditorTotal); ++i) {
                    auto* obj = biped->object[i].parent.object;
                    auto* bound = obj && obj->GetFormType() == RE::ENUM_FORM_ID::kARMO ? obj->As<RE::TESBoundObject>() : nullptr;
                    if (bound && a->GetInventoryObjectCount(bound) > 0) w.forms.push_back(bound->GetFormID());
                }
            }
            auto* mh = a->currentProcess ? a->currentProcess->middleHigh : nullptr;
            if (mh) {
                RE::BSAutoLock l{ mh->equippedItemsLock };
                for (auto& e : mh->equippedItems)
                    if (IsWeapon(e.item.object)) { w.weapon = e.item.object->GetFormID(); break; }
            }
            if (w.weapon) {
                w.forms.push_back(w.weapon);
                // Mods: the equipped stack, else the drawn model's, else the only modded stack it carries.
                const RE::BGSObjectInstanceExtra* drawnMods = nullptr;
                if (biped)
                    for (std::size_t i = std::to_underlying(RE::BIPED_OBJECT::kEditorTotal); i < std::to_underlying(RE::BIPED_OBJECT::kTotal); ++i)
                        if (auto& b = biped->object[i]; b.parent.object && b.parent.object->GetFormID() == w.weapon && b.modExtra) { drawnMods = b.modExtra; break; }
                uint8_t anyMods = 0;
                CoopModEntry any[kMaxItemMods]{};
                if (auto* inv = a->inventoryList)
                    for (auto& item : inv->data) {
                        if (!item.object || item.object->GetFormID() != w.weapon) continue;
                        for (auto* st = item.stackData.get(); st && !w.numMods; st = st->nextStack.get()) {
                            if (st->IsEquipped()) w.numMods = ReadMods(st->extra.get(), w.mods);
                            else if (!anyMods) anyMods = ReadMods(st->extra.get(), any);
                        }
                    }
                if (!w.numMods && drawnMods) w.numMods = ReadInstanceMods(drawnMods, w.mods);
                if (!w.numMods && anyMods) { w.numMods = anyMods; std::copy_n(any, anyMods, w.mods); }
            }
            std::sort(w.forms.begin(), w.forms.end());
            w.forms.erase(std::unique(w.forms.begin(), w.forms.end()), w.forms.end());
            return w;
        }

        std::vector<uint32_t> WornKey(const Worn& w)
        {
            auto k = ModKey(w.weapon, w.mods, w.numMods);
            k.insert(k.end(), w.forms.begin(), w.forms.end());
            return k;
        }

        struct SentWorn { std::vector<uint32_t> key; Clock::time_point at; };
        std::unordered_map<uint32_t, SentWorn> g_npcWornSent;                   // owner: our FormID -> last sent
        std::unordered_map<uint64_t, Worn> g_npcWornWant;                       // mirror: (owner, owner FormID) -> latest
        std::unordered_map<uint32_t, std::vector<uint32_t>> g_npcWornApplied;   // mirror: local FormID -> applied key
        std::unordered_map<uint32_t, Clock::time_point> g_npcRedrawAt;          // mirror: weapon swapped, draw it again after

        // Owner side, once a second: every NPC our game runs, sent when it changes and every 10 s (late
        // joiners, freshly spawned copies).
        void SendNpcEquipment()
        {
            static Clock::time_point s_next{};
            const auto now = Clock::now();
            if (now < s_next) return;
            s_next = now + std::chrono::seconds(1);
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* tes = RE::TES::GetSingleton();
            if (!pl || !tes) return;
            std::vector<RE::Actor*> mine;
            tes->ForEachReferenceInRange(pl->GetPosition(), Range::Loaded(), [&](RE::TESObjectREFR* ref) {
                auto* a = ref ? ref->As<RE::Actor>() : nullptr;
                const uint32_t fid = a ? a->GetFormID() : 0;
                // Body bases are built at runtime (>= FFF00000): a body just spawned isn't registered yet.
                auto* base = a ? a->GetObjectReference() : nullptr;
                if (base && base->GetFormID() >= 0xFFF00000) return RE::BSContainer::ForEachResult::kContinue;
                if (a && a != pl && !a->IsDisabled() && !a->IsDead(false) && a->Get3D() && !GetPlayerIdForBody(fid) &&
                    !(fid >= kDynamicFormIdStart && DynamicActors::IsMirrorCopy(fid)) && Authority::IOwn(a))
                    mine.push_back(a);
                return RE::BSContainer::ForEachResult::kContinue;
            });
            int sent = 0;
            for (auto* a : mine) {
                if (sent >= 16) break;
                const Worn w = ReadWorn(a);
                auto key = WornKey(w);
                auto& last = g_npcWornSent[a->GetFormID()];
                if (last.key == key && now - last.at < std::chrono::seconds(10)) continue;
                if (last.key != key)
                    REX::INFO("[NpcGear] our NPC {:#x}: weapon {:08X} ({} mods), {} items sent", a->GetFormID(), w.weapon, w.numMods, w.forms.size());
                last = { std::move(key), now };
                CoopNpcEquipmentMsg msg{};
                msg.npcFormId = a->GetFormID();
                msg.count = static_cast<uint8_t>(std::min<size_t>(w.forms.size(), std::size(msg.forms)));
                for (uint8_t i = 0; i < msg.count; ++i) msg.forms[i] = w.forms[i];
                msg.weaponFormId = w.weapon;
                msg.numWeaponMods = w.numMods;
                std::copy_n(w.mods, w.numMods, msg.weaponMods);
                Send(MessageType::CoopNpcEquipment, msg);
                ++sent;
            }
        }

        void OnNpcEquipment(const CoopNpcEquipmentMsg& m)
        {
            Worn w;
            const uint8_t n = std::min<uint8_t>(m.count, static_cast<uint8_t>(std::size(m.forms)));
            w.forms.assign(m.forms, m.forms + n);
            std::sort(w.forms.begin(), w.forms.end());
            w.weapon = m.weaponFormId;
            w.numMods = std::min<uint8_t>(m.numWeaponMods, static_cast<uint8_t>(kMaxItemMods));
            std::copy_n(m.weaponMods, w.numMods, w.mods);
            g_npcWornWant[(static_cast<uint64_t>(m.senderId) << 32) | m.npcFormId] = std::move(w);
        }

        // Mirror side: dress and arm our copy as its owner's is (a few NPCs per tick: equipping rebuilds models).
        void ApplyNpcEquipment()
        {
            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return;
            int done = 0;
            const auto now = Clock::now();
            for (auto& [key, want] : g_npcWornWant) {
                if (done >= 3) break;
                const uint32_t owner = static_cast<uint32_t>(key >> 32), fid = static_cast<uint32_t>(key);
                auto* a = fid >= kDynamicFormIdStart ? DynamicActors::Resolve(owner, fid) : LookupForm<RE::Actor>(fid);
                if (!a || !a->Get3D() || a->IsDead(false) || Authority::IOwn(a)) continue;
                const uint32_t local = a->GetFormID();
                auto o = g_npcOwners.find(local);   // only while mirrored (driven by NpcsSync)
                if (o == g_npcOwners.end() || o->second.ownerId != owner || now - o->second.at > std::chrono::seconds(2)) continue;
                auto wantKey = WornKey(want);
                auto& applied = g_npcWornApplied[local];
                if (applied == wantKey) continue;
                ++done;

                const Worn cur = ReadWorn(a);
                auto has = [](const std::vector<uint32_t>& v, uint32_t f) { return std::binary_search(v.begin(), v.end(), f); };
                int off = 0, on = 0;
                // Armor it wears here but not there: off (left in its inventory; looting matches the owner's).
                // Not when the owner listed none at all: a model still loading there reads as naked.
                const bool ownerHasArmor = std::any_of(want.forms.begin(), want.forms.end(), [](uint32_t f) { return !IsWeapon(LookupForm<RE::TESForm>(f)); });
                for (uint32_t f : cur.forms) {
                    auto* obj = LookupForm<RE::TESBoundObject>(f);
                    if (!obj || IsWeapon(obj) || has(want.forms, f) || !ownerHasArmor) continue;
                    RE::BGSObjectInstance inst(obj, nullptr);
                    if (mgr->UnequipObject(a, &inst, 1, nullptr, 0, false, true, false, true, nullptr)) ++off;
                }
                for (uint32_t f : want.forms) {
                    auto* obj = LookupForm<RE::TESBoundObject>(f);
                    if (!obj || IsWeapon(obj) || has(cur.forms, f)) continue;
                    if (a->GetInventoryObjectCount(obj) == 0) AddItems(a, obj, 1);
                    RE::BGSObjectInstance inst(obj, nullptr);
                    if (mgr->EquipObject(a, inst, 0, 1, nullptr, false, true, false, true, false)) ++on;
                }
                // The weapon: the owner's one with its mods (a pipe pistol, not the base pipe gun's default build).
                const bool sameWeapon = cur.weapon == want.weapon &&
                    ModKey(cur.weapon, cur.mods, cur.numMods) == ModKey(want.weapon, want.mods, want.numMods);
                bool weaponChanged = false;
                if (want.weapon && !sameWeapon) {
                    if (auto* obj = LookupForm<RE::TESBoundObject>(want.weapon)) {
                        uint32_t stackId = 0;
                        RE::TBO_InstanceData* data = nullptr;
                        if (want.numMods) {
                            const auto n = static_cast<int32_t>(a->GetInventoryObjectCount(obj));
                            if (n > 0) RemoveItems(a, obj, n);   // its own build of that gun
                            WantMods wm;
                            wm.weapon = want.weapon;
                            wm.count = want.numMods;
                            std::copy_n(want.mods, want.numMods, wm.mods);
                            GiveModdedWeapon(a, obj, wm);
                            data = StackInstance(a, obj, stackId);
                        } else if (a->GetInventoryObjectCount(obj) == 0) {
                            AddItems(a, obj, 1);
                        }
                        // Rounds for it (its caliber after mods): an NPC holding a gun it has no ammo for
                        // never drew it, so the copy stood empty-handed while its owner's fired.
                        auto* weap = obj->As<RE::TESObjectWEAP>();
                        RE::TESAmmo* ammo = data ? static_cast<RE::TESObjectWEAP::InstanceData*>(data)->ammo : nullptr;
                        if (!ammo && weap) ammo = weap->weaponData.ammo;
                        if (ammo && a->GetInventoryObjectCount(ammo) < 20) AddItems(a, ammo, 40);
                        RE::BGSObjectInstance inst(obj, data);
                        weaponChanged = mgr->EquipObject(a, inst, stackId, 1, nullptr, false, true, false, true, false);
                    }
                }
                if (weaponChanged) {
                    a->Reset3D(false, 0, true, 0);   // else the old gun's model stays in its hands
                    g_npcRedrawAt[local] = now + std::chrono::milliseconds(800);   // NpcsSync draws the new one
                }
                applied = std::move(wantKey);
                REX::INFO("[NpcGear] mirror {:#x} (player {}'s {:#x}): weapon {:08X} -> {:08X} ({} mods{}), armor {} off {} on", local, owner,
                    fid, cur.weapon, want.weapon, want.numMods, weaponChanged ? ", equipped" : "", off, on);
            }
        }

        // ---------- items ----------

        std::unordered_set<uint32_t> g_pendingSnapshot;  // containers we asked the host about

        void OnContainerChange(const CoopContainerChangeMsg& m)
        {
            auto* cont = ResolveRef(m.ownerId, m.containerFormId);
            auto* obj = LookupForm<RE::TESBoundObject>(m.itemFormId);
            if (!cont || !obj || m.count == 0) return;
            if (m.count > 0 && m.numMods > 0) {
                int32_t added = 0;
                for (int32_t i = 0; i < std::min(m.count, 20); ++i)
                    if (AddModdedItem(cont, obj, m.mods, m.numMods)) ++added;
                if (added < m.count) AddItems(cont, obj, m.count - added);
                CoopModEntry back[kMaxItemMods]{};
                uint8_t seen = 0;
                if (auto* inv = cont->inventoryList)
                    for (auto& it : inv->data)
                        if (it.object == obj)
                            for (auto* st = it.stackData.get(); st && !seen; st = st->nextStack.get()) seen = ReadMods(st->extra.get(), back);
                REX::INFO("[Coop] player {} put {:08X} x{} with {} mod(s) into {:#x}: {} modded, stack reads {} mod(s)", m.senderId,
                    m.itemFormId, m.count, m.numMods, cont->GetFormID(), added, seen);
            } else if (m.count > 0) {
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

        // Our plugin names by load-order slot: lets every other game translate our FormIDs (sync/LoadOrder).
        void SendPluginList()
        {
            const auto list = LoadOrder::BuildMyList(IsHost());
            std::vector<uint8_t> payload(4, 0);   // senderId, stamped by the server
            payload.insert(payload.end(), list.begin(), list.end());
            NetworkClient::GetInstance().SendPacket(MessageType::CoopPluginList, payload.data(), static_cast<uint32_t>(payload.size()));
            g_stats.sent++;
        }

        void OnPluginList(uint32_t sender, const std::vector<uint8_t>& payload)
        {
            if (payload.size() <= 4 || sender == Authority::MyId()) return;
            const bool fresh = !LoadOrder::Known(sender);
            LoadOrder::OnList(sender, payload.data() + 4, payload.size() - 4);
            if (!LoadOrder::Known(sender)) return;
            // Their FormIDs are translated from now on: nothing has to be paused any more.
            g_badSenders.erase(sender);
            if (sender == Authority::HostId()) g_hostMismatch = false;
            const auto diff = LoadOrder::Differences(sender);
            if (fresh && !diff.empty()) {
                std::string who = Appearance::NameOf(sender);
                if (who.empty()) who = std::format("Player {}", sender);
                const std::string text = std::format("FathersFallout: {}'s mods differ from yours. {}Things from those mods won't show for both of you.", who, diff);
                REX::WARN("[LoadOrder] notice: {}", text);
                RE::SendHUDMessage::ShowHUDMessage(text.c_str(), nullptr, false, false);
            }
        }

        void OnHello(const CoopHelloMsg& m)
        {
            const auto& fp = MyFingerprint();
            // A known plugin list beats the fingerprint: different lists are translated, not paused.
            const bool same = m.loadOrderHash == fp.hash || LoadOrder::Known(m.senderId);
            // A newcomer: tell them our plugins (everyone answers, so guests learn each other's lists too).
            if (!m.isReply && m.senderId != Authority::MyId()) SendPluginList();
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
            if (LoadOrder::Known(senderId)) return true;   // their FormIDs are translated
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
                    msg.numMods = ReadMovedStackMods(LookupForm<RE::TESObjectREFR>(e.newContainerFormID), e.baseObjectFormID, e.uniqueID, msg.mods);
                    if (msg.numMods)
                        REX::INFO("[Coop] put {:08X} x{} with {} mod(s) into {:#x}: sent", e.baseObjectFormID, e.itemCount, msg.numMods, e.newContainerFormID);
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
                // What's already inside isn't what we'll put in (ReadMovedStackMods).
                if (auto* inv = target->inventoryList)
                    for (auto& item : inv->data)
                        if (item.object)
                            for (auto* st = item.stackData.get(); st; st = st->nextStack.get())
                                if (st->extra) g_reportedStacks[(static_cast<uint64_t>(fid) << 32) | item.object->GetFormID()].insert(st->extra.get());

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
        

        // Our player, or a companion following our player in our own game (Dogmeat, Piper...). Their hits on an
        // NPC another game runs went nowhere: the copy's health is the owner's, so a companion's bites and
        // shots did nothing to the other player's enemies.
        // A copy of an NPC another player's game runs (its hits count there, not here).
        bool IsMirroredNpc(uint32_t fid)
        {
            auto o = g_npcOwners.find(fid);
            return o != g_npcOwners.end() && Clock::now() - o->second.at < std::chrono::seconds(2);
        }

        bool IsOurSide(RE::TESObjectREFR* cause)
        {
            if (!cause) return false;
            const uint32_t fid = cause->GetFormID();
            if (fid == kPlayerRefId) return true;
            auto* a = cause->As<RE::Actor>();
            if (!a || GetPlayerIdForBody(fid)) return false;
            if (auto o = g_npcOwners.find(fid); o != g_npcOwners.end() && Clock::now() - o->second.at < std::chrono::seconds(2))
                return false;   // a copy of someone else's companion: theirs counts in their game
            static auto* s_companions = LookupForm<RE::TESFaction>(0x00023C01);   // CurrentCompanionFaction
            return s_companions && a->IsInFaction(s_companions);
        }

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
                // A copy of another player's NPC hit us: its owner's game decides what its attacks do and
                // sends the real hit (CoopPlayerDamage). Counting this one too hurt twice.
                if (auto* pl = RE::PlayerCharacter::GetSingleton(); target == pl && cause && cause != pl &&
                        e.usesHitData && !e.hitData.flags.all(RE::HitData::Flag::kExplosion) && IsMirroredNpc(cause->GetFormID())) {
                    const float back = e.hitData.healthDamage;
                    if (back > 0.0f) if (auto* hp = HealthAV()) pl->RestoreActorValue(*hp, back);
                    static int s_mirrorBack = 0;
                    if (s_mirrorBack++ < 30) REX::INFO("[Coop] mirrored NPC {:#x}'s replayed attack hit us for {:.1f}: given back (its owner sends the real hit)", cause->GetFormID(), back);
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
                        !IsOurSide(cause) || dmg <= 0.0f)
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
                    REX::INFO("[Coop] {} hit player {}'s NPC {:#x} for {:.1f}: forwarded",
                        cause->GetFormID() == kPlayerRefId ? "we" : "our companion", msg.ownerId, msg.npcFormId, dmg);
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (dmg <= 0.0f) return RE::BSEventNotifyControl::kContinue;
                // Our copy of an NPC someone else runs: the real one is in its owner's game, which sends
                // the hit itself (this one doubled every hit on a player standing in another's area).
                if (cause->GetFormID() != kPlayerRefId && IsMirroredNpc(cause->GetFormID())) return RE::BSEventNotifyControl::kContinue;
                CoopPlayerDamageMsg msg{};
                msg.targetPlayerId = pid;
                msg.damage = dmg;
                // Which NPC: the target only takes the hit if it can see that NPC (a player report: enemies
                // nobody could see kept hurting both players).
                msg.byNpc = cause->GetFormID() == kPlayerRefId ? 0u : std::max(cause->GetFormID(), kNpcCauseMin);
                Send(MessageType::CoopPlayerDamage, msg);
                g_stats.damageForwarded++;
                REX::INFO("[Coop] body of player {} hit by {:#x} for {:.1f}: forwarded", pid, cause->GetFormID(), dmg);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        ContainerSink g_containerSink;
        ActivateSink g_activateSink;
        HitSink g_hitSink;

        // Every FormID in a message is the sender's (or, where noted, a key that names the object in its
        // owner's game and is only compared, never looked up). Players with other mods or Creation Club
        // content number the same forms differently: rewrite them into ours (sync/LoadOrder).
        template <class T>
        T* Mut(CoopInboxItem& item)
        {
            return item.payload.size() >= sizeof(T) ? reinterpret_cast<T*>(item.payload.data()) : nullptr;
        }

        void TranslateInbound(CoopInboxItem& item, uint32_t sender)
        {
            auto tr = [sender](uint32_t& fid) { fid = LoadOrder::ToLocal(sender, fid); };
            auto mods = [&](CoopModEntry* m, uint8_t& n) {
                uint8_t kept = 0;
                for (uint8_t i = 0; i < std::min<uint8_t>(n, static_cast<uint8_t>(kMaxItemMods)); ++i) {
                    tr(m[i].modFormId);
                    if (m[i].modFormId) m[kept++] = m[i];
                }
                n = kept;
            };
            // Worn/held lists: items from a plugin we don't have are left out, not sent on as 0.
            auto forms = [&](uint32_t* f, uint8_t& n, size_t cap) {
                uint8_t kept = 0;
                for (uint8_t i = 0; i < std::min<size_t>(n, cap); ++i) {
                    tr(f[i]);
                    if (f[i]) f[kept++] = f[i];
                }
                n = kept;
            };
            switch (item.type) {
            case MessageType::CoopDynamicActor:
                if (auto* m = Mut<CoopDynamicActorMsg>(item)) tr(m->spawnFormId);   // hostRefId: a runtime ID
                break;
            case MessageType::CoopAppearance: {
                // sync/Appearance.cpp Header v1: race @8, head part count @13, hair/beard colour and face
                // details @20/24/28, head parts after the 116-byte header.
                auto& b = item.payload;
                uint32_t version = 0;
                if (b.size() < 116 || (std::memcpy(&version, b.data() + 4, 4), version != 1)) break;
                auto at = [&](size_t off) { uint32_t v; std::memcpy(&v, b.data() + off, 4); tr(v); std::memcpy(b.data() + off, &v, 4); };
                at(8); at(20); at(24); at(28);
                const size_t parts = b[13];
                for (size_t i = 0; i < parts && 116 + i * 4 + 4 <= b.size(); ++i) at(116 + i * 4);
                break;
            }
            case MessageType::CoopEquipment:
                if (auto* m = Mut<CoopEquipmentMsg>(item)) {
                    forms(m->forms, m->count, std::size(m->forms));
                    tr(m->weaponFormId);
                    mods(m->weaponMods, m->numWeaponMods);
                }
                break;
            case MessageType::CoopNpcEquipment:
                if (auto* m = Mut<CoopNpcEquipmentMsg>(item)) {
                    tr(m->npcFormId);
                    forms(m->forms, m->count, std::size(m->forms));
                    tr(m->weaponFormId);
                    mods(m->weaponMods, m->numWeaponMods);
                }
                break;
            case MessageType::CoopNpcAnim:
                if (auto* m = Mut<CoopNpcAnimMsg>(item)) tr(m->npcFormId);
                break;
            case MessageType::CoopNpcDamage:
                if (auto* m = Mut<CoopNpcDamageMsg>(item)) tr(m->npcFormId);   // the hitter's copy, by its own FormID
                break;
            case MessageType::CoopPlayerDamage:
                if (auto* m = Mut<CoopPlayerDamageMsg>(item); m && m->byNpc >= kNpcCauseMin) tr(m->byNpc);
                break;
            case MessageType::CoopWorldReq:
                if (auto* m = Mut<CoopWorldReqMsg>(item)) tr(m->space);
                break;
            case MessageType::CoopRefsHidden:
                if (auto* m = Mut<CoopRefsHiddenMsg>(item))
                    for (uint32_t i = 0; i < std::min<uint32_t>(m->count, static_cast<uint32_t>(kMaxRefsPerMsg)); ++i) tr(m->refs[i]);
                break;
            case MessageType::CoopExplosion:
                if (auto* m = Mut<CoopExplosionMsg>(item)) { tr(m->explosionFormId); tr(m->space); }
                break;
            case MessageType::CoopContainerChange:
                if (auto* m = Mut<CoopContainerChangeMsg>(item)) { tr(m->containerFormId); tr(m->itemFormId); mods(m->mods, m->numMods); }
                break;
            case MessageType::CoopItemDropped:   // refFormId: the drop's key in the dropper's game
                if (auto* m = Mut<CoopItemDroppedMsg>(item)) { tr(m->itemFormId); tr(m->space); mods(m->mods, m->numMods); }
                break;
            case MessageType::CoopBuildPlaced:   // refFormId: the build's key in the builder's game
                if (auto* m = Mut<CoopBuildPlacedMsg>(item)) { tr(m->baseFormId); tr(m->workshopFormId); }
                break;
            case MessageType::CoopRefActivated:
            case MessageType::CoopFurniture:
                if (auto* m = Mut<CoopRefActivatedMsg>(item); m && m->ownerId == 0) tr(m->ownerRefFormId);   // a plugin reference
                break;
            case MessageType::CoopPropMoved:
                if (auto* m = Mut<CoopPropMovedMsg>(item)) tr(m->refFormId);
                break;
            case MessageType::CoopWorldRefTaken:
                if (auto* m = Mut<CoopWorldRefTakenMsg>(item)) tr(m->refFormId);
                break;
            case MessageType::CoopContainerSnapshotReq:
                if (auto* m = Mut<CoopContainerSnapshotReqMsg>(item)) tr(m->containerFormId);
                break;
            case MessageType::CoopContainerSnapshot:
                if (auto* m = Mut<CoopContainerSnapshotMsg>(item)) {
                    tr(m->containerFormId);
                    for (uint16_t i = 0; i < std::min<uint16_t>(m->count, static_cast<uint16_t>(std::size(m->entries))); ++i) tr(m->entries[i].itemFormId);
                }
                break;
            case MessageType::CoopQuestStage:
                if (auto* m = Mut<CoopQuestStageMsg>(item)) tr(m->questFormId);
                break;
            case MessageType::CoopLockChanged:
                if (auto* m = Mut<CoopLockChangedMsg>(item)) tr(m->refFormId);
                break;
            case MessageType::CoopWorldTime:
                if (auto* m = Mut<CoopWorldTimeMsg>(item)) tr(m->weatherFormId);
                break;
            default:
                break;   // no FormIDs, or only keys (drop taken, build moved/removed)
            }
        }

        void DispatchOne(const CoopInboxItem& item);

        void Dispatch(const CoopInboxItem& item)
        {
            const uint32_t sender = item.payload.size() >= 4 ? *reinterpret_cast<const uint32_t*>(item.payload.data()) : 0;
            if (item.type == MessageType::CoopHello || item.type == MessageType::CoopPluginList ||
                !LoadOrder::Known(sender) || LoadOrder::Identical(sender)) {
                DispatchOne(item);
                return;
            }
            CoopInboxItem copy = item;
            TranslateInbound(copy, sender);
            DispatchOne(copy);
        }

        void DispatchOne(const CoopInboxItem& item)
        {
            if (item.type == MessageType::CoopHello) {
                if (auto* m = As<CoopHelloMsg>(item)) OnHello(*m);
                return;
            }
            const uint32_t sender = item.payload.size() >= 4 ? *reinterpret_cast<const uint32_t*>(item.payload.data()) : 0;
            if (item.type == MessageType::CoopPluginList) {
                OnPluginList(sender, item.payload);
                return;
            }
            if (item.type != MessageType::CoopPlayerDamage && item.type != MessageType::CoopPlayerVitals &&
                !FormIdSyncAllowed(sender)) return;

            // Attack/reload replays that waited out a loading screen (25 of one raider's at once
            // right after an interior load) would all play now: shots nobody fired. Only fresh ones.
            if (item.type == MessageType::CoopNpcAnim || item.type == MessageType::CoopAnimEvent) {
                const auto now = Clock::now();
                const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                if (now - item.receivedAt > std::chrono::milliseconds(1500) || nowMs - g_loadedAtMs.load() < 2000) {
                    static int s_stale = 0;
                    if (s_stale++ < 10) REX::INFO("[Coop] stale combat animation dropped (waited {} ms)",
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - item.receivedAt).count());
                    return;
                }
            }
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
            case MessageType::CoopNpcEquipment:
                if (auto* m = As<CoopNpcEquipmentMsg>(item); m && g_settings.equipment && m->senderId != Authority::MyId()) OnNpcEquipment(*m);
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
    bool IsLoadOrderMismatch() { return g_hostMismatch && !LoadOrder::Known(Authority::HostId()); }
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
            SendPluginList();
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
        if (IsLoadOrderMismatch()) return;
        if (g_settings.building) Workshop::Tick();

        if (g_settings.equipment) {
            PollLocalEquipment();
            ApplyEquipmentToBodies();
            SendNpcEquipment();
            ApplyNpcEquipment();
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

    void OnGameLoaded()
    {
        g_loadedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    }

    void OnDisconnected()
    {
        LoadOrder::Reset();
        g_lastSentEquip.clear();
        g_lastSentMods.clear();
        g_wantEquip.clear();
        g_wantMods.clear();
        g_appliedEquip.clear();
        g_appliedMods.clear();
        g_npcWornSent.clear();
        g_npcWornWant.clear();
        g_npcWornApplied.clear();
        g_npcRedrawAt.clear();
        g_reportedStacks.clear();
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

    bool TakeNpcRedraw(uint32_t localFormId)
    {
        auto it = g_npcRedrawAt.find(localFormId);
        if (it == g_npcRedrawAt.end() || Clock::now() < it->second) return false;
        g_npcRedrawAt.erase(it);
        return true;
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
