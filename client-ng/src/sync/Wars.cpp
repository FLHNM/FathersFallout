// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/Wars.h"
#include "sync/Range.h"
#include "sync/WarsSquads.h"
#include "sync/WarsDeath.h"
#include "sync/WarsVoice.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <unordered_set>

namespace F4MP::Wars
{
    namespace
    {
        constexpr uint32_t kProtocolOffset = 1000;   // = server's kWarsProtocolOffset
        // Written into the co-save of every Wars world; the launcher looks for it to sort saves into modes.
        constexpr char kMark[32] = "FATHERSFALLOUT-WASTELAND-WARS-1";

        bool g_mode = false;                 // set once at plugin load
        bool g_newWar = false;               // [Launch] NewWar=1, taken at the main menu
        std::atomic<bool> g_world{ false };  // the loaded save is a Wars world
        std::atomic<bool> g_tickRunning{ false };
        std::atomic<bool> g_newWarCancelled{ false };   // a save loaded while a new war waited for the mirror

        // Where a new war starts, picked at random so nobody lands on top of everyone else: settlements' map markers
        // with little living there (a moveto, not a coc: coc to exteriors can hang the game, testing; never Red Rocket,
        // any teleport into it hangs the loading screen, testing).
        struct Start { uint32_t marker; const char* name; };
        constexpr Start kStarts[] = {
            { 0x00072A48, "Sanctuary" }, { 0x0006F5D4, "Abernathy Farm" }, { 0x0009B1B1, "Tenpines Bluff" },
            { 0x0009B1CF, "Oberland Station" }, { 0x0009B1F0, "Greentop Nursery" }, { 0x00026469, "Finch Farm" },
            { 0x0009B1DA, "County Crossing" }, { 0x0002C26E, "Kingsport Lighthouse" }, { 0x0009B1E5, "Somerville Place" },
            { 0x0002BA8F, "The Slog" }, { 0x0002C6AB, "Egret Tours Marina" }, { 0x00024F95, "Starlight Drive In" },
        };
        constexpr uint32_t kMQ101 = 0x0001ED86, kMQ102 = 0x0001CC2A;

        // Survival: the game's own mode, on for every Wars world. Difficulty 6 is a setting (the game's
        // GetDifficultyLevel reads it), which the game's survival script watches through an event on the player: set
        // from here that event doesn't fire, so its startup is called the way the event would. It's guarded by the
        // script's own flag, so a world already in survival is left alone.
        constexpr int kSurvival = 6;
        constexpr uint32_t kHCManager = 0x0000080E;   // HC_Manager
        // The rules themselves are globals, all 0 in the game's data (the settings menu turns them on): with only the
        // difficulty set, hunger, thirst and the rest stayed off.
        constexpr uint32_t kHCRules[] = { 0x0000080D /* NoFastTravel */, 0x00000810 /* AdrenalineOn */, 0x00000812 /* SleepEffects */,
            0x00000846 /* NoLimbConditionHeal */, 0x00000848 /* DamageWhenEncumbered */, 0x0000084C /* ScaleDamage */,
            0x00000854 /* SustenanceEffects */, 0x0000088A /* DiseaseEffects */, 0x000008B1 /* CompanionNoHeal */ };
        bool g_survivalUp = false;                    // this world's script was started

        // What a new war opens with. OK has to be pressed before the game goes on (and before it connects).
        constexpr char kNote[] =
            "FROM THE PRESIDENT OF THE COMMONWEALTH\n\n"
            "This is it. You're on your own. Nobody is coming to save you.\n\n"
            "Eat, drink and sleep or you won't last the week. Sleeping in a bed saves your game.\n\n"
            "Clear a settlement, hold its workbench for 30 seconds and it's yours. "
            "Defend it: they will come back.\n\n"
            "Press F1 to start a crew or pick a side. "
            "If you die, your gear stays where you fell.\n\n"
            "Good luck out there.\n"
            "President FatherLamb";

        // Up to the player's OK (false = it never appeared).
        bool ShowNote()
        {
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            return vm && vm->DispatchStaticCall("Debug", "MessageBox", nullptr, RE::BSFixedString(kNote));
        }

        void KeepSurvival()
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* setting = RE::GetINISetting("iDifficulty:GamePlay");
            if (!pl || !setting) return;
            const int was = setting->GetInt();
            if (was != kSurvival) {
                setting->SetInt(kSurvival);
                REX::INFO("[Wars] survival on (difficulty was {}, the game now reads {})", was, static_cast<int>(pl->GetDifficultyLevel()));
            }
            for (uint32_t id : kHCRules)
                if (auto* rule = RE::TESForm::GetFormByID<RE::TESGlobal>(id); rule && rule->value != 1.0f) rule->value = 1.0f;
            if (g_survivalUp) return;
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            auto* quest = RE::TESForm::GetFormByID(kHCManager);
            if (!vm || !quest) return;
            const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<std::uint32_t>(quest->GetFormType()), quest);
            RE::BSTSmartPointer<RE::BSScript::Object> script;
            if (!vm->FindBoundObject(handle, "Hardcore:HC_ManagerScript", false, script, false) || !script) return;   // next tick
            g_survivalUp = true;
            const bool ok = vm->DispatchMethodCall(script, "StartupHardcore", [](RE::BSScrapArray<RE::BSScript::Variable>&) { return true; }, nullptr);
            REX::INFO("[Wars] survival script started -> {}", ok);
        }

        void Hud(std::string a_text)
        {
            REX::INFO("[Wars] notice: {}", a_text);
            if (auto* tasks = F4SE::GetTaskInterface())
                tasks->AddTask([t = std::move(a_text)]() { RE::SendHUDMessage::ShowHUDMessage(t.c_str(), nullptr, false, false); });
        }

        void Console(const std::string& a_cmd) { RE::Console::ExecuteCommand(a_cmd.c_str()); }

        bool InWorld()
        {
            auto* ui = RE::UI::GetSingleton();
            auto* pl = RE::PlayerCharacter::GetSingleton();
            return ui && pl && pl->parentCell && pl->Get3D() && !ui->GetMenuOpen("LoadingMenu") && !ui->GetMenuOpen("MainMenu");
        }

        // The Commonwealth without its story: every unique character (Preston, Piper, Codsworth, the traders,
        // faction leaders...) is disabled as its area loads. Generic faction members, raiders, gunners, creatures
        // and random encounters stay. Disabled refs stay disabled in the save. Runtime refs (players' bodies,
        // copies of others' NPCs, spawns) are never touched.
        void SweepUniques()
        {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* tes = RE::TES::GetSingleton();
            if (!pl || !tes) return;
            static std::unordered_set<uint32_t> s_done;
            static int s_logged = 0;
            int n = 0;
            tes->ForEachReferenceInRange(pl->GetPosition(), Range::Loaded(), [&](RE::TESObjectREFR* r) {
                auto* a = r ? r->As<RE::Actor>() : nullptr;
                if (!a || a == pl || a->GetFormID() >= 0xFF000000u || a->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
                auto* npc = a->GetNPC();
                if (!npc || !npc->IsUnique()) return RE::BSContainer::ForEachResult::kContinue;
                a->Disable();
                ++n;
                if (s_done.insert(a->GetFormID()).second && s_logged++ < 300)
                    REX::INFO("[Wars] story character {:#x} '{}' removed", a->GetFormID(), a->GetDisplayFullName() ? a->GetDisplayFullName() : "");
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (s_done.size() > 4096) s_done.clear();
            (void)n;
        }

        // A world started without the intro: the game's input layers can still hold movement, looking etc. off
        // (the intro releases them; we skip it). Every layer gets every control back. Wars worlds only.
        // Game.EnablePlayerControls(all) and Game.SetPlayerAIDriven(false) through Papyrus: what the intro calls when
        // it hands the player over (its own input layers aren't reachable from here).
        void PapyrusGameControls(bool a_log)
        {
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            if (!vm) return;
            const bool t = true;
            const bool a = vm->DispatchStaticCall("Game", "EnablePlayerControls", nullptr, t, t, t, t, t, t, t, t, t, t, t);
            const bool b = vm->DispatchStaticCall("Game", "SetPlayerAIDriven", nullptr, false);
            const bool c = vm->DispatchStaticCall("Game", "SetCharGenHUDMode", nullptr, 0);
            if (a_log) REX::INFO("[Wars] Game.EnablePlayerControls -> {}, SetPlayerAIDriven(false) -> {}, SetCharGenHUDMode(0) -> {}", a, b, c);
        }

        void UnlockControls(bool a_log)
        {
            PapyrusGameControls(a_log);
            // The skipped intro leaves its fade (FaderMenu) up, and with it the player's input blocked (testing: the
            // new-war character could open the Pip-Boy but not move or look). Fade in, close it.
            if (auto* game = RE::GameVM::GetSingleton(); game && game->GetVM()) {
                const bool f = game->GetVM()->DispatchStaticCall("Game", "FadeOutGame", nullptr, false, true, 0.0f, 0.1f, false);
                if (a_log) REX::INFO("[Wars] Game.FadeOutGame(fade in) -> {}", f);
            }
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->GetMenuOpen("FaderMenu"))
                if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                    q->AddMessage("FaderMenu", RE::UI_MESSAGE_TYPE::kHide);
                    if (a_log) REX::INFO("[Wars] FaderMenu was still open: closed");
                }
            if (auto* pc = RE::PlayerControls::GetSingleton()) {
                if (a_log) REX::INFO("[Wars] PlayerControls.blockPlayerInput was {}", pc->blockPlayerInput);
                pc->blockPlayerInput = false;
            }
            // Every input layer gets every control back, user events (move, look...) and the others (running,
            // sprinting...): the intro's walk-only layer belongs to its script and outlives the quest (testing: walking
            // speed, no sprint, after the skip). Through the game's own functions, for each layer id; the layer
            // flags can't be read reliably on this game version.
            auto* mgr = RE::BSInputEnableManager::GetSingleton();
            if (!mgr) return;
            const std::uint32_t n = std::min<std::uint32_t>(mgr->layers.size(), 256);
            int ok = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                ok += mgr->EnableUserEvent(i, RE::UserEvents::USER_EVENT_FLAG::kAll, true, RE::UserEvents::SENDER_ID::kGameplay) ? 1 : 0;
                mgr->EnableOtherEvent(i, RE::OtherInputEvents::OTHER_EVENT_FLAG::kAll, true, RE::UserEvents::SENDER_ID::kGameplay);
            }
            if (a_log) REX::INFO("[Wars] controls unlocked on {} input layer(s) ({} took the user events)", n, ok);
        }

        // An intro quest's input layer held in its script's property: deleted and the property cleared, as the
        // intro's own exit does (MQ101 stage 1000 does it too, but that stage also takes the player into the cryo
        // pod - testing).
        bool ReleaseScriptLayer(RE::BSScript::IVirtualMachine& a_vm, uint32_t a_quest, const char* a_script, const char* a_prop)
        {
            auto* q = RE::TESForm::GetFormByID(a_quest);
            if (!q) return false;
            const auto handle = a_vm.GetObjectHandlePolicy().GetHandleForObject(static_cast<std::uint32_t>(q->GetFormType()), q);
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            if (!a_vm.FindBoundObject(handle, a_script, false, obj, false) || !obj) return false;
            auto* v = obj->GetProperty(a_prop);
            if (!v || !v->is<RE::BSScript::Object>()) return false;
            if (auto layer = RE::BSScript::get<RE::BSScript::Object>(*v))
                a_vm.DispatchMethodCall(layer, "Delete", [](RE::BSScrapArray<RE::BSScript::Variable>&) { return true; }, nullptr);
            *v = nullptr;
            return true;
        }

        // What the intro's own scripts undo on the way out of Vault 111, done for a skipped intro (read from the
        // game's MQ101/MQ102 scripts): MQ101's start puts the game in chargen (SetInCharGen, chargen skeleton, chargen
        // HUD, time frozen at TimeScale 0, a sprint-off input layer); the mirror only gives a male character his
        // normal skeleton back, a female one keeps the chargen skeleton until a later intro animation. Also wakes a
        // world started by the old skip (setstage MQ102 0 = the cryo wake-up scene, its groggy camera and breathing).
        void ClearIntroState(bool a_log, unsigned a_parts = 0xFF)
        {
            auto* game = RE::GameVM::GetSingleton();
            auto vm = game ? game->GetVM() : nullptr;
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!vm || !pl) return;
            bool a = false, b = false, c = false, d = false, e = false;
            if (a_parts & 1) a = vm->DispatchStaticCall("Game", "SetInCharGen", nullptr, false, false, false);
            if (a_parts & 2) b = vm->DispatchStaticCall("Game", "SetCharGenHUDMode", nullptr, 0);
            if (a_parts & 4) c = vm->DispatchStaticCall("Game", "ForceDisableSSRGodraysDirLight", nullptr, false, false, false);
            if (a_parts & 8) d = vm->DispatchStaticCall("Game", "PrecacheCharGenClear", nullptr);
            if (a_parts & 16) {
                const auto handle = vm->GetObjectHandlePolicy().GetHandleForObject(static_cast<std::uint32_t>(pl->GetFormType()), pl);
                e = vm->DispatchMethodCall(handle, "Actor", "SetHasCharGenSkeleton", nullptr, false);
            }
            // Right after the mirror MQ101 (stage 10) gives the player the pre-war "fast walk" animations; its later
            // stages (400, 760) give the player ones back. Skipped, the character jogged at ~40% speed (testing: 155
            // u/s against 376 on a campaign save, speedMult 100).
            bool h = false;
            if (a_parts & 128) {
                constexpr uint32_t kAnimArchetypePlayer = 0x0006B508;
                auto* kw = RE::TESForm::GetFormByID<RE::BGSKeyword>(kAnimArchetypePlayer);
                const auto& handles = vm->GetObjectHandlePolicy();
                const auto handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::Actor>(), static_cast<const void*>(pl));
                RE::BSTSmartPointer<RE::BSScript::Object> object;
                if (kw && handle != handles.EmptyHandle() && vm->FindBoundObject(handle, "Actor", false, object, false) && object)
                    h = vm->DispatchMethodCall(object, "ChangeAnimArchetype", [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
                        a_out = RE::BSScript::detail::PackVariables(kw);
                        return true;
                    }, nullptr);
            }
            bool f = false, g = false;
            if (a_parts & 64) {
                f = ReleaseScriptLayer(*vm, kMQ101, "MQ101QuestScript", "MQ101EnableLayer");
                g = ReleaseScriptLayer(*vm, kMQ102, "MQ03QuestScript", "VSEnableLayer");
            }
            if (a_parts & 32) {
                Console("rimod CharGenCameraImod");
                Console("rimod CharGenMirrorImod");
                Console("rimod CryoWakeImod");
                Console("rimod CryoSuspensionImod");
            }
            constexpr uint32_t kTimeScale = 0x0000003A;
            if (auto* ts = RE::TESForm::GetFormByID<RE::TESGlobal>(kTimeScale); ts && ts->value < 1.0f) {
                ts->value = 20.0f;
                if (a_log) REX::INFO("[Wars] TimeScale was frozen: 20 again");
            }
            if (a_log) REX::INFO("[Wars] intro state cleared (parts {:#x}): SetInCharGen {} HUD {} godrays {} precache {} skeleton {} MQ101 layer {} MQ102 layer {} player anims {}", a_parts, a, b, c, d, e, f, g, h);
        }

        void Tick()
        {
            if (!Live() || !InWorld()) return;
            KeepSurvival();
            SweepUniques();
            Squads::Tick();
        }

        // Its own 1/s clock: Wars runs solo too, before or without a connection. Only in Wars mode.
        void StartTicking()
        {
            if (g_tickRunning.exchange(true)) return;
            std::thread([]() {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (Live())
                        if (auto* tasks = F4SE::GetTaskInterface()) tasks->AddTask([]() { Tick(); });
                }
            }).detach();
        }

        // The main menu's NEW: a real new game (the intro's quests set the player up properly; a coc from the main
        // menu left a character that took no input at all, testing).
        bool QueueNewGame()
        {
            auto* ui = RE::UI::GetSingleton();
            auto menu = ui ? ui->GetMenu<RE::MainMenu>() : nullptr;
            if (!menu) return false;
            // Closing the menu ourselves crashed the game: only the menu's own flags.
            menu->userEngaged = true;
            menu->mainMenuExitCondition = RE::MainMenu::MAIN_MENU_EXIT_CONDITION::kNewGame;
            menu->queueStartNewGame = true;
            return true;
        }

        // A fresh character's kit: Pip-Boy, vault suit, a 10mm with ammo, a few stimpaks, something to eat and drink.
        void GiveStartingKit()
        {
            Console("player.additem 00021B3B 1");   // Pip-Boy
            Console("player.equipitem 00021B3B");
            Console("player.additem 0001EED7 1");   // Vault 111 jumpsuit
            Console("player.equipitem 0001EED7");
            Console("player.additem 00004822 1");   // 10mm pistol
            Console("player.additem 0001F276 120");  // 10mm rounds
            Console("player.additem 00023736 5");    // stimpaks
            Console("player.additem 000366C0 3");    // purified water
            Console("player.additem 000330EE 3");    // cram
        }

        const Start& PickStart()
        {
            static std::mt19937 s_rng{ std::random_device{}() };
            return kStarts[std::uniform_int_distribution<size_t>(0, std::size(kStarts) - 1)(s_rng)];
        }
    }

    void GiveKit() { GiveStartingKit(); }
    uint32_t RandomStart() { return PickStart().marker; }

    void Init(const std::string& a_iniPath)
    {
        const char* ini = a_iniPath.c_str();
        char game[32]{};
        GetPrivateProfileStringA("Mode", "Game", "", game, sizeof(game), ini);
        g_mode = _stricmp(game, "wars") == 0;
        // One launch only: a later start without the launcher is the campaign again.
        WritePrivateProfileStringA("Mode", "Game", nullptr, ini);
        g_newWar = g_mode && GetPrivateProfileIntA("Launch", "NewWar", 0, ini) != 0;
        WritePrivateProfileStringA("Launch", "NewWar", nullptr, ini);
        if (g_mode) {
            REX::INFO("[Wars] WASTELAND WARS (experimental) mode{}", g_newWar ? ": starting a new war" : "");
            StartTicking();
        }
    }

    bool Mode() { return g_mode; }
    bool WorldIsWars() { return g_world.load(); }
    bool Live() { return g_mode && g_world.load(); }
    uint32_t ProtocolOffset() { return g_mode ? kProtocolOffset : 0; }
    const char* ConnectSection() { return g_mode ? "Wars" : "Connect"; }

    bool TakeNewWar()
    {
        const bool take = g_newWar;
        g_newWar = false;
        return take;
    }

    // A new war is a real new game:
    // the intro video (skippable) and the mirror's character creation as usual, then the rest of the intro is
    // skipped the way fast-start mods do it - MQ101 completed, MQ102's vault-exit stages set - and the player is
    // moved to Sanctuary with a starting kit. The main story never goes on from there.
    void StartNewWar(std::function<void()> a_ready)
    {
        std::thread([ready = std::move(a_ready)]() {
            using namespace std::chrono;
            auto onMain = [](auto&& a_fn) {
                std::atomic<bool> done{ false };
                F4SE::GetTaskInterface()->AddTask([&]() { a_fn(); done = true; });
                while (!done) std::this_thread::sleep_for(milliseconds(20));
            };
            auto menuOpen = [](const char* a_menu) {
                auto* ui = RE::UI::GetSingleton();
                return ui && ui->GetMenuOpen(a_menu);
            };
            // The player presses NEW.
            REX::INFO("[Wars] new war: waiting for NEW in the main menu and the character at the mirror");
            g_newWarCancelled = false;
            // Character creation at the mirror: wait for it to open, then to close (up to 30 minutes).
            bool sawLooks = false, created = false;
            const auto until = steady_clock::now() + minutes(30);
            while (steady_clock::now() < until) {
                bool looks = false, world = false;
                onMain([&]() { looks = menuOpen("LooksMenu"); world = InWorld(); });
                if (g_newWarCancelled) { REX::INFO("[Wars] new war: a save was loaded instead - cancelled"); return; }
                if (looks && !sawLooks) { sawLooks = true; REX::INFO("[Wars] new war: character creation open"); }
                if (sawLooks && !looks && world) { created = true; break; }
                std::this_thread::sleep_for(milliseconds(500));
            }
            if (!created) { REX::WARN("[Wars] new war: character creation never finished"); return; }
            std::this_thread::sleep_for(seconds(2));
            onMain([]() {
                REX::INFO("[Wars] new war: character made - skipping the rest of the intro");
                // Not MQ101 1000: its layer is released below
                // (ClearIntroState). MQ102 10 = out of the vault (its layers released, the respec trigger's too);
                // never MQ102 0, which starts the cryo wake-up scene (groggy camera, breathing) and freezes time.
                Console("completequest MQ101");
                Console("setstage MQ102 10");
            });
            std::this_thread::sleep_for(seconds(3));
            const Start& start = PickStart();
            onMain([&]() {
                REX::INFO("[Wars] new war: starting at {}", start.name);
                char cmd[64];
                std::snprintf(cmd, sizeof(cmd), "player.moveto %08X", start.marker);
                Console(cmd);
            });
            // The trip: a loading screen, then the world.
            std::this_thread::sleep_for(seconds(3));
            bool arrived = false;
            const auto tripUntil = steady_clock::now() + minutes(4);
            while (steady_clock::now() < tripUntil) {
                bool world = false;
                onMain([&]() { world = InWorld(); });
                if (world) { arrived = true; break; }
                std::this_thread::sleep_for(milliseconds(500));
            }
            if (!arrived) { REX::WARN("[Wars] new war: the trip to {} never ended", start.name); return; }
            std::this_thread::sleep_for(seconds(3));
            onMain([]() {
                Console("stopquest MQ102");   // no main story in a war
                ClearIntroState(true);
                UnlockControls(true);
                g_world = true;   // from here this world is a Wars world (the save below carries the mark)
                GiveStartingKit();
                SweepUniques();
            });
            std::this_thread::sleep_for(seconds(2));
            F4SE::GetTaskInterface()->AddTask([]() { Console("save WastelandWars"); });
            std::this_thread::sleep_for(seconds(2));
            // The President's note: the game (and the connection) waits for its OK.
            bool noteAsked = false, noteUp = false;
            onMain([&]() { noteAsked = ShowNote(); });
            const auto noteUntil = steady_clock::now() + minutes(30);
            const auto noteAppears = steady_clock::now() + seconds(6);
            while (noteAsked && steady_clock::now() < noteUntil) {
                bool open = false;
                onMain([&]() { open = menuOpen("MessageBoxMenu"); });
                if (open) noteUp = true;
                else if (noteUp || steady_clock::now() > noteAppears) break;
                std::this_thread::sleep_for(milliseconds(200));
            }
            REX::INFO("[Wars] new war: the President's note {}", noteUp ? "read" : "never appeared");
            Hud("WASTELAND WARS: the Commonwealth is yours to take. (Experimental)");
            if (ready) ready();
        }).detach();
    }

    bool OnGameLoaded()
    {
        g_newWarCancelled = true;   // a loaded save is never the new war's world
        g_survivalUp = false;
        Squads::Reset();
        // A Wars save made by an older skip: player animations and the intro's layers only (all parts at once
        // right at load crashed once in testing, Fallout4+0x610EA4; these two were fine on their own).
        if (Live()) { ClearIntroState(true, 0xC0); UnlockControls(true); }
        if (g_mode && !g_world) {
            Hud("WASTELAND WARS: this is a campaign save - nothing was changed and you are not connected. Start a NEW WAR in the launcher.");
            return false;
        }
        if (!g_mode && g_world) {
            Hud("FathersFallout: this is a Wasteland Wars save - play it from the launcher's WASTELAND WARS mode. Not connected.");
            return false;
        }
        return true;
    }

    void OnSave(const F4SE::SerializationInterface* a_intfc)
    {
        if (!g_world) return;   // campaign saves get no Wars record at all
        if (!a_intfc->OpenRecord(kRecord, 1)) return;
        a_intfc->WriteRecordData(kMark, sizeof(kMark));
        REX::INFO("[Wars] saved: Wars world mark");
    }

    void OnLoadRecord(const F4SE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length)
    {
        if (a_version != 1 || a_length < sizeof(kMark)) return;
        char mark[sizeof(kMark)]{};
        if (a_intfc->ReadRecordData(mark, sizeof(mark)) != sizeof(mark)) return;
        g_world = std::memcmp(mark, kMark, sizeof(kMark)) == 0;
        REX::INFO("[Wars] loaded a {} world", g_world ? "Wasteland Wars" : "(unknown mark)");
    }

    void OnRevert()
    {
        g_world = false;
        g_survivalUp = false;
        Squads::Reset();
    }

    void Bridge(const std::string& a_args)
    {
        std::string what = a_args;
        while (!what.empty() && what.front() == ' ') what.erase(what.begin());
        if (what == "diag") {
            auto* pc = RE::PlayerControls::GetSingleton();
            auto* pl = RE::PlayerCharacter::GetSingleton();
            REX::INFO("[Wars] blockPlayerInput {} | player AI-driven? process {} | menu mode {}", pc && pc->blockPlayerInput,
                pl && pl->currentProcess ? static_cast<int>(pl->currentProcess->processLevel) : -1, RE::UI::GetSingleton() ? RE::UI::GetSingleton()->menuMode : -1);
            if (auto* avs = RE::ActorValue::GetSingleton(); avs && pl) {
                auto av = [&](RE::ActorValueInfo* a_i) { return a_i ? pl->GetActorValue(*a_i) : -1.0f; };
                auto base = [&](RE::ActorValueInfo* a_i) { return a_i ? pl->GetBaseActorValue(*a_i) : -1.0f; };
                REX::INFO("[Wars] speedMult {} (base {}) | AP {} | carry {} weight {} | right leg {}", av(avs->speedMult), base(avs->speedMult),
                    av(avs->actionPoints), av(avs->carryWeight), pl->GetWeightInContainer(), av(avs->rightMobilityCondition));
            }
            if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->currentState)
                REX::INFO("[Wars] camera state {}", static_cast<int>(cam->currentState->id.get()));
            if (auto* ui = RE::UI::GetSingleton()) {
                std::string open;
                for (auto& m : ui->menuStack) if (m) { open += m->menuName.c_str(); open += ' '; }
                REX::INFO("[Wars] menus open: {}", open);
            }
            auto* mgr = RE::BSInputEnableManager::GetSingleton();
            if (!mgr) return;
            REX::INFO("[Wars] input: cached {:#x} forced {:#x}, {} layer(s)", static_cast<std::uint32_t>(mgr->cachedInputUserEventsFlags.underlying()),
                static_cast<std::uint32_t>(mgr->forceEnableInputUserEventsFlags.underlying()), mgr->layers.size());
            for (std::uint32_t i = 0; i < mgr->layers.size(); ++i)
                REX::INFO("[Wars]   layer {}: user {:#x} other {:#x}", i, static_cast<std::uint32_t>(mgr->layers[i].inputUserEvents.underlying()),
                    static_cast<std::uint32_t>(mgr->layers[i].otherInputEvents.underlying()));
        } else if (what == "newgame") {
            REX::INFO("[Wars] bridge: new game from the main menu -> {}", QueueNewGame());
        } else if (what == "squads") {
            Squads::Log();
        } else if (what.rfind("voice", 0) == 0) {
            Voice::Bridge(what.size() > 6 ? what.substr(6) : std::string());
        } else if (what == "die") {
            Death::Bridge("die");
        } else if (what == "bed") {
            // testing: into the nearest bed, as if the player had used it.
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* tes = RE::TES::GetSingleton();
            RE::TESObjectREFR* best = nullptr;
            float bestDist = 1e9f;
            if (pl && tes) tes->ForEachReferenceInRange(pl->GetPosition(), 6000.0f, [&](RE::TESObjectREFR* r) {
                const char* name = r ? r->GetDisplayFullName() : nullptr;
                auto* base = r ? r->GetObjectReference() : nullptr;
                if (name && base && base->As<RE::TESFurniture>() && (std::strstr(name, "Bed") || std::strstr(name, "Mattress") || std::strstr(name, "Sleeping"))) {
                    const float d = r->GetPosition().GetDistance(pl->GetPosition());
                    if (d < bestDist) { bestDist = d; best = r; }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            REX::INFO("[Wars] bridge: nearest bed {:#x} ({:.0f} away)", best ? best->GetFormID() : 0u, bestDist);
            if (best) {
                char cmd[64];
                std::snprintf(cmd, sizeof(cmd), "%08X.activate player", best->GetFormID());
                Console(cmd);
            }
        } else if (what == "note") {
            REX::INFO("[Wars] bridge: the President's note -> {}", ShowNote());
        } else if (what == "survival") {
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* setting = RE::GetINISetting("iDifficulty:GamePlay");
            auto* quest = RE::TESForm::GetFormByID<RE::TESQuest>(kHCManager);
            auto stage = [&](const char* a_av) {
                auto* av = RE::TESForm::GetFormByEditorID<RE::ActorValueInfo>(a_av);
                return av && pl ? pl->GetActorValue(*av) : -1.0f;
            };
            REX::INFO("[Wars] difficulty {} (setting {}), HC_Manager {}, survival script started {}; hunger {} thirst {} sleep {}",
                pl ? static_cast<int>(pl->GetDifficultyLevel()) : -1, setting ? setting->GetInt() : -1, quest ? quest->GetFormEditorID() : "missing",
                g_survivalUp, stage("HC_HungerEffect"), stage("HC_ThirstEffect"), stage("HC_SleepEffect"));
        } else if (what.rfind("jog", 0) == 0) {
            Squads::SetJog(what != "jog 0");
        } else if (what == "aiprobe") {
            Squads::AiProbe();

        } else if (what == "epc") {
            PapyrusGameControls(true);
        } else if (what == "unlock") {
            UnlockControls(true);
        } else if (what.rfind("intro", 0) == 0) {
            const auto parts = what.size() > 6 ? std::strtoul(what.c_str() + 6, nullptr, 16) : 0xFFu;
            ClearIntroState(true, static_cast<unsigned>(parts));
        } else {
            REX::INFO("[Wars] bridge: unknown '{}' (diag, unlock, intro, epc, newgame)", what);
        }
    }
}
