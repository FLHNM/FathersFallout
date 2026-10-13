// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Appearance.h"
#include "sync/Paths.h"
#include "sync/FaceDonors.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <new>
#include <unordered_map>
#include <mutex>

namespace F4MP::Appearance
{
    namespace
    {
        constexpr uint32_t kFormatVersion = 1;
        constexpr uint32_t kPlayerTemplate = 0x00000007;
        // Runtime FormIDs for the per-player body bases. The engine hands out runtime IDs from
        // 0xFF000000 upward; this block sits far above anything a session reaches.
        constexpr uint32_t kBaseIdStart = 0xFFF00000;

#pragma pack(push, 1)
        struct Header
        {
            uint32_t senderId;
            uint32_t version;
            uint32_t race;
            uint8_t  female;
            uint8_t  numHeadParts;
            uint16_t numMorphSliders;
            uint16_t numRegionSliders;
            uint16_t numBoneRegions;
            uint32_t hairColor;
            uint32_t facialHairColor;
            uint32_t faceDetails;
            int8_t   bodyTint[4];
            float    morphWeight[3];
            float    height;
            char     name[64];
        };
        struct MorphSlider { uint32_t key; float value; };
        struct BoneRegion { uint32_t key; float pos[3]; float rot[3]; float scale[3]; };
#pragma pack(pop)

        // Parsed appearance of one remote player.
        struct Look
        {
            Header header{};
            std::vector<uint32_t> headParts;
            std::vector<MorphSlider> morphSliders;
            std::vector<float> regionSliders;
            std::vector<BoneRegion> boneRegions;
            std::vector<uint8_t> raw;     // for change detection
        };

        struct BodyBase { uint32_t formId = 0; bool built = false; };

        std::unordered_map<uint32_t, Look> g_looks;          // playerId -> appearance
        std::unordered_map<uint32_t, BodyBase> g_bases;      // playerId -> body base record
        std::unordered_map<uint32_t, bool> g_respawn;        // playerId -> appearance changed
        uint32_t g_nextBaseId = kBaseIdStart;

        uint32_t IdOf(const RE::TESForm* f) { return f ? f->GetFormID() : 0; }

        template <class T>
        T* Lookup(uint32_t id)
        {
            auto* f = id ? RE::TESForm::GetFormByID(id) : nullptr;
            return f ? f->As<T>() : nullptr;
        }

        template <class T>
        T* GameNew()
        {
            void* mem = RE::malloc(sizeof(T));
            return mem ? new (mem) T() : nullptr;
        }

        template <class T>
        void Append(std::vector<uint8_t>& out, const T& v)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + sizeof(T));
        }

        bool Parse(const uint8_t* data, size_t size, Look& out)
        {
            if (size < sizeof(Header)) return false;
            std::memcpy(&out.header, data, sizeof(Header));
            const auto& h = out.header;
            if (h.version != kFormatVersion) return false;
            size_t need = sizeof(Header) + h.numHeadParts * sizeof(uint32_t) + h.numMorphSliders * sizeof(MorphSlider) +
                          h.numRegionSliders * sizeof(float) + h.numBoneRegions * sizeof(BoneRegion);
            if (size < need) return false;
            const uint8_t* p = data + sizeof(Header);
            auto take = [&](auto& vec, size_t n) {
                using E = typename std::remove_reference_t<decltype(vec)>::value_type;
                vec.resize(n);
                std::memcpy(vec.data(), p, n * sizeof(E));
                p += n * sizeof(E);
            };
            take(out.headParts, h.numHeadParts);
            take(out.morphSliders, h.numMorphSliders);
            take(out.regionSliders, h.numRegionSliders);
            take(out.boneRegions, h.numBoneRegions);
            out.raw.assign(data + 4, data + need);  // skip senderId
            return true;
        }

        // The vanilla NPC whose head looks most like `wanted`: same sex, same hair, same beard (or
        // lack of one), a generic settler rather than a named character.
        RE::TESNPC* PickFaceDonor(const std::vector<RE::BGSHeadPart*>& wanted, bool female)
        {
            uint32_t hair = 0, beard = 0;
            for (auto* hp : wanted) {
                if (hp->type == RE::BGSHeadPart::HeadPartType::kHair) hair = hp->GetFormID();
                if (hp->type == RE::BGSHeadPart::HeadPartType::kFacialHair) beard = hp->GetFormID();
            }
            const FaceDonor* best = nullptr;
            int bestScore = -1;
            for (auto& d : kFaceDonors) {
                if ((d.female != 0) != female) continue;
                const int score = (d.hair == hair ? 100 : 0) + (d.beard == beard ? 20 : 0) + (d.unique ? 0 : 5);
                if (score > bestScore && Lookup<RE::TESNPC>(d.id)) { bestScore = score; best = &d; }
            }
            return best ? Lookup<RE::TESNPC>(best->id) : nullptr;
        }

        // Copy the player template into a new NPC base record and dress it in `look`.
        RE::TESNPC* BuildBase(const Look& look)
        {
            auto* tmpl = Lookup<RE::TESNPC>(kPlayerTemplate);
            auto* factory = RE::ConcreteFormFactory<RE::TESNPC>::GetFormFactory();
            if (!tmpl || !factory) return nullptr;
            auto* npc = factory->Create();
            if (!npc) return nullptr;
            npc->Copy(tmpl);
            npc->SetFormID(g_nextBaseId++, false);

            const auto& h = look.header;
            // No default outfit: an empty body slot would otherwise be refilled with the vault suit.
            npc->defOutfit = nullptr;
            npc->sleepOutfit = nullptr;
            if (h.female) npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
            else npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
            if (auto* race = Lookup<RE::TESRace>(h.race)) npc->formRace = race;

            char name[sizeof(h.name) + 1]{};
            std::memcpy(name, h.name, sizeof(h.name));
            RE::TESFullName::SetFullName(*npc, name[0] ? name : "Player");

            // Fresh game-heap containers. The copy may still point at the player template's
            // containers; writing into those would change the local player's own face.
            auto* hrd = GameNew<RE::TESNPC::HeadRelatedData>();
            if (hrd) {
                hrd->hairColor = Lookup<RE::BGSColorForm>(h.hairColor);
                hrd->facialHairColor = Lookup<RE::BGSColorForm>(h.facialHairColor);
                hrd->faceDetails = Lookup<RE::BGSTextureSet>(h.faceDetails);
                npc->headRelatedData = hrd;
            }

            std::vector<RE::BGSHeadPart*> parts;
            // Meatcaps are the gore caps shown only after a dismemberment; listed as a normal part
            // they draw as a dark band around the neck.
            for (uint32_t id : look.headParts)
                if (auto* hp = Lookup<RE::BGSHeadPart>(id); hp && hp->type != RE::BGSHeadPart::HeadPartType::kMeatcaps)
                    parts.push_back(hp);
            auto** arr = static_cast<RE::BGSHeadPart**>(RE::malloc(std::max<size_t>(parts.size(), 1) * sizeof(void*)));
            if (arr) {
                std::copy(parts.begin(), parts.end(), arr);
                npc->headParts = arr;
                npc->numHeadParts = static_cast<int8_t>(parts.size());
            }
            if (npc->UsingAlternateHeadPartList()) {
                RE::BSTArray<RE::BGSHeadPart*> alt;
                for (auto* hp : parts) alt.push_back(hp);
                RE::TESNPC::GetAlternateHeadPartListMap().emplace(npc, std::move(alt));
            }

            if (auto* sliders = GameNew<RE::BSTHashMap<std::uint32_t, float>>()) {
                for (auto& s : look.morphSliders) sliders->emplace(s.key, s.value);
                npc->morphSliderValues = sliders;
            }
            if (auto* regions = GameNew<RE::BSTArray<float>>()) {
                for (float v : look.regionSliders) regions->push_back(v);
                npc->morphRegionSliderValues = regions;
            }
            if (auto* bones = GameNew<RE::BSTHashMap<std::uint32_t, RE::BGSCharacterMorph::Transform>>()) {
                for (auto& b : look.boneRegions) {
                    RE::BGSCharacterMorph::Transform t{};
                    t.position = { b.pos[0], b.pos[1], b.pos[2] };
                    t.rotation = { b.rot[0], b.rot[1], b.rot[2] };
                    t.scale = { b.scale[0], b.scale[1], b.scale[2] };
                    bones->emplace(b.key, t);
                }
                npc->facialBoneRegionSliderValues = bones;
            }

            npc->bodyTintColorR = h.bodyTint[0];
            npc->bodyTintColorG = h.bodyTint[1];
            npc->bodyTintColorB = h.bodyTint[2];
            npc->bodyTintColorA = h.bodyTint[3];
            npc->morphWeight = { h.morphWeight[0], h.morphWeight[1], h.morphWeight[2] };
            npc->height = h.height;
            // The copy carries the LOCAL player's skin tint layers (skin tone, stubble, scars) and
            // face template: on someone else's face, especially the other sex, that reads as "my
            // head on their body". Start from an empty tint list (race defaults) until tints sync.
            if (auto* tints = GameNew<RE::BGSCharacterTint::Entries>()) npc->tintingData = tints;
            // The head comes from the faceNPC's FaceGen mesh. The copy's faceNPC was the player
            // template, so every body wore the LOCAL player's head ("they're bald like me"). Only
            // the player's head is generated at runtime; a runtime NPC without a faceNPC has no head
            // (and crashes on a full 3D reset). So borrow the head of the vanilla NPC that looks
            // closest: same sex and hair style, same beard. (The face shape is the donor's.)
            if (auto* donor = PickFaceDonor(parts, h.female != 0)) {
                npc->faceNPC = donor;
                REX::INFO("[Appearance] head borrowed from {:#x} '{}' for '{}'", donor->GetFormID(),
                    donor->GetFullName() ? donor->GetFullName() : "", name);
            }

            REX::INFO("[Appearance] built body base {:#x} '{}' ({}, race {:#x}, {} head parts, {} sliders, {} regions, {} bones); lookup ok: {}",
                npc->GetFormID(), name, h.female ? "female" : "male", h.race, parts.size(), look.morphSliders.size(),
                look.regionSliders.size(), look.boneRegions.size(), RE::TESForm::GetFormByID(npc->GetFormID()) == npc);
            return npc;
        }
    }

    std::vector<uint8_t> CaptureLocal()
    {
        std::vector<uint8_t> out;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* npc = pl ? pl->GetNPC() : nullptr;
        if (!npc) return out;

        Header h{};
        h.version = kFormatVersion;
        h.race = IdOf(pl->race ? pl->race : npc->formRace);
        h.female = npc->IsFemale() ? 1 : 0;
        const auto parts = npc->GetHeadParts();
        h.numHeadParts = static_cast<uint8_t>(std::min<size_t>(parts.size(), 64));
        if (auto* hrd = npc->headRelatedData) {
            h.hairColor = IdOf(hrd->hairColor);
            h.facialHairColor = IdOf(hrd->facialHairColor);
            h.faceDetails = IdOf(hrd->faceDetails);
        }
        h.bodyTint[0] = npc->bodyTintColorR;
        h.bodyTint[1] = npc->bodyTintColorG;
        h.bodyTint[2] = npc->bodyTintColorB;
        h.bodyTint[3] = npc->bodyTintColorA;
        h.morphWeight[0] = npc->morphWeight.x;
        h.morphWeight[1] = npc->morphWeight.y;
        h.morphWeight[2] = npc->morphWeight.z;
        h.height = npc->height;
        if (const char* name = npc->GetFullName()) std::strncpy(h.name, name, sizeof(h.name) - 1);

        std::vector<MorphSlider> sliders;
        if (npc->morphSliderValues)
            for (auto& [k, v] : *npc->morphSliderValues) sliders.push_back({ k, v });
        std::vector<float> regions;
        if (npc->morphRegionSliderValues)
            for (float v : *npc->morphRegionSliderValues) regions.push_back(v);
        std::vector<BoneRegion> bones;
        if (npc->facialBoneRegionSliderValues)
            for (auto& [k, t] : *npc->facialBoneRegionSliderValues)
                bones.push_back({ k, { t.position.x, t.position.y, t.position.z }, { t.rotation.x, t.rotation.y, t.rotation.z },
                    { t.scale.x, t.scale.y, t.scale.z } });
        h.numMorphSliders = static_cast<uint16_t>(std::min<size_t>(sliders.size(), 256));
        h.numRegionSliders = static_cast<uint16_t>(std::min<size_t>(regions.size(), 256));
        h.numBoneRegions = static_cast<uint16_t>(std::min<size_t>(bones.size(), 128));

        Append(out, h);
        for (size_t i = 0; i < h.numHeadParts; ++i) Append(out, IdOf(parts[i]));
        for (size_t i = 0; i < h.numMorphSliders; ++i) Append(out, sliders[i]);
        for (size_t i = 0; i < h.numRegionSliders; ++i) Append(out, regions[i]);
        for (size_t i = 0; i < h.numBoneRegions; ++i) Append(out, bones[i]);
        return out;
    }

    namespace
    {
        // Co-op characters: one file per character in Documents\My Games\Fallout4\F4SE\F4MP_characters,
        // plus selected.txt naming the one to play as. The old single F4MP_mycharacter.bin is imported once.
        namespace fs = std::filesystem;

        fs::path CharactersDir()
        {
            const auto docs = F4MP::F4SEDocsDir();
            if (docs.empty()) return {};
            fs::path dir = fs::path(docs) / "F4MP_characters";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }

        std::vector<uint8_t> ReadFile(const fs::path& a_path)
        {
            std::vector<uint8_t> out;
            FILE* f = _wfopen(a_path.c_str(), L"rb");
            if (!f) return out;
            uint8_t buf[4096];
            for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) out.insert(out.end(), buf, buf + n);
            std::fclose(f);
            Look check;
            if (!Parse(out.data(), out.size(), check)) out.clear();   // stale/corrupt file: ignore
            return out;
        }

        bool WriteFile(const fs::path& a_path, const std::vector<uint8_t>& a_data)
        {
            FILE* f = _wfopen(a_path.c_str(), L"wb");
            if (!f) return false;
            const bool ok = std::fwrite(a_data.data(), 1, a_data.size(), f) == a_data.size();
            std::fclose(f);
            return ok;
        }

        std::string NameIn(const std::vector<uint8_t>& a_blob)
        {
            if (a_blob.size() < sizeof(Header)) return {};
            Header h{};
            std::memcpy(&h, a_blob.data(), sizeof(Header));
            return std::string(h.name, strnlen(h.name, sizeof(h.name)));
        }

        // File name for a character name: letters/digits/space/-/_ kept, the rest dropped.
        std::string FileFor(const std::string& a_name)
        {
            std::string out;
            for (char c : a_name)
                if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') out += c;
            if (out.empty()) out = "Character";
            return out + ".bin";
        }

        void WriteSelected(const fs::path& a_dir, const std::string& a_file)
        {
            if (FILE* f = _wfopen((a_dir / "selected.txt").c_str(), L"wb")) {
                std::fputs(a_file.c_str(), f);
                std::fclose(f);
            }
        }

        void ImportLegacy(const fs::path& a_dir)
        {
            const auto legacy = fs::path(F4MP::F4SEDocsDir()) / "F4MP_mycharacter.bin";
            std::error_code ec;
            if (!fs::exists(legacy, ec)) return;
            auto blob = ReadFile(legacy);
            if (!blob.empty()) {
                const auto file = FileFor(NameIn(blob));
                // Kept, not selected: the old single look was often saved while playing someone
                // else's save (the host's character), so selecting it made people look like the host.
                if (!fs::exists(a_dir / file, ec)) WriteFile(a_dir / file, blob);
            }
            fs::rename(legacy, fs::path(F4MP::F4SEDocsDir()) / "F4MP_mycharacter.bin.imported", ec);
            REX::INFO("[Appearance] imported the old co-op look as a character");
        }

        std::string SelectedFile(const fs::path& a_dir)
        {
            std::string name;
            if (FILE* f = _wfopen((a_dir / "selected.txt").c_str(), L"rb")) {
                char buf[256]{};
                if (std::fgets(buf, sizeof(buf), f)) name = buf;
                std::fclose(f);
                while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
            }
            return name;
        }
    }

    namespace
    {
        // One-time fix for the build that selected the imported look by itself: drop that selection
        // once, so everyone starts as the character they're playing.
        void UndoAutoSelection(const fs::path& a_dir)
        {
            std::error_code ec;
            const auto marker = a_dir / ".selection-v2";
            if (fs::exists(marker, ec)) return;
            if (fs::exists(fs::path(F4MP::F4SEDocsDir()) / "F4MP_mycharacter.bin.imported", ec)) WriteSelected(a_dir, {});
            if (FILE* f = _wfopen(marker.c_str(), L"wb")) std::fclose(f);
        }
    }

    std::vector<Character> ListCharacters()
    {
        std::vector<Character> out;
        const auto dir = CharactersDir();
        if (dir.empty()) return out;
        ImportLegacy(dir);
        UndoAutoSelection(dir);
        const auto selected = SelectedFile(dir);
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".bin") continue;
            auto blob = ReadFile(e.path());
            if (blob.empty()) continue;
            Header h{};
            std::memcpy(&h, blob.data(), sizeof(Header));
            Character c;
            c.file = e.path().filename().string();
            c.name = NameIn(blob);
            c.female = h.female != 0;
            c.selected = c.file == selected;
            out.push_back(std::move(c));
        }
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
        return out;
    }

    std::string SaveCurrentAsCharacter(const std::string& a_name)
    {
        auto blob = CaptureLocal();
        if (!a_name.empty() && blob.size() >= sizeof(Header)) {
            auto* h = reinterpret_cast<Header*>(blob.data());
            std::memset(h->name, 0, sizeof(h->name));
            std::memcpy(h->name, a_name.data(), (std::min)(a_name.size(), sizeof(h->name) - 1));
        }
        const auto dir = CharactersDir();
        if (blob.empty() || dir.empty()) {
            REX::WARN("[Appearance] character not saved: capture {} bytes, folder '{}'", blob.size(), dir.string());
            return {};
        }
        const auto file = FileFor(NameIn(blob));
        if (!WriteFile(dir / file, blob)) {
            REX::WARN("[Appearance] character not saved: cannot write {}", (dir / file).string());
            return {};
        }
        REX::INFO("[Appearance] saved character '{}' ({} bytes) as {}", NameIn(blob), blob.size(), file);
        return file;
    }

    bool SelectCharacter(const std::string& a_file)
    {
        const auto dir = CharactersDir();
        if (dir.empty()) return false;
        WriteSelected(dir, a_file);
        REX::INFO("[Appearance] playing as {}", a_file.empty() ? "the loaded character" : a_file);
        return true;
    }

    bool DeleteCharacter(const std::string& a_file)
    {
        const auto dir = CharactersDir();
        std::error_code ec;
        if (dir.empty() || a_file.empty() || a_file.find_first_of("\\/") != std::string::npos) return false;
        if (SelectedFile(dir) == a_file) SelectCharacter({});
        return fs::remove(dir / a_file, ec);
    }

    bool SaveMyLook()
    {
        const auto file = SaveCurrentAsCharacter();
        return !file.empty() && SelectCharacter(file);
    }

    std::vector<uint8_t> LookToSend()
    {
        const auto dir = CharactersDir();
        if (!dir.empty()) {
            ImportLegacy(dir);
            UndoAutoSelection(dir);
            const auto sel = SelectedFile(dir);
            if (!sel.empty()) {
                auto blob = ReadFile(dir / sel);
                if (!blob.empty()) return blob;
            }
        }
        return CaptureLocal();
    }

    std::string MyLookName()
    {
        const auto dir = CharactersDir();
        if (dir.empty()) return {};
        const auto sel = SelectedFile(dir);
        return sel.empty() ? std::string() : NameIn(ReadFile(dir / sel));
    }

    // Character names for the name tags (read on the render thread, hence the lock).
    namespace { std::mutex g_namesMutex; std::unordered_map<uint32_t, std::string> g_names; }

    std::string NameOf(uint32_t playerId)
    {
        std::lock_guard lk(g_namesMutex);
        auto it = g_names.find(playerId);
        return it != g_names.end() ? it->second : std::string();
    }

    bool OnReceived(uint32_t playerId, const uint8_t* data, size_t size)
    {
        if (size > 4) {
            std::vector<uint8_t> blob(data, data + size);
            std::lock_guard lk(g_namesMutex);
            g_names[playerId] = NameIn(blob);
        }
        Look look;
        if (!Parse(data, size, look)) {
            REX::WARN("[Appearance] bad appearance from player {} ({} bytes)", playerId, size);
            return false;
        }
        auto it = g_looks.find(playerId);
        if (it != g_looks.end() && it->second.raw == look.raw) return false;
        // Keep a copy of every look we meet (F4MP_characters\received\<name>.bin): lets a look
        // be reproduced offline, and later be picked as a character.
        if (const auto dir = CharactersDir(); !dir.empty()) {
            std::error_code ec;
            fs::create_directories(dir / "received", ec);
            std::vector<uint8_t> blob(data, data + size);
            WriteFile(dir / "received" / FileFor(NameIn(blob)), blob);
        }
        const bool hadOne = it != g_looks.end();
        g_looks[playerId] = std::move(look);
        g_bases.erase(playerId);           // rebuild the base with the new look
        if (hadOne) g_respawn[playerId] = true;
        REX::INFO("[Appearance] received appearance of player {} ({} bytes)", playerId, size);
        return true;
    }

    uint32_t GetBodyBase(uint32_t playerId)
    {
        auto look = g_looks.find(playerId);
        if (look == g_looks.end()) return 0;
        auto& base = g_bases[playerId];
        if (!base.built) {
            base.built = true;  // build at most once per received look, even if it fails
            if (auto* npc = BuildBase(look->second)) base.formId = npc->GetFormID();
        }
        return base.formId;
    }

    void RequestRespawn(uint32_t playerId) { g_respawn[playerId] = true; }

    bool TakeRespawnRequest(uint32_t playerId)
    {
        auto it = g_respawn.find(playerId);
        if (it == g_respawn.end() || !it->second) return false;
        it->second = false;
        return true;
    }

    void Forget(uint32_t playerId)
    {
        g_looks.erase(playerId);
        g_bases.erase(playerId);
        g_respawn.erase(playerId);
    }

    void Reset()
    {
        g_looks.clear();
        g_bases.clear();
        g_respawn.clear();
    }
}
