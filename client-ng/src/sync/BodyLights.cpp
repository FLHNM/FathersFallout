// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/BodyLights.h"

#include <unordered_map>

namespace F4MP::BodyLights
{
    namespace
    {
        // defaultPipboyLight, in power armor too: defaultPowerArmorHeadlampLight placed on its own gave no
        // light at all, so a frame's "headlamp" is this one at helmet height.
        constexpr uint32_t kPipboyLight = 0x0001ED2D;
        // Where the light sits on the body: chest height (Pip-Boy), head height in power armor.
        constexpr float kChestHeight = 95.0f, kHelmetHeight = 150.0f;

        struct Lit { uint32_t ref = 0; bool armor = false; RE::TESObjectCELL* cell = nullptr; uint32_t color = 0; void* tinted = nullptr; };
        std::unordered_map<uint32_t, Lit> g_lit;   // playerId -> its light reference

        RE::TESObjectREFR* RefOf(uint32_t a_fid)
        {
            auto* f = a_fid ? RE::TESForm::GetFormByID(a_fid) : nullptr;
            return f ? f->As<RE::TESObjectREFR>() : nullptr;
        }

        void Remove(Lit& a_lit)
        {
            if (auto* r = RefOf(a_lit.ref)) {
                r->Disable();
                r->SetDelete(true);
            }
            a_lit = {};
        }

        // A little in front of the body: inside the chest the body shaded its own front.
        constexpr float kAhead = 30.0f;

        RE::NiPoint3 LightSpot(RE::Actor* a_body, bool a_armor)
        {
            auto p = a_body->GetPosition();
            const float h = a_body->GetHeading();
            p.x += std::sin(h) * kAhead;
            p.y += std::cos(h) * kAhead;
            p.z += a_armor ? kHelmetHeight : kChestHeight;
            return p;
        }

        RE::NiLight* FindLight(RE::NiAVObject* a_obj, int a_depth = 0)
        {
            if (!a_obj || a_depth > 6) return nullptr;
            if (auto* l = netimmerse_cast<RE::NiLight*>(a_obj)) return l;
            if (auto* n = a_obj->IsNode())
                for (auto& c : n->children)
                    if (auto* l = FindLight(c.get(), a_depth + 1)) return l;
            return nullptr;
        }

        // Its player's Pip-Boy color on this light's own 3D (the record is shared by every light, ours too).
        void Tint(Lit& a_lit, RE::TESObjectREFR* a_ref)
        {
            auto* root = a_ref->Get3D();
            if (!a_lit.color || !root || a_lit.tinted == root) return;
            auto* light = FindLight(root);
            if (!light) return;
            light->diff = { ((a_lit.color >> 16) & 0xFF) / 255.0f, ((a_lit.color >> 8) & 0xFF) / 255.0f, (a_lit.color & 0xFF) / 255.0f };
            a_lit.tinted = root;
        }

        bool Place(Lit& a_lit, RE::Actor* a_body, bool a_armor)
        {
            auto* base = RE::TESForm::GetFormByID(kPipboyLight);
            auto* obj = base ? base->As<RE::TESBoundObject>() : nullptr;
            auto* dh = RE::TESDataHandler::GetSingleton();
            auto* cell = a_body->GetParentCell();
            if (!obj || !dh || !cell) return false;
            RE::NEW_REFR_DATA data;
            data.location = LightSpot(a_body, a_armor);
            data.direction = { 0.0f, 0.0f, a_body->GetHeading() };
            data.object = obj;
            if (cell->IsInterior()) data.interior = cell;
            else data.world = cell->worldSpace;
            auto ref = dh->CreateReferenceAtLocation(data).get();
            if (!ref) return false;
            a_lit = { ref->GetFormID(), a_armor, cell, a_lit.color, nullptr };
            return true;
        }
    }

    void Sync(const std::vector<Want>& a_bodies)
    {
        // Players whose body is gone (left, out of range, being rebuilt): their light goes too.
        for (auto it = g_lit.begin(); it != g_lit.end();) {
            const bool still = std::any_of(a_bodies.begin(), a_bodies.end(), [&](const Want& w) { return w.playerId == it->first && w.body; });
            if (still) { ++it; continue; }
            Remove(it->second);
            it = g_lit.erase(it);
        }
        for (const auto& w : a_bodies) {
            if (!w.body) continue;
            auto found = g_lit.find(w.playerId);
            if (!w.on || !w.body->Get3D()) {
                if (found != g_lit.end()) {
                    Remove(found->second);
                    g_lit.erase(found);
                    REX::INFO("[BodyLights] player {}: light off", w.playerId);
                }
                continue;
            }
            const bool armor = RE::PowerArmor::ActorInPowerArmor(*w.body);
            Lit& lit = g_lit[w.playerId];
            auto* ref = RefOf(lit.ref);
            // A new cell (door, fast travel) or power armor on/off: a fresh light, at the right height.
            if (!ref || ref->IsDeleted() || lit.armor != armor || lit.cell != w.body->GetParentCell()) {
                const bool had = ref != nullptr;
                Remove(lit);
                lit.color = w.color;
                if (Place(lit, w.body, armor))
                    REX::INFO("[BodyLights] player {}: {} {} ({:#x})", w.playerId, armor ? "headlamp (power armor)" : "Pip-Boy light",
                        had ? "moved over" : "on", lit.ref);
                continue;
            }
            // The reference's position alone left the light where it was put: its 3D moves too.
            if (lit.color != w.color) { lit.color = w.color; lit.tinted = nullptr; }
            Tint(lit, ref);   // once its 3D has loaded (a frame or so after placing)
            const auto spot = LightSpot(w.body, armor);
            ref->SetLocationOnReference(spot);
            ref->SetAngleOnReference({ 0.0f, 0.0f, w.body->GetHeading() });
            ref->Update3DPosition(true);
            if (auto* node = ref->Get3D()) {
                const auto d = node->world.translate - spot;
                if (d.x * d.x + d.y * d.y + d.z * d.z > 1.0f) {
                    node->local.translate = spot;
                    RE::NiUpdateData ud{};
                    node->UpdateWorldData(&ud);
                }
            }
        }
    }

    uint32_t LocalColor()
    {
        auto channel = [](const char* a_name, float a_default) {
            auto* s = RE::GetINISetting(a_name);
            const float v = s ? s->GetFloat() : a_default;
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        const uint32_t c = (channel("fPipboyEffectColorR:Pipboy", 0.08f) << 16) | (channel("fPipboyEffectColorG:Pipboy", 1.0f) << 8) |
                           channel("fPipboyEffectColorB:Pipboy", 0.09f);
        return c ? c : 0x010101;   // 0 on the wire means "an older game: no color sent"
    }

    void Clear()
    {
        for (auto& [pid, lit] : g_lit) Remove(lit);
        g_lit.clear();
    }
}
