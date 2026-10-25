// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Based on F4MP by Jous99 (public domain). Licensed under the GNU
// GPL v3 with additional terms: see LICENSE and NOTICE.

#include "pch.h"
#include "F4SEMenuFramework.h"
#include "network/NetworkClient.h"
#include "network/SteamLink.h"
#include "sync/CoopSync.h"
#include "sync/FrameLimiter.h"
#include "sync/CrashLog.h"
#include "sync/BodyLights.h"
#include "sync/Appearance.h"
#include "sync/DynamicActors.h"
#include "sync/Range.h"
#include "sync/Authority.h"
#include "sync/Paths.h"
#include "sync/Workshop.h"
#include "sync/Activity.h"
#include "sync/GraphOverride.h"
#include "sync/AimHook.h"
#include "sync/PlayerEvents.h"
#include "sync/Explosions.h"

#include <thread>
#include <sstream>
#include <cstring>
#include <tlhelp32.h>
#include <chrono>
#include <deque>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// HITO 2 + 3: menu in-game (F4SE Menu Framework) + red real.
// The menu connects/disconnects and a network thread sends your position to the server.

namespace F4MP
{
    // Actor::weaponState is a 3-bit field of a signed enum: "want to holster" (4) and "holstering" (5) read
    // back as -4 / -3, so comparisons with those states never matched and GetWeaponMagicDrawn said "away"
    // mid-holster.
    static int WeaponStateOf(const RE::Actor* a) { return static_cast<int>(a->weaponState) & 7; }

    // An AI holstering against its player's / owner's state: the graph's Unequip was dropped
    // (GraphOverride), so the model is still in the hands; put the state back to drawn.
    static bool KeepDrawn(RE::Actor* a)
    {
        const int ws = WeaponStateOf(a);
        if (ws != 4 && ws != 5) return false;
        a->SetWeaponState(RE::WEAPON_STATE::kDrawn);
        return true;
    }

    static char g_serverAddr[64] = "127.0.0.1";
    static int  g_serverPort = 7779;
    static char g_playerName[64] = "Wastelander";
    static std::atomic<bool> g_netThreadStarted{ false };

    // HOST: broadcasts the state of nearby NPCs. Others receive it and drive their local
    // NPCs (by FormID) to the host's state.
    static bool g_isHost = false;

    // INVISIBLE dedicated host (test): our own position isn't sent, so nobody gets a body for us
    // (this game only serves the world and its NPCs). Our character still exists here to keep the
    // area loaded.
    static bool g_hideSelf = false;

    // The local player's own animation inputs, sampled on the main thread every sync tick. The
    // network thread sends these instead of a speed measured from position jumps (that runs out of
    // step with game frames and swings between 0 and ~2x the real speed). -1 = not available.
    static std::atomic<float> g_localGraphSpeed{ -1.0f };
    static std::atomic<float> g_localGraphDir{ -1.0f };   // fraction of a turn, 0 = forward
    static std::atomic<uint32_t> g_localMoveMode{ 0xFFFFFFFF };   // ActorState::moveMode, ~0 = unknown
    constexpr uint32_t kMoveModeSprinting = 0x100;

    // The local player as the main thread saw it on its last sync tick. The network thread used to read
    // the player directly (position, sneak, weapon...) while the game was updating it - and a send between
    // two frames repeated the last position, which read as speed 0.
    struct LocalSample {
        bool valid = false;
        uint64_t seq = 0;
        std::chrono::steady_clock::time_point at{};
        RE::NiPoint3 pos{};
        float heading = 0.0f, pitch = 0.0f;
        bool sneaking = false, jumping = false, weaponDrawn = false, lightOn = false;
        uint32_t lightColor = 0;
    };
    static std::mutex g_localLock;
    static LocalSample g_local;

    // Main-thread tasks the network thread schedules: at most one of each kind waiting. During a
    // loading screen the game runs no tasks, and thousands piled up and ran at once afterwards.
    struct TaskGate {
        std::atomic<bool> pending{ false };
        template <class F>
        void Queue(F&& fn)
        {
            if (pending.exchange(true)) return;
            F4SE::GetTaskInterface()->AddTask([this, f = std::forward<F>(fn)]() mutable {
                pending = false;
                f();
            });
        }
    };
    static TaskGate g_gateBodies, g_gateCoop, g_gateNpcSend, g_gateNpcSync, g_gateProbe;

    static void BodiesSync();       // defined further down
    static void MaybeTravelOnJoin();
    static void BroadcastNpcs();    // host: difundir NPCs cercanos
    static void NpcsSync();         // guest: drive local NPCs by FormID
    static bool TravelBusy();       // a guest's trip to the host isn't over (or a load just happened)

    // Reads the local player's appearance to send it to the server.
    static Network::PlayerAppearanceMsg CaptureAppearance(uint32_t myId)
    {
        Network::PlayerAppearanceMsg a{};
        a.playerId = myId;
        a.sex = -1;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return a;
        a.sex = static_cast<int32_t>(pl->GetSex());
        if (pl->race) a.raceFormID = pl->race->GetFormID();

        auto* npc = pl->GetNPC();
        if (npc) {
            a.skinR = static_cast<uint8_t>(npc->bodyTintColorR);
            a.skinG = static_cast<uint8_t>(npc->bodyTintColorG);
            a.skinB = static_cast<uint8_t>(npc->bodyTintColorB);
            a.skinA = static_cast<uint8_t>(npc->bodyTintColorA);
            if (npc->headRelatedData && npc->headRelatedData->hairColor)
                a.hairColor = npc->headRelatedData->hairColor->color;

            auto hp = npc->GetHeadParts();
            uint8_t n = static_cast<uint8_t>(std::min<size_t>(hp.size(), 16));
            a.numHeadParts = n;
            for (uint8_t i = 0; i < n; ++i)
                a.headParts[i] = hp[i] ? hp[i]->GetFormID() : 0;
        }
        return a;
    }

    // FathersFallout.ini next to this DLL. (A relative path would make the profile API look in C:\Windows.)
    static std::string IniPath()
    {
        char path[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&IniPath), &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        const std::string dir = std::string(path).substr(0, std::string(path).find_last_of("\\/") + 1);
        // FathersFallout.ini since Update 23; an F4MP.ini from older versions is renamed once.
        const std::string ini = dir + "FathersFallout.ini", legacy = dir + "F4MP.ini";
        static std::once_flag s_migrate;
        std::call_once(s_migrate, [&]() {
            if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
                MoveFileA(legacy.c_str(), ini.c_str());
        });
        return ini;
    }

    // ---- Connecting: on-screen feedback, optional local server launch, retries ----

    // Top-left game notification. Safe from any thread (runs on the main thread).
    static void Notify(std::string a_text)
    {
        REX::INFO("[FathersFallout] notice: {}", a_text);
        if (auto* tasks = F4SE::GetTaskInterface()) {
            tasks->AddTask([t = std::move(a_text)]() {
                RE::SendHUDMessage::ShowHUDMessage(t.c_str(), nullptr, false, false);
            });
        }
    }

    // ---- The pre-war intro: no co-op until the character has left Vault 111 ----
    // A save from the intro (the autosave after making the character, or quitting before the vault) loaded,
    // auto-connected and travelled to the host: the intro broke, the player was stuck in pre-war Sanctuary
    // and the connection timed out (player report, Update 17 over the internet).
    static std::atomic<bool> g_inIntro{ false };

    // Main thread. Pre-war world or pre-war vault, or the Vault 111 cryo cells before Out of Time started.
    static bool RefreshInIntro()
    {
        bool intro = false;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        if (cell) {
            const uint32_t c = cell->GetFormID();
            const uint32_t ws = cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
            constexpr uint32_t kSanctuaryHillsWorld = 0x000A7FF4, kPrewarVault111 = 0x000CA260, kPrewarVault111Ext = 0x000A8031;
            constexpr uint32_t kVault111Cryo = 0x000016D8, kVault111Start = 0x000016D9, kMQ102 = 0x0001CC2A;
            if (ws == kSanctuaryHillsWorld || c == kPrewarVault111 || c == kPrewarVault111Ext) intro = true;
            if (c == kVault111Cryo || c == kVault111Start) {
                auto* f = RE::TESForm::GetFormByID(kMQ102);
                auto* q = f ? f->As<RE::TESQuest>() : nullptr;
                intro = q && q->currentStage == 0;   // still frozen: Out of Time hasn't begun
            }
        }
        g_inIntro = intro;
        return intro;
    }

    static void NotifyIntro()
    {
        Notify("FathersFallout: finish the intro first - leave Vault 111 and save, then load that save to play co-op");
    }

    static std::string g_serverExe;            // FathersFallout.ini [Connect] ServerExe=
    static char g_password[32] = "";           // [Connect] Password= : host's join password / the one we join with
    static bool g_serverRestarted = false;     // host: restarted our own server once for a new password
    static std::atomic<int> g_retriesLeft{ 0 };  // automatic reconnect attempts left

    static bool IsLocalAddress(const char* a)
    {
        return std::strcmp(a, "127.0.0.1") == 0 || _stricmp(a, "localhost") == 0;
    }

    // Server=steam or steam:<friend code or name>: join a Steam friend instead of an IP.
    static bool IsSteamAddress(const char* a)
    {
        return _strnicmp(a, "steam", 5) == 0 && (a[5] == '\0' || a[5] == ':');
    }
    static bool g_steamHosting = true;   // [Connect] Steam=0 turns hosting through Steam off

    static bool IsProcessRunning(const wchar_t* a_exeName)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{ sizeof(pe) };
        bool found = false;
        for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
            found = _wcsicmp(pe.szExeFile, a_exeName) == 0;
        CloseHandle(snap);
        return found;
    }

    // Host convenience: start the server from ServerExe if it isn't running. Its first run only
    // writes config.json and exits, so a quick exit gets one more launch.
    static bool LaunchServer()
    {
        if (g_serverExe.empty() || IsProcessRunning(L"FathersFalloutServer.exe") || IsProcessRunning(L"F4MPServer.exe")) return false;
        if (GetFileAttributesA(g_serverExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Notify("FathersFallout: ServerExe not found: " + g_serverExe);
            return false;
        }
        const std::string dir = g_serverExe.substr(0, g_serverExe.find_last_of("\\/"));
        for (int run = 0; run < 2; ++run) {
            STARTUPINFOA si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            std::string cmd = "\"" + g_serverExe + "\"";
            // The password goes in the server's environment (not its command line, which other programs can read).
            SetEnvironmentVariableA("FF_SERVER_PASSWORD", g_password);
            const BOOL started = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                dir.c_str(), &si, &pi);
            SetEnvironmentVariableA("FF_SERVER_PASSWORD", nullptr);
            if (!started) {
                Notify("FathersFallout: could not start the server (error " + std::to_string(GetLastError()) + ")");
                return false;
            }
            CloseHandle(pi.hThread);
            const DWORD wait = WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hProcess);
            if (wait == WAIT_TIMEOUT) {
                REX::INFO("[FathersFallout] started server {}", g_serverExe);
                return true;
            }
        }
        Notify("FathersFallout: the server closed right after starting - run FathersFalloutServer.exe yourself to see why");
        return false;
    }

    // Host whose own server still runs with an older password (it outlives the game): close it so the
    // next connect starts it again with the password set now.
    static bool StopLocalServer()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{ sizeof(pe) };
        bool stopped = false;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, L"FathersFalloutServer.exe") != 0 && _wcsicmp(pe.szExeFile, L"F4MPServer.exe") != 0) continue;
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
                stopped = TerminateProcess(h, 0) != 0 || stopped;
                WaitForSingleObject(h, 3000);
                CloseHandle(h);
            }
        }
        CloseHandle(snap);
        return stopped;
    }

    // The host turned us away for a reason another try won't fix (end reasons from the server / Steam relay).
    static bool IsRefusal(const std::string& a_err)
    {
        for (const char* r : { "password", "Server full", "Protocol version", "FathersFallout version", "only lets Steam friends", "Too many connections" })
            if (a_err.find(r) != std::string::npos) return true;
        return false;
    }

    // Menu button and auto-connect both come here. Runs off the main thread (may wait on the server).
    static void ConnectNow(int a_retries)
    {
        if (g_inIntro) { NotifyIntro(); return; }
        std::thread([a_retries]() {
            auto& net = Network::NetworkClient::GetInstance();
            if (net.IsConnected() || net.IsConnecting()) return;
            if (g_isHost && IsLocalAddress(g_serverAddr) && LaunchServer()) {
                Notify("FathersFallout: started the co-op server");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            g_retriesLeft = a_retries;
            net.SetPlayerName(g_playerName);
            net.SetPassword(g_password);
            if (IsSteamAddress(g_serverAddr)) {
                // The host's Steam presence can take a moment to reach us: look again for ~1 min.
                for (int attempt = 0; attempt < 12; ++attempt) {
                    if (net.Connect(g_serverAddr, static_cast<uint16_t>(g_serverPort))) {
                        Notify("FathersFallout: connecting to " + net.GetTarget() + "...");
                        return;
                    }
                    if (attempt == 0) Notify("FathersFallout: " + net.GetLastError() + " - still looking...");
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    if (net.IsConnected() || net.IsConnecting()) return;
                }
                Notify("FathersFallout: " + net.GetLastError() + ". The host must be your Steam friend, in game and hosting "
                       "(or type their friend code in the launcher).");
                return;
            }
            if (net.Connect(g_serverAddr, static_cast<uint16_t>(g_serverPort)))
                Notify(std::string("FathersFallout: connecting to ") + g_serverAddr + "...");
            else
                Notify(std::string("FathersFallout: invalid server address ") + g_serverAddr);
        }).detach();
    }

    static std::string ConnectHint()
    {
        if (IsSteamAddress(g_serverAddr))
            return "Through Steam: the host must be in game with co-op hosting on. Both of you need Steam running.";
        if (IsLocalAddress(g_serverAddr))
            return "No server is running on this PC. Start FathersFalloutServer.exe (or set ServerExe= in FathersFallout.ini), then press Connect.";
        return "Check the host's IP, that the host's FathersFalloutServer.exe is running, and that UDP 7779 is forwarded to the host.";
    }

    // Called from the network thread: announce state changes, retry failed attempts.
    static void WatchConnection()
    {
        using namespace std::chrono;
        enum class S { Idle, Connecting, Connected };
        static S s_prev = S::Idle;
        static steady_clock::time_point s_retryAt{};

        auto& net = Network::NetworkClient::GetInstance();
        const S now = net.IsConnected() ? S::Connected : net.IsConnecting() ? S::Connecting : S::Idle;

        if (now != s_prev) {
            // The host left its own game: Steam friends must not keep landing in a server without a host
            // (and the F1 page said "Hosting through Steam" after Disconnect). Reconnecting hosts again.
            if (s_prev == S::Connected && now != S::Connected && Network::SteamLink::IsHosting())
                Network::SteamLink::StopHosting();
            if (now == S::Connected) {
                g_retriesLeft = 0;
                Notify("FathersFallout: connected to " + net.GetTarget() + " as " + g_playerName);
                // Host: friends can now join through Steam too (the server keeps taking IP joins).
                if (g_isHost && g_steamHosting && !net.IsSteam() && Network::SteamLink::Available() && !Network::SteamLink::IsHosting()) {
                    Network::SteamLink::StartHosting(net.Sockets(), static_cast<uint16_t>(g_serverPort), g_playerName);
                    if (Network::SteamLink::IsHosting()) {
                        Notify("FathersFallout: Steam friends can join you now (launcher: JOIN THROUGH STEAM)");
                        static bool s_selfTest = GetPrivateProfileIntA("Debug", "SteamSelfTest", 0, IniPath().c_str()) != 0;
                        if (s_selfTest) { s_selfTest = false; Network::SteamLink::SelfTest(); }
                    }
                }
            } else if (now == S::Idle && !net.GetLastError().empty()) {
                const std::string err = net.GetLastError();
                if ((s_prev == S::Connecting || net.LastCloseWasAttempt()) && IsRefusal(err)) {
                    g_retriesLeft = 0;
                    if (g_isHost && IsLocalAddress(g_serverAddr) && err.find("password") != std::string::npos &&
                        !g_serverRestarted && !g_serverExe.empty()) {
                        // Our own server from an earlier session, started with another password.
                        g_serverRestarted = true;
                        Notify("FathersFallout: restarting the co-op server with your new password...");
                        std::thread([]() { StopLocalServer(); std::this_thread::sleep_for(std::chrono::seconds(1)); ConnectNow(2); }).detach();
                    } else if (err.find("Wrong password") != std::string::npos) {
                        Notify("FathersFallout: wrong password for " + net.GetTarget() + " - ask the host for it and type it in the launcher (or F1 > Play together)");
                    } else if (err.find("uses a password") != std::string::npos) {
                        Notify("FathersFallout: " + err);
                    } else {
                        Notify("FathersFallout: " + net.GetTarget() + " turned us away: " + err);
                    }
                } else if (s_prev == S::Connecting || net.LastCloseWasAttempt()) {
                    if (g_retriesLeft > 0) {
                        Notify("FathersFallout: no answer from " + net.GetTarget() + ", retrying...");
                        s_retryAt = steady_clock::now() + seconds(3);
                    } else {
                        Notify("FathersFallout: could not connect to " + net.GetTarget() + ". " + ConnectHint());
                    }
                } else {
                    Notify("FathersFallout: lost connection (" + net.GetLastError() + ")");
                }
            }
            s_prev = now;
        }

        if (now == S::Idle && g_retriesLeft > 0 && s_retryAt != steady_clock::time_point{} &&
            steady_clock::now() >= s_retryAt) {
            s_retryAt = {};
            const int left = g_retriesLeft - 1;
            ConnectNow(left);
        }
    }

    // Test bridge (FathersFallout.ini [Debug] ConsoleBridge=1, off by default): console commands written to
    // Documents\My Games\Fallout4\F4SE\F4MP_console.txt run on the main thread, one per line, and
    // are echoed to FathersFallout.log. Lets test automation drive the game without window focus or keys.
    static bool g_consoleBridge = false;
    static bool g_bodyTrace = false;
    static std::chrono::steady_clock::time_point g_freezeUntil{};   // test: bodies not placed until then
    static std::atomic<bool> g_creatingCharacter{ false };   // "Create a new character" waits for LooksMenu to close
    static std::string g_newCharacterName;   // [Debug] BodyTrace=1: log remote body movement 4x/s

    static RE::Actor* FindNearbyActor(uint32_t baseFormId, const std::unordered_set<uint32_t>& exclude);
    static bool GoToHost(bool a_quiet);

    // Calls an Actor script function on a_actor. Its script object found by type (Preston's is a subclass of
    // Actor), or made and bound when it has none, the way CommonLib packs a form argument.
    static bool PapyrusOnActor(RE::Actor* a_actor, const char* a_func, auto... a_args)
    {
        auto* game = RE::GameVM::GetSingleton();
        auto vm = game ? game->GetVM() : nullptr;
        if (!vm || !a_actor) return false;
        const auto& handles = vm->GetObjectHandlePolicy();
        const auto handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::Actor>(), static_cast<const void*>(a_actor));
        if (handle == handles.EmptyHandle()) return false;
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        if (!vm->FindBoundObject(handle, "Actor", false, object, false) && vm->CreateObject("Actor", object) && object)
            vm->GetObjectBindPolicy().BindObject(object, handle);
        if (!object) return false;
        return vm->DispatchMethodCall(object, a_func, [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
            a_out = RE::BSScript::detail::PackVariables(a_args...);
            return true;
        }, nullptr);
    }

    // The same for an ObjectReference function on a reference.
    static bool PapyrusOnRef(RE::TESObjectREFR* a_ref, const char* a_func, auto... a_args)
    {
        auto* game = RE::GameVM::GetSingleton();
        auto vm = game ? game->GetVM() : nullptr;
        if (!vm || !a_ref) return false;
        const auto& handles = vm->GetObjectHandlePolicy();
        const auto handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<RE::TESObjectREFR>(), static_cast<const void*>(a_ref));
        if (handle == handles.EmptyHandle()) return false;
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        if (!vm->FindBoundObject(handle, "ObjectReference", false, object, false) && vm->CreateObject("ObjectReference", object) && object)
            vm->GetObjectBindPolicy().BindObject(object, handle);
        if (!object) return false;
        return vm->DispatchMethodCall(object, a_func, [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
            a_out = RE::BSScript::detail::PackVariables(a_args...);
            return true;
        }, nullptr);
    }

    // Invisible markers (XMarker) the walked NPCs path to, one each, moved to where the owner's copy stands.
    // placeatme's marker appears a frame later: one requested at a time, the newcomer near us taken.
    static std::vector<uint32_t> g_markerFree;
    static std::unordered_map<uint32_t, uint32_t> g_npcMarker;   // local NPC -> its marker
    static std::unordered_set<uint32_t> g_markersKnown;          // ours, and markers that were there before a request
    static std::chrono::steady_clock::time_point g_markerAskedAt{};

    static RE::TESObjectREFR* MarkerFor(uint32_t a_npc)
    {
        auto lookup = [](uint32_t id) { auto* f = id ? RE::TESForm::GetFormByID(id) : nullptr; return f ? f->As<RE::TESObjectREFR>() : nullptr; };
        if (auto it = g_npcMarker.find(a_npc); it != g_npcMarker.end()) {
            if (auto* m = lookup(it->second); m && !m->IsDeleted()) return m;
            g_npcMarker.erase(it);
        }
        while (!g_markerFree.empty()) {
            const uint32_t id = g_markerFree.back();
            g_markerFree.pop_back();
            if (auto* m = lookup(id); m && !m->IsDeleted()) { g_npcMarker[a_npc] = id; return m; }
        }
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* tes = RE::TES::GetSingleton();
        if (!pl || !tes || g_npcMarker.size() >= 48) return nullptr;
        const auto now = std::chrono::steady_clock::now();
        auto scan = [&](bool a_take) {
            RE::TESObjectREFR* found = nullptr;
            tes->ForEachReferenceInRange(pl->GetPosition(), 400.0f, [&](RE::TESObjectREFR* r) {
                auto* base = r ? r->GetObjectReference() : nullptr;
                if (base && base->GetFormID() == 0x3B && r->GetFormID() >= 0xFF000000u && !g_markersKnown.contains(r->GetFormID())) {
                    if (a_take && !found) found = r;
                    else if (!a_take) g_markersKnown.insert(r->GetFormID());
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            return found;
        };
        if (g_markerAskedAt != std::chrono::steady_clock::time_point{} && now - g_markerAskedAt < std::chrono::seconds(2)) {
            if (auto* m = scan(true)) {
                g_markersKnown.insert(m->GetFormID());
                g_npcMarker[a_npc] = m->GetFormID();
                g_markerAskedAt = {};
                return m;
            }
            return nullptr;   // still on its way
        }
        scan(false);   // whatever markers are around now (the explosions' one) are not ours
        // Forced persistent (ObjectReference.PlaceAtMe): a console placeatme marker is a temporary reference, and
        // handing one to a script function crashed the game.
        auto* xmarker = RE::TESForm::GetFormByID(0x3B);
        if (!xmarker || !PapyrusOnRef(pl, "PlaceAtMe", xmarker, 1, true, false, false)) return nullptr;
        g_markerAskedAt = now;
        return nullptr;
    }

    static void FreeMarker(uint32_t a_npc)
    {
        if (auto it = g_npcMarker.find(a_npc); it != g_npcMarker.end()) { g_markerFree.push_back(it->second); g_npcMarker.erase(it); }
    }

    static float g_npcSpeedMult = 1.0f;   // test aid (bridge f4mp:npcspeed)
    static std::uint32_t g_mirrorOff = 0;  // test aid (bridge f4mp:mirroroff <hex>): 1 vitals, 2 weapon, 4 idle filter, 8 sneak
    static bool g_npcAiMove = false;       // test: mirrored NPCs walked by their own AI (see PapyrusOnActor)

    static void PollConsoleBridge()
    {
        if (!g_consoleBridge) return;
        static const std::string file = F4SEDocsDir() + "\\F4MP_console.txt";
        FILE* f = std::fopen(file.c_str(), "rb");
        if (!f) return;
        std::string text;
        char buf[512];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        std::fclose(f);
        std::remove(file.c_str());
        std::vector<std::string> lines;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find_first_of("\r\n", start);
            if (end == std::string::npos) end = text.size();
            if (end > start) lines.emplace_back(text.substr(start, end - start));
            start = end + 1;
        }
        if (lines.empty()) return;
        F4SE::GetTaskInterface()->AddTask([lines]() {
            for (auto& cmd : lines) {
                REX::INFO("[Bridge] > {}", cmd);
                // Test helper: "f4mp:freezebody <seconds>" lets the animation move bodies alone.
                if (cmd.rfind("f4mp:freezebody ", 0) == 0) {
                    g_freezeUntil = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(static_cast<int>(std::atof(cmd.c_str() + 16) * 1000));
                    continue;
                }
                // Test helper: "f4mp:companion <ref hex>" logs whether that NPC counts as our companion.
                if (cmd.rfind("f4mp:companion ", 0) == 0) {
                    const uint32_t fid = std::strtoul(cmd.c_str() + 15, nullptr, 16);
                    auto* f = RE::TESForm::GetFormByID(fid);
                    auto* a = f ? f->As<RE::Actor>() : nullptr;
                    auto* cur = RE::TESForm::GetFormByID(0x00023C01);
                    REX::INFO("[Bridge] {:#x}: actor {} in CurrentCompanionFaction {} our companion {} 3D {}", fid, a != nullptr,
                        a && cur && a->IsInFaction(cur->As<RE::TESFaction>()), a && DynamicActors::IsOurCompanion(a), a && a->Get3D());
                    continue;
                }
                // Test helper: "f4mp:spine <mode> <scale>" for the bodies' bent-spine aim (AimHook).
                if (cmd.rfind("f4mp:spine ", 0) == 0) {
                    int mode = 1;
                    float scale = 1.0f;
                    std::sscanf(cmd.c_str() + 11, "%d %f", &mode, &scale);
                    AimHook::SetSpine(mode, scale);
                    REX::INFO("[Bridge] body spine mode {} scale {:.2f}", mode, scale);
                    continue;
                }
                // Test helper: "f4mp:actor <hex>" logs how an actor is drawn (an NPC you can target but not see).
                if (cmd.rfind("f4mp:actor ", 0) == 0) {
                    auto* f = RE::TESForm::GetFormByID(std::strtoul(cmd.c_str() + 11, nullptr, 16));
                    auto* a = f ? f->As<RE::Actor>() : nullptr;
                    if (!a) { REX::INFO("[Bridge] no such actor"); continue; }
                    auto* root = a->Get3D();
                    auto* fade = root ? root->IsFadeNode() : nullptr;
                    auto* cell = a->GetParentCell();
                    const auto p = a->GetPosition();
                    int children = 0, culled = 0;
                    if (auto* node = root ? root->IsNode() : nullptr)
                        for (auto& c : node->children) { ++children; if (c && c->GetAppCulled()) ++culled; }
                    REX::INFO("[Bridge] actor {:#x}: 3D {} culled {} fade {:.2f} children {} ({} culled) disabled {} dead {} cell {:#x} attached {} pos {:.0f} {:.0f} {:.0f}",
                        a->GetFormID(), root != nullptr, root && root->GetAppCulled(), fade ? fade->currentFade : -1.0f, children, culled,
                        a->IsDisabled(), a->IsDead(false), cell ? cell->GetFormID() : 0u, cell && cell->IsAttached(), p.x, p.y, p.z);
                    std::string eq;
                    if (auto* mh = a->currentProcess ? a->currentProcess->middleHigh : nullptr) {
                        RE::BSAutoLock l{ mh->equippedItemsLock };
                        for (auto& e : mh->equippedItems)
                            if (e.item.object) {
                                auto* w = e.item.object->As<RE::TESObjectWEAP>();
                                auto* ammo = w ? (e.item.instanceData ? static_cast<RE::TESObjectWEAP::InstanceData*>(e.item.instanceData.get())->ammo : w->weaponData.ammo) : nullptr;
                                eq += std::format(" {:08X}{}", e.item.object->GetFormID(),
                                    ammo ? std::format("(ammo {:08X} x{})", ammo->GetFormID(), a->GetInventoryObjectCount(ammo)) : "");
                            }
                    }
                    REX::INFO("[Bridge]   drawn {} state {} equipped:{}", a->GetWeaponMagicDrawn(), static_cast<int>(a->weaponState) & 7, eq);
                    continue;
                }
                // Test helper: "f4mp:state <hex>" logs an actor's health and fight (Preston turning on the host, U24).
                if (cmd.rfind("f4mp:state ", 0) == 0) {
                    auto* f = RE::TESForm::GetFormByID(std::strtoul(cmd.c_str() + 11, nullptr, 16));
                    auto* a = f ? f->As<RE::Actor>() : nullptr;
                    auto* pl = RE::PlayerCharacter::GetSingleton();
                    auto* avs = RE::ActorValue::GetSingleton();
                    if (!a || !pl || !avs || !avs->health) { REX::INFO("[Bridge] no such actor"); continue; }
                    auto t = a->currentCombatTarget.get();
                    REX::INFO("[Bridge] state {:#x}: health {:.1f} combat {} target {:#x} hostile-to-player {} dist {:.0f} process {}", a->GetFormID(),
                        a->GetActorValue(*avs->health), a->IsInCombat(), t ? t->GetFormID() : 0u, a->GetHostileToActor(pl),
                        a->GetPosition().GetDistance(pl->GetPosition()), a->currentProcess ? static_cast<int>(a->currentProcess->processLevel) : -1);
                    continue;
                }
                // Test helper: "f4mp:steps <hex>" logs how long ago that body/mirrored NPC's graph sent a footstep.
                if (cmd.rfind("f4mp:steps ", 0) == 0) {
                    auto* f = RE::TESForm::GetFormByID(std::strtoul(cmd.c_str() + 11, nullptr, 16));
                    auto* a = f ? f->As<RE::Actor>() : nullptr;
                    const long long step = a ? GraphOverride::LastStepMs(a) : 0;
                    REX::INFO("[Bridge] steps {:#x}: last footstep {} ms ago", f ? f->GetFormID() : 0u, step ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - step : -1LL);
                    continue;
                }
                // Test helper: "f4mp:npcspeed <mult>" scales the walk speed given to mirrored NPCs' graphs.
                if (cmd.rfind("f4mp:npcspeed ", 0) == 0) {
                    g_npcSpeedMult = static_cast<float>(std::atof(cmd.c_str() + 14));
                    REX::INFO("[Bridge] mirrored NPC graph speed x{:.2f}", g_npcSpeedMult);
                    continue;
                }
                // Test helper: "f4mp:animstep <hex>" logs the animation time step that actor gets (0 = off).
                if (cmd.rfind("f4mp:animstep ", 0) == 0) {
                    AimHook::ProbeDelta(static_cast<std::uint32_t>(std::strtoul(cmd.c_str() + 14, nullptr, 16)));
                    continue;
                }
                // Test helper: "f4mp:npcai 0|1": mirrored NPCs placed every frame (0) or walked by their own AI (1).
                if (cmd.rfind("f4mp:npcai ", 0) == 0) {
                    g_npcAiMove = cmd[11] == '1';
                    REX::INFO("[Bridge] mirrored NPCs {}", g_npcAiMove ? "walked by their own AI" : "placed every frame");
                    continue;
                }
                // Test helper: "f4mp:papyrus <hex> <Actor function>" calls a no-argument Actor script function.
                if (cmd.rfind("f4mp:papyrus ", 0) == 0) {
                    char* end = nullptr;
                    auto* f = RE::TESForm::GetFormByID(std::strtoul(cmd.c_str() + 13, &end, 16));
                    std::string fn = end && *end ? end + 1 : "";
                    auto* a = f ? f->As<RE::Actor>() : nullptr;
                    REX::INFO("[Bridge] papyrus {:#x}.{}() -> {}", f ? f->GetFormID() : 0u, fn, a && PapyrusOnActor(a, fn.c_str()));
                    continue;
                }
                // Test helper: "f4mp:papyrusp <hex>": IsHostileToActor(player), then KeepOffsetFromActor(player, 0 200 0).
                if (cmd.rfind("f4mp:papyrusp ", 0) == 0) {
                    auto* f = RE::TESForm::GetFormByID(std::strtoul(cmd.c_str() + 14, nullptr, 16));
                    auto* a = f ? f->As<RE::Actor>() : nullptr;
                    RE::Actor* pl = RE::PlayerCharacter::GetSingleton();
                    const bool h = a && PapyrusOnActor(a, "IsHostileToActor", pl);
                    const bool k = a && PapyrusOnActor(a, "PathToReference", static_cast<RE::TESObjectREFR*>(pl), 0.0f);
                    REX::INFO("[Bridge] papyrus with player: IsHostileToActor {} PathToReference(player, walk) {}", h, k);
                    continue;
                }
                if (cmd.rfind("f4mp:events ", 0) == 0) {
                    GraphOverride::ProbeEvents(static_cast<std::uint32_t>(std::strtoul(cmd.c_str() + 12, nullptr, 16)));
                    continue;
                }
                if (cmd.rfind("f4mp:mirroroff ", 0) == 0) {
                    g_mirrorOff = static_cast<std::uint32_t>(std::strtoul(cmd.c_str() + 15, nullptr, 16));
                    REX::INFO("[Bridge] mirror features off: {:#x}", g_mirrorOff);
                    continue;
                }
                // Test helper: "f4mp:synctime" = the menu's "Sync time with host".
                if (cmd == "f4mp:synctime") {
                    REX::INFO("[Bridge] sync time with host -> {}", Coop::SyncTimeWithHost());
                    continue;
                }
                // Test helper: "f4mp:clock" logs the calendar (the hour and the days-passed counters).
                if (cmd == "f4mp:clock") {
                    auto* cal = RE::Calendar::GetSingleton();
                    if (cal && cal->gameHour && cal->gameDaysPassed)
                        REX::INFO("[Bridge] clock hour {:.3f} daysPassed {:.4f} raw {:.4f} day {} month {}", cal->gameHour->value,
                            cal->gameDaysPassed->value, cal->rawDaysPassed, cal->gameDay ? cal->gameDay->value : -1.0f,
                            cal->gameMonth ? cal->gameMonth->value : -1.0f);
                    continue;
                }
                // Test helper: "f4mp:crashtest" crashes the game on purpose, to check what the crash logger writes.
                if (cmd == "f4mp:crashtest") {
                    REX::WARN("[Bridge] crashing on purpose (crash logger test)");
                    *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;
                    continue;
                }
                // Test helper: "f4mp:gvar <hex> <name>..." logs an actor's graph variables (float, int or bool).
                if (cmd.rfind("f4mp:gvar ", 0) == 0) {
                    std::istringstream in(cmd.substr(10));
                    std::string id, name;
                    in >> id;
                    auto* f = RE::TESForm::GetFormByID(std::strtoul(id.c_str(), nullptr, 16));
                    auto* a = f ? f->As<RE::Actor>() : nullptr;
                    if (!a) { REX::INFO("[Bridge] no such actor"); continue; }
                    std::string out;
                    while (in >> name) {
                        float fv = 0.0f; int iv = 0; bool bv = false;
                        if (a->GetGraphVariableImplFloat(name, fv)) out += std::format(" {}={:.2f}", name, fv);
                        else if (a->GetGraphVariableImplInt(name, iv)) out += std::format(" {}={}", name, iv);
                        else if (a->GetGraphVariableImplBool(name, bv)) out += std::format(" {}={}", name, bv);
                        else out += " " + name + "=?";
                    }
                    REX::INFO("[Bridge] actor {:#x} graph:{}", a->GetFormID(), out);
                    continue;
                }
                // Test helper: "f4mp:pitchscale <graph units per radian>" for the bodies' aim.
                if (cmd.rfind("f4mp:pitchscale ", 0) == 0) {
                    const float v = static_cast<float>(std::atof(cmd.c_str() + 16));
                    GraphOverride::SetPitchScale(v);
                    REX::INFO("[Bridge] body pitch scale {:.3f}", v);
                    continue;
                }
                // Test helper: "f4mp:flst <hex>..." logs form lists as this save has them.
                if (cmd.rfind("f4mp:flst ", 0) == 0) {
                    std::istringstream in(cmd.substr(10));
                    std::string h;
                    while (in >> h) {
                        auto* fl = RE::TESForm::GetFormByID(std::stoul(h, nullptr, 16));
                        auto* list = fl ? fl->As<RE::BGSListForm>() : nullptr;
                        if (!list) { REX::INFO("[FLST] {} not a form list", h); continue; }
                        std::string line;
                        for (auto* f : list->arrayOfForms) line += std::format(" {:08X}", f ? f->GetFormID() : 0u);
                        std::string added;
                        if (list->scriptAddedTempForms)
                            for (auto id : *list->scriptAddedTempForms) added += std::format(" {:08X}", id);
                        REX::INFO("[FLST] {:08X}: {} forms [{} ] script-added [{} ]", list->GetFormID(), list->arrayOfForms.size(), line, added);
                    }
                    continue;
                }
                // Test helpers for sync/Activity: "f4mp:act <what> <actor> ..." where actor is a FormID in hex,
                // "body" (the first player body) or "player". info | seat <furn> [marker] | unseat [i m u] |
                // idle <idle> | pa [frameBase] [pieces...] | paout; "f4mp:act near" describes actors around us.
                if (cmd.rfind("f4mp:act ", 0) == 0) {
                    std::istringstream in(cmd.substr(9));
                    std::string what, who;
                    in >> what;
                    auto* pl = RE::PlayerCharacter::GetSingleton();
                    if (what == "near") {
                        if (auto* tes = RE::TES::GetSingleton(); tes && pl)
                            tes->ForEachReferenceInRange(pl->GetPosition(), 2500.0f, [&](RE::TESObjectREFR* ref) {
                                if (auto* a = ref ? ref->As<RE::Actor>() : nullptr; a && a != pl && a->Get3D()) Activity::Describe(a);
                                return RE::BSContainer::ForEachResult::kContinue;
                            });
                        continue;
                    }
                    in >> who;
                    RE::Actor* a = nullptr;
                    if (who == "player") a = pl;
                    else if (who == "body") { auto b = GetBodies(); if (!b.empty()) if (auto* f = RE::TESForm::GetFormByID(b.front().second)) a = f->As<RE::Actor>(); }
                    else if (auto* f = RE::TESForm::GetFormByID(std::strtoul(who.c_str(), nullptr, 16))) a = f->As<RE::Actor>();
                    if (!a) { REX::INFO("[Bridge] act: no actor '{}'", who); continue; }
                    std::vector<uint32_t> args;
                    std::string h;
                    while (in >> h) args.push_back(std::strtoul(h.c_str(), nullptr, 16));
                    if (what == "seat" && !args.empty()) {
                        auto* furn = RE::TESForm::GetFormByID(args[0]);
                        auto* ref = furn ? furn->As<RE::TESObjectREFR>() : nullptr;
                        const bool ok = Activity::Seat(a, ref, args.size() > 1 ? args[1] : 0);
                        REX::INFO("[Bridge] act seat {:#x} on {:#x} -> {}", a->GetFormID(), args[0], ok);
                    } else if (what == "unseat") {
                        if (args.size() >= 3) a->StopInteractingQuick(args[0] != 0, args[1] != 0, args[2] != 0);
                        else Activity::Unseat(a);
                        REX::INFO("[Bridge] act unseat {:#x}", a->GetFormID());
                    } else if (what == "idle" && !args.empty()) {
                        auto* f = RE::TESForm::GetFormByID(args[0]);
                        const bool ok = Activity::PlayIdle(a, f ? f->As<RE::TESIdleForm>() : nullptr);
                        REX::INFO("[Bridge] act idle {:#x} {:#x} -> {}", a->GetFormID(), args[0], ok);
                    } else if (what == "pa") {
                        const uint32_t base = args.empty() ? 0 : args[0];
                        Activity::EnterPowerArmor(a, base, args.size() > 1 ? args.data() + 1 : nullptr, args.size() > 1 ? static_cast<int>(args.size() - 1) : 0);
                    } else if (what == "paout") {
                        Activity::ExitPowerArmor(a);
                    } else if (what == "seq") {
                        // seq <steps...> in one go: name=0|1 sets a bool, name:=<float> a float, name#=<int> an int,
                        // anything else is sent as an event; each result is logged.
                        std::istringstream in2(cmd.substr(9));
                        std::string skip, t;
                        in2 >> skip >> skip;
                        while (in2 >> t) {
                            bool ok = false;
                            if (auto p = t.find(":="); p != std::string::npos) ok = a->SetGraphVariableFloat(t.substr(0, p).c_str(), std::stof(t.substr(p + 2)));
                            else if (auto q = t.find("#="); q != std::string::npos) ok = a->SetGraphVariableInt(t.substr(0, q).c_str(), std::stoi(t.substr(q + 2)));
                            else if (auto e = t.find('='); e != std::string::npos) ok = a->SetGraphVariableBool(t.substr(0, e).c_str(), t.substr(e + 1) == "1");
                            else ok = GraphOverride::SendEvent(a, t.c_str());
                            REX::INFO("[Bridge] act seq {:#x} {} -> {}", a->GetFormID(), t, ok);
                        }
                        continue;
                    } else if (what == "ev" || what == "var" || what == "setb") {
                        // ev <events...>: send graph events (as F4MP) and log what the graph said; var <names...>: read
                        // graph variables (float/int/bool); setb <name> <0|1>: set a bool variable.
                        std::istringstream in2(cmd.substr(9));
                        std::string skip, name;
                        in2 >> skip >> skip;
                        if (what == "setb") {
                            int v = 0;
                            in2 >> name >> v;
                            REX::INFO("[Bridge] act setb {:#x} {} = {} -> {}", a->GetFormID(), name, v, a->SetGraphVariableBool(name.c_str(), v != 0));
                        }
                        while (what != "setb" && in2 >> name) {
                            if (what == "ev") {
                                REX::INFO("[Bridge] act ev {:#x} {} -> {}", a->GetFormID(), name, GraphOverride::SendEvent(a, name.c_str()));
                                continue;
                            }
                            float f = 0.0f; std::int32_t i = 0; bool bv = false;
                            const bool hf = a->GetGraphVariableImplFloat(name, f), hi = a->GetGraphVariableImplInt(name, i), hb = a->GetGraphVariableImplBool(name, bv);
                            REX::INFO("[Bridge] act var {:#x} {}: float {} {:.2f} | int {} {} | bool {} {}", a->GetFormID(), name, hf, f, hi, i, hb, bv);
                        }
                        continue;
                    }
                    Activity::Describe(a);
                    continue;
                }
                // Test helper: "f4mp:bodyfurn" logs the furniture each body occupies.
                if (cmd == "f4mp:bodyfurn") {
                    for (auto& [pid, fid] : GetBodies()) {
                        auto* f = RE::TESForm::GetFormByID(fid);
                        auto* a = f ? f->As<RE::Actor>() : nullptr;
                        RE::TESObjectREFR* furn = (a && a->currentProcess) ? a->currentProcess->GetOccupiedFurniture().get().get() : nullptr;
                        REX::INFO("[FURN] body of player {} ({:#x}): furniture {:#x}, at {:.0f} {:.0f} {:.0f}", pid, fid,
                            furn ? furn->GetFormID() : 0u, a ? a->GetPosition().x : 0.f, a ? a->GetPosition().y : 0.f, a ? a->GetPosition().z : 0.f);
                    }
                    continue;
                }
                // Test helper: "f4mp:nearfurn" logs the nearest furniture references.
                if (cmd == "f4mp:nearfurn") {
                    auto* pl = RE::PlayerCharacter::GetSingleton();
                    auto* tes = RE::TES::GetSingleton();
                    std::vector<std::pair<float, uint32_t>> found;
                    if (pl && tes)
                        tes->ForEachReferenceInRange(pl->GetPosition(), 3000.0f, [&](RE::TESObjectREFR* ref) {
                            auto* b = ref ? ref->GetObjectReference() : nullptr;
                            if (b && b->GetFormType() == RE::ENUM_FORM_ID::kFURN && ref->Get3D())
                                found.emplace_back(ref->GetPosition().GetDistance(pl->GetPosition()), ref->GetFormID());
                            return RE::BSContainer::ForEachResult::kContinue;
                        });
                    std::sort(found.begin(), found.end());
                    for (size_t i = 0; i < found.size() && i < 12; ++i)
                        if (auto* r = RE::TESForm::GetFormByID(found[i].second)->As<RE::TESObjectREFR>())
                            REX::INFO("[FURN] {:08X} at {:.0f}: {:.0f} {:.0f} {:.0f}", found[i].second, found[i].first,
                                r->GetPosition().x, r->GetPosition().y, r->GetPosition().z);
                    continue;
                }
                // Test helper: "f4mp:gotohost" = the menu's "Go to host".
                if (cmd == "f4mp:gotohost") {
                    GoToHost(false);
                    continue;
                }
                // Test helper: "f4mp:dump3d <refhex> [filter]" logs a reference's 3D tree (name, culled).
                if (cmd.rfind("f4mp:dump3d ", 0) == 0) {
                    char filter[64]{};
                    unsigned id = 0;
                    if (cmd.compare(12, 5, "near:") == 0) {   // f4mp:dump3d near:<basehex> = nearest actor of that base
                        std::sscanf(cmd.c_str() + 17, "%x %63s", &id, filter);
                        auto* nearest = FindNearbyActor(id, {});
                        id = nearest ? nearest->GetFormID() : 0;
                    } else {
                        std::sscanf(cmd.c_str() + 12, "%x %63s", &id, filter);
                    }
                    auto* form = RE::TESForm::GetFormByID(id);
                    auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
                    auto* root = ref ? ref->Get3D() : nullptr;
                    REX::INFO("[Dump3D] {:#x}: 3D {}", id, static_cast<void*>(root));
                    if (auto* act = ref ? ref->As<RE::Actor>() : nullptr) {
                        auto* npc = act->GetNPC();
                        auto* base = ref->GetObjectReference();
                        std::string parts;
                        if (npc)
                            for (auto* hp : npc->GetHeadParts()) parts += std::format(" {:08X}", hp ? hp->GetFormID() : 0u);
                        REX::INFO("[Dump3D] base {:#x} npc {:#x} faceNPC {:#x} alt {} origRace {:#x} race {:#x} parts [{} ]",
                            base ? base->GetFormID() : 0u, npc ? npc->GetFormID() : 0u,
                            npc && npc->faceNPC ? npc->faceNPC->GetFormID() : 0u, npc ? npc->UsingAlternateHeadPartList() : false,
                            npc && npc->originalRace ? npc->originalRace->GetFormID() : 0u,
                            npc && npc->formRace ? npc->formRace->GetFormID() : 0u, parts);
                    }
                    std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* o, int depth) {
                        if (!o || depth > 40) return;
                        const char* nm = o->GetName().c_str();
                        std::string name = nm ? nm : "";
                        std::string lower = name;
                        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        if (!filter[0] || lower.find(filter) != std::string::npos)
                            REX::INFO("[Dump3D] {}{} culled {} flags {:#x}", std::string(depth, ' '), name, o->GetAppCulled(), o->GetFlags());
                        if (auto* node = o->IsNode())
                            for (auto& child : node->children) walk(child.get(), depth + 1);
                    };
                    walk(root, 0);
                    continue;
                }
                // Test helper: "f4mp:createchar <name>" = the menu's "Create a new character".
                if (cmd.rfind("f4mp:createchar ", 0) == 0) {
                    g_newCharacterName = cmd.substr(16);
                    g_creatingCharacter = true;
                    RE::Console::ExecuteCommand("showlooksmenu player 1");
                    continue;
                }
                // Test helper: "f4mp:findweap <text>" logs weapons whose name contains <text>.
                if (cmd.rfind("f4mp:findweap ", 0) == 0) {
                    std::string want = cmd.substr(14);
                    std::transform(want.begin(), want.end(), want.begin(), ::tolower);
                    int shown = 0;
                    if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                        for (auto* w : dh->GetFormArray<RE::TESObjectWEAP>()) {
                            if (!w || shown >= 15) continue;
                            std::string name(RE::TESFullName::GetFullName(*w));
                            std::string low = name;
                            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                            if (low.find(want) == std::string::npos) continue;
                            REX::INFO("[Bridge]   weapon {:08X} '{}' ammo {:08X}", w->GetFormID(), name,
                                w->weaponData.ammo ? w->weaponData.ammo->GetFormID() : 0);
                            shown++;
                        }
                    }
                    continue;
                }
                RE::Console::ExecuteCommand(cmd.c_str());
            }
        });
    }

    // The co-op launcher's PLAY: FathersFallout.ini [Launch] LoadSave=<save name> loads that save as soon as
    // the main menu is up, once (the key is cleared first, so a normal launch isn't affected).
    static void LoadSaveFromLauncher()
    {
        char save[260]{};
        GetPrivateProfileStringA("Launch", "LoadSave", "", save, sizeof(save), IniPath().c_str());
        if (!save[0]) return;
        WritePrivateProfileStringA("Launch", "LoadSave", nullptr, IniPath().c_str());
        std::string name = save;
        REX::INFO("[FathersFallout] launcher: will load '{}' at the main menu", name);
        std::thread([name]() {
            using namespace std::chrono;
            const auto until = steady_clock::now() + minutes(3);
            while (steady_clock::now() < until) {
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->GetMenuOpen("MainMenu") && !ui->GetMenuOpen("LoadingMenu")) break;
                std::this_thread::sleep_for(milliseconds(250));
            }
            std::this_thread::sleep_for(milliseconds(1500));   // let the menu finish opening
            F4SE::GetTaskInterface()->AddTask([name]() {
                REX::INFO("[FathersFallout] launcher: loading '{}'", name);
                RE::Console::ExecuteCommand(("load \"" + name + "\"").c_str());
            });
        }).detach();
    }

    // ---- Network thread: handles messages, sends our position, syncs bodies ----
    static void NetThread()
    {
        using namespace std::chrono;
        auto lastSend = steady_clock::now();
        auto lastSync = steady_clock::now();

        for (;;) {
            auto& net = Network::NetworkClient::GetInstance();
            net.PumpMessages();
            WatchConnection();
            MaybeTravelOnJoin();

            static auto s_lastBridge = steady_clock::now();
            if (duration_cast<milliseconds>(steady_clock::now() - s_lastBridge).count() >= 500) {
                s_lastBridge = steady_clock::now();
                PollConsoleBridge();
            }

            static bool s_sentAppearance = false;
            if (net.IsConnected()) {
                auto now = steady_clock::now();

                // Send our appearance ONCE after connecting.
                if (!s_sentAppearance) {
                    F4SE::GetTaskInterface()->AddTask([]() {
                        auto& net2 = Network::NetworkClient::GetInstance();
                        if (!net2.IsConnected()) return;
                        auto ap = CaptureAppearance(net2.GetPlayerId());
                        net2.SendPacket(Network::MessageType::PlayerAppearance, &ap, sizeof(ap));
                        REX::INFO("[FathersFallout] appearance sent (sex={} race={:#x} headparts={})",
                            ap.sex, ap.raceFormID, ap.numHeadParts);
                    });
                    s_sentAppearance = true;
                }

                // Invisible dedicated host: our player isn't sent (we don't appear).
                if (!g_hideSelf && duration_cast<milliseconds>(now - lastSend).count() >= 16) {  // 60/s
                    static RE::NiPoint3 s_lastPos{};
                    static bool s_havePos = false;
                    static auto s_lastPosTime = now;
                    static Network::PlayerPositionMsg s_lastMsg{};
                    static bool s_haveMsg = false;

                    LocalSample ls;
                    {
                        std::lock_guard l(g_localLock);
                        ls = g_local;
                    }
                    static uint64_t s_sentSeq = 0;
                    // Only a fresh sample: the same position twice reads as standing still.
                    if (ls.valid && ls.seq != s_sentSeq && now - ls.at < std::chrono::milliseconds(500)) {
                        s_sentSeq = ls.seq;
                        const float sendDt = std::chrono::duration<float>(ls.at - s_lastPosTime).count();
                        s_lastPosTime = ls.at;
                        const auto pos = ls.pos;
                        const float facing = ls.heading;

                        // Speed and direction from how far we actually moved.
                        float speed = 0.0f, moveDir = 0.0f;
                        if (s_havePos && sendDt > 0.0001f && sendDt < 1.0f) {
                            float dx = pos.x - s_lastPos.x, dy = pos.y - s_lastPos.y;
                            float horizDist = std::sqrt(dx * dx + dy * dy);
                            speed = horizDist / sendDt;
                            if (horizDist > 1.0f) moveDir = std::atan2(dx, dy) - facing;  // 0 = adelante
                        }
                        s_lastPos = pos;
                        s_havePos = true;

                        Network::PlayerPositionMsg msg{};
                        msg.playerId = net.GetPlayerId();
                        msg.x = pos.x;
                        msg.y = pos.y;
                        msg.z = pos.z;
                        msg.angleZ = facing;  // where you look (camera)
                        // Movement over the last ~150 ms: a single send step reads 0 or double whenever
                        // sends and game frames aren't in step.
                        static std::deque<std::pair<std::chrono::steady_clock::time_point, RE::NiPoint3>> s_track;
                        s_track.emplace_back(ls.at, pos);
                        while (s_track.size() > 2 && ls.at - s_track.front().first > std::chrono::milliseconds(150)) s_track.pop_front();
                        float trackSpeed = 0.0f, trackDir = 0.0f;
                        if (const float secs = std::chrono::duration<float>(ls.at - s_track.front().first).count(); secs > 0.05f) {
                            const float dx = pos.x - s_track.front().second.x, dy = pos.y - s_track.front().second.y;
                            trackSpeed = std::sqrt(dx * dx + dy * dy) / secs;
                            if (trackSpeed > 10.0f) trackDir = std::atan2(dx, dy) - facing;
                        }
                        // Prefer the game's own animation speed/direction (smooth, exact units). Not while
                        // it reads (near) 0 and the player clearly moves: aiming/firing a gun, the graph's
                        // Speed dropped to 0 for a moment every second and the other screen
                        // stopped and restarted the body's walk: it slid.
                        if (const float gs = g_localGraphSpeed.load(); gs >= 0.0f) {
                            speed = gs;
                            moveDir = g_localGraphDir.load() * 6.2831853f;   // back to radians on the wire
                            if (trackSpeed > 40.0f && gs < trackSpeed * 0.5f) {
                                speed = trackSpeed;
                                moveDir = trackDir;
                            }
                        }
                        msg.pitch = ls.pitch;   // looking up/down: aim and replayed shots
                        msg.speed = speed;
                        msg.moveDir = moveDir;
                        msg.isMoving = speed > 10.0f;
                        // The game's own sprint state (a normal run is ~370 units/s, faster than
                        // any speed threshold that would tell run and sprint apart).
                        const uint32_t mm = g_localMoveMode.load();
                        msg.isSprinting = mm != 0xFFFFFFFF ? (mm & kMoveModeSprinting) != 0 : speed > 450.0f;
                        msg.isRunning = speed > 150.0f && !msg.isSprinting;
                        msg.isSneaking = ls.sneaking;
                        msg.isJumping = ls.jumping;              // in the air / jumping
                        msg.weaponDrawn = ls.weaponDrawn;  // weapon out?
                        if (ls.lightOn) msg.extraFlags |= Network::kPosLightOn | (ls.lightColor << Network::kPosLightColorShift);   // Pip-Boy light / headlamp
                        msg.cellId = Authority::LocalSpaceAtomic();   // interior cell / worldspace
                        lastSend = now;
                        net.SendPacket(Network::MessageType::PlayerPosition, &msg, sizeof(msg));
                        s_lastMsg = msg;
                        s_haveMsg = true;
                    } else if (s_haveMsg && now - lastSend >= std::chrono::milliseconds(250)) {
                        // No new frame (loading screen, a stall): keep the others' body of us standing where
                        // we were, or they'd forget us after 5 s and despawn it.
                        lastSend = now;
                        s_lastMsg.speed = 0.0f;
                        s_lastMsg.isMoving = false;
                        net.SendPacket(Network::MessageType::PlayerPosition, &s_lastMsg, sizeof(s_lastMsg));
                    }
                }
                // Bodies are synced on the MAIN thread ~60/s (smooth interpolation).
                if (duration_cast<milliseconds>(now - lastSync).count() >= 16) {
                    lastSync = now;
                    g_gateBodies.Queue([]() { BodiesSync(); });
                }

                // Diagnostic: how long main-thread tasks wait in the F4SE queue. After the first
                // load from the main menu they have been seen to stall ~30 s.
                static auto lastProbe = now;
                if (duration_cast<milliseconds>(now - lastProbe).count() >= 1000) {
                    lastProbe = now;
                    g_gateProbe.Queue([queued = now]() {
                        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - queued).count();
                        if (waited > 500) {
                            auto* ui = RE::UI::GetSingleton();
                            REX::WARN("[FathersFallout] main-thread task waited {} ms (menuMode {}, LoadingMenu {}, Console {})",
                                waited, ui ? ui->menuMode : 0u,
                                ui && ui->GetMenuOpen("LoadingMenu"), ui && ui->GetMenuOpen("Console"));
                        }
                    });
                }

                // Co-op sync (equipment, combat, items, quests) ~10/s on the main thread.
                static auto lastCoop = now;
                if (duration_cast<milliseconds>(now - lastCoop).count() >= 100) {
                    lastCoop = now;
                    g_gateCoop.Queue([]() { Coop::Tick(); });
                }

                static auto lastNpcSend = now;
                static auto lastNpcSync = now;
                // Every game broadcasts the NPCs it owns (~10/s) and mirrors the others' (~30/s).
                if (duration_cast<milliseconds>(now - lastNpcSend).count() >= 100) {
                    lastNpcSend = now;
                    g_gateNpcSend.Queue([]() { BroadcastNpcs(); });
                }
                if (duration_cast<milliseconds>(now - lastNpcSync).count() >= 33) {
                    lastNpcSync = now;
                    g_gateNpcSync.Queue([]() { NpcsSync(); });
                }
            } else {
                if (s_sentAppearance)  // just dropped: forget co-op session state
                    F4SE::GetTaskInterface()->AddTask([]() { Coop::OnDisconnected(); });
                s_sentAppearance = false;  // send the appearance again after reconnecting
            }

            std::this_thread::sleep_for(milliseconds(8));
        }
    }

    // ---- Spawn TEST: places a Protectron at your position through Papyrus ----
    // Calls ObjectReference.PlaceAtMe through the script VM (no game-version addresses). Test only:
    // checks that spawning works. 0x00000007 = the player's base (your look): a copy of your character.
    // (Antes 0x00106B09 = Protectron de prueba.)
    static const uint32_t kDummyBase = 0x00000007;

    // Remote player bodies use a neutral human base instead of a copy of the local player:
    // no default outfit (the player base's Vault111Suit refilled any empty body slot), no
    // template, no name (so no "Talk" prompt with the host's character name). Picked by sex
    // from the player's appearance message. Fallout4.esm "RelayTowerHuman*Actor" records.
    static const uint32_t kBodyBaseMale = 0x0003C62E;
    static const uint32_t kBodyBaseFemale = 0x0018E8F2;
    static uint32_t g_spawnBase = kBodyBaseMale;

    // Finds an actor of the given base near the player that isn't claimed yet.
    static RE::Actor* FindNearbyActor(uint32_t baseFormId, const std::unordered_set<uint32_t>& exclude)
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return nullptr;
        auto* cell = pl->GetParentCell();
        if (!cell) return nullptr;
        const RE::NiPoint3 pos = pl->GetPosition();

        // Take the NEWEST match (highest runtime FormID): the actor placeatme just created,
        // not an older copy such as a test dummy standing nearby.
        RE::Actor* found = nullptr;
        cell->ForEachReferenceInRange(pos, 800.0f, [&](RE::TESObjectREFR* ref) {
            if (ref && ref != static_cast<RE::TESObjectREFR*>(pl)) {
                auto* base = ref->GetObjectReference();
                if (base && base->GetFormID() == baseFormId && exclude.find(ref->GetFormID()) == exclude.end() &&
                    !ref->IsDeleted() && !ref->IsDisabled() &&
                    (!found || ref->GetFormID() > found->GetFormID())) {
                    if (auto* a = ref->As<RE::Actor>()) found = a;
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return found;
    }

    // ================= Remote players' bodies =================
    static std::unordered_map<uint32_t, uint32_t> g_bodies;      // playerId -> the body actor's FormID
    static std::unordered_map<uint32_t, float> g_lastHeading;    // playerId -> last heading applied
    // Where each body is drawn, for the name tags (the HUD runs on the render thread).
    static std::mutex g_tagLock;
    static std::unordered_map<uint32_t, RE::NiPoint3> g_tagBodyPos;

    // Used by CoopSync (main thread only, like g_bodies itself).
    bool IsHost() { return g_isHost; }
    std::vector<std::pair<uint32_t, uint32_t>> GetBodies() { return { g_bodies.begin(), g_bodies.end() }; }
    uint32_t GetPlayerIdForBody(uint32_t formId)
    {
        for (auto& [pid, fid] : g_bodies)
            if (fid == formId) return pid;
        return 0;
    }

    // Last movement state applied to each body: graph events (moveStart/moveStop, SprintStart/
    // SprintStop) are sent ONLY when it changes, not every tick (or the graph never moves on).
    struct BodyAnimState { bool moving = false; bool sprinting = false; bool init = false;
                           bool weaponDrawn = false; bool jumping = false; bool sneaking = false;
                           float smoothSpeed = 0.0f; float aimX = 0.0f; float aimY = 0.0f; float pitch = 0.0f;
                           std::chrono::steady_clock::time_point lastDrawCheck{}; bool drawReached = true;
                           // Combat guard: when the body was pulled out of combat (re-calm its AI
                           // shortly after), and recent re-draws (a draw/holster tug-of-war).
                           std::chrono::steady_clock::time_point recalmAt{};
                           std::vector<std::chrono::steady_clock::time_point> redraws;
                           // Standing still: is the body still getting closer to its spot, or blocked?
                           std::chrono::steady_clock::time_point idleCheckAt{}; float idleCheckLag = 0.0f; bool blocked = false;
                           bool parked = false;   // stuck off its spot while its player stands: put there once, then left alone
                           bool senderIdle = false;   // its player has stood still for 250 ms (no walk wanted, whatever we measure)
                           RE::NiPoint3 idleCheckTarget{};
                           // The received speed jumps 0 / 1x / 2x between packets (sends and frames aren't in
                           // step): smoothed for the gait, and "standing" only after 250 ms without movement.
                           float rxSmooth = 0.0f; float lastRxSpeed = 0.0f; std::chrono::steady_clock::time_point lastRxMoveAt{};
                           int gait = 0;   // gait event the graph took: 0 none yet, 1 Walk, 2 Run (jog)
                           int gaitTries = 0; int gaitWant = 0; int moveRefused = 0; std::chrono::steady_clock::time_point gaitWantSince{};
                           RE::NiPoint3 lastPos{}; std::chrono::steady_clock::time_point lastPosAt{}; bool havePos = false;
                           // Footstep watchdog (StepWatch): the walk/run must really play while the body moves.
                           bool prevMoving = false; long long movingSinceMs = 0, stillSinceMs = 0, lastKickMs = 0;
                           long long lastStep = 0, stillStepsFrom = 0, quickUntilMs = 0; int stillSteps = 0; int idleKicks = 0; };
    static std::unordered_map<uint32_t, BodyAnimState> g_animState;
    static bool g_lastOkSpeed = false, g_lastOkDir = false;       // retorno de SetGraphVariable (debug)
    static bool g_graphDumped = false;                            // graph variables dumped already?

    // Diagnostics: lists which animation graph variables EXIST on the body, so the real names are
    // used instead of guesses.
    static void DumpGraphVars(RE::Actor* actor)
    {
        static const char* kFloats[] = {
            "Speed", "Direction", "SpeedSampled", "TurnDelta", "Turn", "MovementSpeed",
            "SpeedRatio", "PitchDelta", "LeftRight", "ForwardBack", "walkSpeed",
            "vertVelocity", "AnimSpeed", "fMovementDirection", "MoveSpeed"
        };
        static const char* kBools[] = {
            "IsMoving", "bMoving", "IsSprinting", "bSprinting", "bIsSprinting", "IsSneaking",
            "IsRunning", "bIsMovementDriven", "bAnimationDriven", "IsNPC", "bWantToSprint",
            "IsSynced", "bIsSynced", "GraphControlsMovement", "IsFirstPerson", "bAllowRotation",
            // JUMP / AIR / SNEAK candidates (to find the real names):
            "bInJumpState", "IsJumping", "bIsJumping", "IsInAir", "bInAir", "IsFalling",
            "bIsSneaking", "IsSneaky", "IsCrouching", "bSneaking"
        };
        REX::INFO("[FathersFallout] ===== animation graph variables (existing) =====");
        for (auto* n : kFloats) {
            float o = 0.0f;
            if (actor->GetGraphVariableImplFloat(n, o))
                REX::INFO("[FathersFallout]   FLOAT {} = {:.3f}", n, o);
        }
        for (auto* n : kBools) {
            bool o = false;
            if (actor->GetGraphVariableImplBool(n, o))
                REX::INFO("[FathersFallout]   BOOL  {} = {}", n, o);
        }
        REX::INFO("[FathersFallout] ===== end of graph variables =====");
    }
    static std::unordered_set<uint32_t> g_dummies;   // "Spawn dummy" test actors: never a player body
    static std::unordered_map<uint32_t, uint32_t> g_bodyBase;  // playerId -> base the body was spawned from
    static uint32_t g_spawnCount = 0;                // body spawns this session (for the log)
    // Spawns in the last 5 minutes. A cap per session made the other player invisible for good in a long
    // session: every load or cell change that removed a body used one up and never gave it back.
    static std::deque<std::chrono::steady_clock::time_point> g_recentSpawns;
    static bool g_spawnPending = false;
    static uint32_t g_spawnFor = 0;
    static std::chrono::steady_clock::time_point g_spawnAt;

    static std::unordered_set<uint32_t> ClaimedFormIds()
    {
        std::unordered_set<uint32_t> s(g_dummies.begin(), g_dummies.end());
        for (auto& [pid, fid] : g_bodies) s.insert(fid);
        return s;
    }

    // Removes a body through the API (more reliable than console commands). Disable hides it at once;
    // SetWantsDelete + MarkAsDeleted make the game delete it and NOT keep it in the save (placeatme
    // actors are persistent: otherwise "ghost dummies" pile up in the save).
    static void DespawnBody(uint32_t fid)
    {
        auto* form = RE::TESForm::GetFormByID(fid);
        auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!ref) return;
        AimHook::Forget(ref->As<RE::Actor>());
        ref->Disable();
        ref->SetWantsDelete(true);
        ref->MarkAsDeleted();
    }

    // Bodies are ordinary runtime actors, so a save made while connected used to store them
    // for good (they reappeared, frozen, after loading). Remove every runtime actor built
    // from a body base that is not a live body or a test dummy. Runs every 5 s.
    static void SweepOrphanBodies()
    {
        static auto s_last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (g_spawnPending || now - s_last < std::chrono::seconds(5)) return;
        // A spawn we gave up on can still land late: not a leftover yet (the next spawn's FindNearbyActor takes it).
        if (now - g_spawnAt < std::chrono::seconds(8)) return;
        s_last = now;

        auto* tes = RE::TES::GetSingleton();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!tes || !pl) return;
        const auto keep = ClaimedFormIds();
        std::vector<uint32_t> orphans;
        tes->ForEachReferenceInRange(pl->GetPosition(), 30000.0f, [&](RE::TESObjectREFR* ref) {
            if (!ref || ref == static_cast<RE::TESObjectREFR*>(pl)) return RE::BSContainer::ForEachResult::kContinue;
            const uint32_t fid = ref->GetFormID();
            if (fid < 0xFF000000 || keep.contains(fid) || ref->IsDeleted() || !ref->As<RE::Actor>())
                return RE::BSContainer::ForEachResult::kContinue;
            auto* base = ref->GetObjectReference();
            const uint32_t b = base ? base->GetFormID() : 0;
            if (b == kDummyBase || b == kBodyBaseMale || b == kBodyBaseFemale || b >= 0xFFF00000) orphans.push_back(fid);
            return RE::BSContainer::ForEachResult::kContinue;
        });
        for (uint32_t fid : orphans) {
            DespawnBody(fid);
            REX::INFO("[FathersFallout] removed leftover body actor {:#x} (from a save or an old session)", fid);
        }
    }

    // Called on F4SE kPreSaveGame: never write bodies into the save. They respawn ~3 s later
    // through the normal "body is gone" path; these respawns don't count against the cap.
    static void DespawnBodiesForSave()
    {
        for (auto& [pid, fid] : g_bodies) DespawnBody(fid);
        if (!g_bodies.empty())
            REX::INFO("[FathersFallout] saving: removed {} body(ies) so they are not stored in the save", g_bodies.size());
        g_spawnCount -= std::min<uint32_t>(g_spawnCount, static_cast<uint32_t>(g_bodies.size()));
        g_bodies.clear();
        g_lastHeading.clear();
        g_animState.clear();
        g_spawnAt = std::chrono::steady_clock::now();
        Activity::Reset();   // the frames we gave bodies (runtime references) mustn't go into the save either
        BodyLights::Clear(); // nor their lights
    }

    // Area of interest for remote bodies: see sync/Range.h (follows uGridsToLoad).

    static bool IsNearLocalPlayer(const Network::PlayerPositionMsg& p, float range)
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return false;
        // Different interior/worldspace: coordinates aren't comparable (0 = unknown, e.g. the simulator).
        const uint32_t mySpace = Authority::LocalSpace();
        if (p.cellId && mySpace && p.cellId != mySpace) return false;
        const auto me = pl->GetPosition();
        const float dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
        return dx * dx + dy * dy + dz * dz <= range * range;
    }

    // Runs on the MAIN thread: creates/moves one body per remote player.
    static void SampleLocalMovement()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            std::lock_guard l(g_localLock);
            g_local.valid = true;
            g_local.seq++;
            g_local.at = std::chrono::steady_clock::now();
            g_local.pos = player->GetPosition();
            g_local.heading = player->GetEyeHeading();
            g_local.pitch = player->data.angle.x;
            g_local.sneaking = player->IsSneaking();
            g_local.jumping = player->IsJumping();
            g_local.weaponDrawn = player->GetWeaponMagicDrawn();
            g_local.lightOn = player->IsPipboyLightOn();
            static uint32_t s_color = 0;
            static auto s_colorAt = std::chrono::steady_clock::time_point{};
            if (g_local.at - s_colorAt > std::chrono::seconds(2)) { s_colorAt = g_local.at; s_color = BodyLights::LocalColor(); }
            g_local.lightColor = s_color;   // Pip-Boy color can change in the settings
        }
        Authority::LocalSpace();   // keeps the space the network thread sends current
        float speed = -1.0f, dir = -1.0f;
        if (!player || !player->GetGraphVariableImplFloat("Speed", speed) ||
            !player->GetGraphVariableImplFloat("Direction", dir)) {
            g_localGraphSpeed = -1.0f;
            return;
        }
        g_localGraphSpeed = speed;
        g_localGraphDir = dir;
        // Aim pitch probe: the graph's own Pitch next to the camera's angle.x (units for the bodies).
        if (g_bodyTrace) {
            static float s_lastAx = 99.0f;
            float gp = -999.0f;
            player->GetGraphVariableImplFloat("Pitch", gp);
            const float ax = player->data.angle.x;
            if (std::fabs(ax - s_lastAx) > 0.15f) {
                s_lastAx = ax;
                float ap = -999.0f, ah = -999.0f, cpo = -999.0f;
                bool aimActive = false, firstPerson = false;
                player->GetGraphVariableImplFloat("AimPitchCurrent", ap);
                player->GetGraphVariableImplFloat("AimHeadingCurrent", ah);
                player->GetGraphVariableImplFloat("currentPitchOffset", cpo);
                player->GetGraphVariableImplBool("bAimActive", aimActive);
                player->GetGraphVariableImplBool("IsFirstPerson", firstPerson);
                REX::INFO("[Trace] local pitch: angle.x {:.2f} rad | Pitch {:.2f} AimPitchCurrent {:.2f} AimHeadingCurrent {:.2f} "
                          "currentPitchOffset {:.2f} bAimActive {} firstPerson {} drawn {}",
                    ax, gp, ap, ah, cpo, aimActive, firstPerson, player->GetWeaponMagicDrawn());
            }
        }
        const uint32_t mm = player->moveMode;
        if (g_bodyTrace && mm != g_localMoveMode.load())
            REX::INFO("[Trace] local moveMode {:#x} -> {:#x} (speed {:.0f})", g_localMoveMode.load(), mm, speed);
        g_localMoveMode = mm;
    }

    // Host: enemies fighting one player turn on another player who is clearly closer, like they
    // would between the player and a companion. Without this, an ambush that started on the host
    // ignored the other players until they shot first. Once a second, a few switches at most.
    static std::unordered_map<uint32_t, uint32_t> g_drivenOwner;   // NPCs mirrored here: local actor FormID -> owner

    static void ShareAggro()
    {
        // Every game, for the NPCs it runs (sync/Authority): in an area a guest's game runs, its enemies
        // ignored the host until shot.
        if (g_bodies.empty()) return;
        static auto s_next = std::chrono::steady_clock::now();
        static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_switched;
        const auto now = std::chrono::steady_clock::now();
        if (now < s_next) return;
        s_next = now + std::chrono::seconds(1);

        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!pl || !lists) return;
        std::vector<RE::Actor*> players{ pl };
        std::unordered_set<RE::Actor*> isPlayer{ pl };
        for (auto& [pid, fid] : g_bodies) {
            auto* f = RE::TESForm::GetFormByID(fid);
            if (auto* a = f ? f->As<RE::Actor>() : nullptr; a && !a->IsDisabled() && a->Get3D()) {
                players.push_back(a);
                isPlayer.insert(a);
            }
        }
        if (players.size() < 2) return;
        auto dist2 = [](RE::Actor* a, RE::Actor* b) {
            const auto p = a->GetPosition(), q = b->GetPosition();
            return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z);
        };
        // [Debug] BodyTrace: how nearby NPCs see the first body (in combat? target? hostile to it?)
        if (g_bodyTrace) {
            static auto s_nextDiag = now;
            if (now >= s_nextDiag) {
                s_nextDiag = now + std::chrono::seconds(3);
                RE::Actor* body = players[1];
                for (auto& h : lists->highActorHandles) {
                    auto npc = h.get();
                    if (!npc || isPlayer.count(npc.get()) || npc->IsDead(false) || dist2(npc.get(), body) > 2000.0f * 2000.0f) continue;
                    auto t = npc->currentCombatTarget.get();
                    REX::INFO("[Trace] npc {:#x} '{}' combat {} target {:#x} hostile-to-body {} body-hostile-to-it {} dist {:.0f}",
                        npc->GetFormID(), npc->GetDisplayFullName() ? npc->GetDisplayFullName() : "", npc->IsInCombat(),
                        t ? t->GetFormID() : 0u, npc->GetHostileToActor(body), body->GetHostileToActor(npc.get()),
                        std::sqrt(dist2(npc.get(), body)));
                }
            }
        }
        int switches = 0;
        for (auto& h : lists->highActorHandles) {
            auto npc = h.get();
            if (!npc || isPlayer.count(npc.get()) || npc->IsDead(false) || !npc->IsInCombat()) continue;
            if (!Authority::IOwn(npc.get()) || g_drivenOwner.contains(npc->GetFormID())) continue;   // a mirror: its owner decides
            auto tgt = npc->currentCombatTarget.get();
            if (!tgt || !isPlayer.count(tgt.get())) continue;
            const uint32_t nid = npc->GetFormID();
            if (auto it = s_switched.find(nid); it != s_switched.end() && now - it->second < std::chrono::seconds(4)) continue;
            RE::Actor* best = tgt.get();
            float bestD = dist2(npc.get(), best);
            for (auto* p : players) {
                const float d = dist2(npc.get(), p);
                if (d < bestD * 0.36f) { best = p; bestD = d; }   // at least 40% closer
            }
            if (best == tgt.get()) continue;
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "%X.startcombat %X", nid, best->GetFormID());
            RE::Console::ExecuteCommand(cmd);
            s_switched[nid] = now;
            REX::INFO("[Coop] {:#x} turns from {:#x} to closer player {:#x}", nid, tgt->GetFormID(), best->GetFormID());
            if (++switches >= 4) break;
        }
    }

    static std::chrono::steady_clock::time_point g_lastLoading{};   // last loading screen (or stall)
    static std::atomic<int64_t> g_lastLoadingMs{ 0 };                 // the same, readable from any thread

    static int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Nothing has loaded (or stalled the main thread) for this long.
    static bool SettledFor(int a_seconds) { return NowMs() - g_lastLoadingMs.load() > a_seconds * 1000LL; }

    // Start the walk/run. Crouched, the graph takes moveStartAnimated and refuses moveStart (testing: the
    // engine sends the player's own graph moveStartAnimated when it moves while sneaking); standing, the other
    // way round. Either name is tried.
    static bool StartMoving(RE::Actor* actor, bool sneaking)
    {
        const char* first = sneaking ? "moveStartAnimated" : "moveStart";
        const char* second = sneaking ? "moveStart" : "moveStartAnimated";
        if (GraphOverride::SendEvent(actor, first) || GraphOverride::SendEvent(actor, second)) return true;
        // Weapon out the stance has its own starts. After the
        // footstep watchdog restarted the walk of a body with its gun out, moveStart was refused for seconds
        // at a time and the body slid along on still feet.
        return !sneaking && actor->GetWeaponMagicDrawn() &&
               (GraphOverride::SendEvent(actor, "walkStart") || GraphOverride::SendEvent(actor, "runStart"));
    }

    // A mirrored NPC starts walking the way the game starts its own walk (testing, vanilla Preston pathing: walkStart,
    // moveForward, MoveStart in one frame). moveStart alone was refused or left the graph in a slow shuffle without
    // a direction: the NPCs walked in slow motion on the other screen.
    static bool StartMovingNpc(RE::Actor* actor, bool sneaking)
    {
        const bool walk = GraphOverride::SendEvent(actor, "walkStart");
        const bool fwd = GraphOverride::SendEvent(actor, "moveForward");
        const bool start = StartMoving(actor, sneaking);
        return walk || fwd || start;
    }

    // Footstep watchdog for bodies and mirrored NPCs. Locomotion events go out only when our idea of the
    // state changes, so when the graph quietly dropped out of its walk (a reset, a refused event) the body
    // glided along with still feet, or kept walking on the spot, until its player jumped, crouched or drew:
    // the "press a button to resync" reports. The graph's own footsteps say what really plays.
    // 1 = moving for 1.5 s without a footstep (send the walk/run again), 2 = steps keep coming while its
    // player/owner stands (stop it), 0 = fine or nothing to judge by (no footsteps from this graph yet).
    // standing: what the sender says (its speed), not our measurement: a body put back on its spot read as
    // moving for a moment every time, and that kept resetting the count.
    template <class S>
    static int StepWatch(RE::Actor* actor, S& st, bool moving, float speed, bool skip, bool standing, long long patience = 1500)
    {
        const long long now = NowMs();
        if (moving != st.prevMoving) {
            st.prevMoving = moving;
            if (moving) st.movingSinceMs = now;
        }
        if (!standing) { st.stillSinceMs = now; st.stillSteps = 0; }
        const long long step = GraphOverride::LastStepMs(actor);
        const bool newStep = step && step != st.lastStep;
        st.lastStep = step;
        // Right after a draw/holster look sooner: 0.8 s, not 1.5 s.
        const long long wait = now < st.quickUntilMs ? (std::min)(800LL, patience) : patience;
        if (!step || skip || now - st.lastKickMs < (now < st.quickUntilMs ? 1000 : 3000)) return 0;
        if (newStep) st.idleKicks = 0;
        if (moving && !standing) {
            // Some gaits never step: two kicks with no footstep after them = leave it be.
            if (st.idleKicks >= 2) return 0;
            if (speed > 70.0f && now - st.movingSinceMs > wait && now - step > wait) { st.lastKickMs = now; ++st.idleKicks; return 1; }
            return 0;
        }
        // Turning on the spot steps too: only a steady stream (4 in 2.5 s, a walk steps every ~0.6 s) is walking in place.
        if (standing && newStep && now - st.stillSinceMs > 800) {
            if (st.stillSteps == 0 || now - st.stillStepsFrom > 2500) { st.stillSteps = 0; st.stillStepsFrom = now; }
            if (++st.stillSteps >= 4) { st.stillSteps = 0; st.lastKickMs = now; return 2; }
        }
        return 0;
    }

    static void BodiesSync()
    {
        // An intro save loaded while connected: leave the session before anything travels or syncs.
        {
            static std::chrono::steady_clock::time_point s_next{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_next) {
                s_next = now + std::chrono::seconds(1);
                auto& n = Network::NetworkClient::GetInstance();
                if (RefreshInIntro() && (n.IsConnected() || n.IsConnecting())) {
                    REX::WARN("[FathersFallout] in the pre-war intro while connected: leaving the session");
                    n.Disconnect();
                    g_retriesLeft = 0;
                    NotifyIntro();
                }
            }
        }
        {
            // A load stalls this task: a gap of a second or more since the last call counts too.
            static std::chrono::steady_clock::time_point s_lastCall{};
            const auto now = std::chrono::steady_clock::now();
            auto* ui = RE::UI::GetSingleton();
            if ((ui && ui->GetMenuOpen("LoadingMenu")) || now - s_lastCall > std::chrono::seconds(1)) {
                g_lastLoading = now;
                g_lastLoadingMs = NowMs();
            }
            s_lastCall = now;
        }
        SampleLocalMovement();
        SweepOrphanBodies();
        ShareAggro();
        if (Network::NetworkClient::GetInstance().IsConnected()) Workshop::HoldRemoteProps();

        auto& net = Network::NetworkClient::GetInstance();
        if (!net.IsConnected()) {
            // Disconnected: remove any body left over.
            for (auto& [pid, fid] : g_bodies) {
                DespawnBody(fid);
            }
            if (!g_bodies.empty()) {
                REX::INFO("[FathersFallout] disconnected: {} body/bodies removed", g_bodies.size());
                g_bodies.clear();
            }
            g_lastHeading.clear();
            g_animState.clear();
            BodyLights::Clear();
            return;
        }
        auto remotes = net.GetRemotePlayers();

        auto clockNow = std::chrono::steady_clock::now();  // for the log throttle


        // 0. Remove the bodies of players who are no longer connected.
        std::vector<uint32_t> leftPlayers;
        for (auto& [pid, fid] : g_bodies) {
            if (remotes.find(pid) == remotes.end()) leftPlayers.push_back(pid);
        }
        for (uint32_t pid : leftPlayers) {
            uint32_t fid = g_bodies[pid];
            DespawnBody(fid);   // reliable removal through the API (Disable + MarkAsDeleted)
            g_bodies.erase(pid);
            g_lastHeading.erase(pid);
            g_animState.erase(pid);
            REX::INFO("[FathersFallout] body removed (player {} disconnected, actor {:#x})", pid, fid);
        }

        // 0a. Area of interest: a body only exists while its player is near us. Teleporting a
        //     body into an unloaded part of the world makes the engine unload it at once, which
        //     looked like "the body is gone" and caused a respawn every 3 s. Hysteresis avoids
        //     flicker at the edge; range-based despawns don't count against the spawn cap.
        std::vector<uint32_t> outOfRange;
        for (auto& [pid, fid] : g_bodies) {
            auto it = remotes.find(pid);
            if (it != remotes.end() && !IsNearLocalPlayer(it->second, Range::BodyDespawn())) outOfRange.push_back(pid);
        }
        for (uint32_t pid : outOfRange) {
            DespawnBody(g_bodies[pid]);
            g_bodies.erase(pid);
            g_lastHeading.erase(pid);
            g_animState.erase(pid);
            if (g_spawnCount > 0) g_spawnCount--;
            REX::INFO("[FathersFallout] player {} is out of range: body removed until they come back", pid);
        }

        // 0c. Appearance arrived (or changed) after this body was spawned, or the body died here
        //     (only the real player decides that): rebuild it. A fresh body, not Resurrect - a
        //     resurrected actor keeps the limbs and head its death blow blew off.
        std::vector<std::pair<uint32_t, bool>> restyle;   // (player, died)
        for (auto& [pid, fid] : g_bodies) {
            const uint32_t want = Appearance::GetBodyBase(pid);
            auto* form = RE::TESForm::GetFormByID(fid);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (actor && actor->IsDead(false)) restyle.push_back({ pid, true });
            else if (Appearance::TakeRespawnRequest(pid) || (want && g_bodyBase[pid] != want)) restyle.push_back({ pid, false });
        }
        for (auto [pid, died] : restyle) {
            DespawnBody(g_bodies[pid]);
            g_bodies.erase(pid);
            g_lastHeading.erase(pid);
            g_animState.erase(pid);
            if (g_spawnCount > 0) g_spawnCount--;
            if (died) REX::WARN("[FathersFallout] player {}'s body died on this screen: replaced by a fresh one", pid);
            else REX::INFO("[FathersFallout] player {} appearance updated: rebuilding their body", pid);
        }

        // 0b. Bodies the game removed (cell unload, cleanup) get respawned: without this
        //     the player stays invisible for the rest of the session.
        std::vector<uint32_t> lost;
        for (auto& [pid, fid] : g_bodies) {
            auto* form = RE::TESForm::GetFormByID(fid);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor || actor->IsDeleted() || actor->IsDisabled()) lost.push_back(pid);
        }
        for (uint32_t pid : lost) {
            REX::WARN("[FathersFallout] body for player {} ({:#x}) is gone, respawning", pid, g_bodies[pid]);
            g_bodies.erase(pid);
            g_lastHeading.erase(pid);
            g_animState.erase(pid);
        }

        // Their Pip-Boy lights (or headlamps), following the bodies.
        {
            std::vector<BodyLights::Want> lights;
            for (auto& [pid, fid] : g_bodies) {
                auto it = remotes.find(pid);
                auto* form = RE::TESForm::GetFormByID(fid);
                lights.push_back({ pid, form ? form->As<RE::Actor>() : nullptr, it != remotes.end() && (it->second.extraFlags & Network::kPosLightOn) != 0,
                    it != remotes.end() ? it->second.extraFlags >> Network::kPosLightColorShift : 0u });
            }
            BodyLights::Sync(lights);
        }

        // 1. Move each body smoothly toward its player's position.
        std::vector<RE::Actor*> liveBodies;
        for (auto& [pid, fid] : g_bodies) {
            auto it = remotes.find(pid);
            if (it == remotes.end()) continue;
            auto* form = RE::TESForm::GetFormByID(fid);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor) continue;
            liveBodies.push_back(actor);
            GraphOverride::Install(actor);
            AimHook::InstallSpine(actor);
            GraphOverride::InstallNoTalk(actor);
            GraphOverride::DetachMovementChannels(actor);

            // Once, when the graph has loaded, dump its variables.
            if (!g_graphDumped) {
                float probe = 0.0f;
                if (actor->GetGraphVariableImplFloat("Speed", probe)) {  // grafo listo
                    g_graphDumped = true;
                    DumpGraphVars(actor);
                }
            }

            // Sitting/lying on furniture: the furniture places and animates the body.
            if (Workshop::BodySeated(pid)) continue;

            const RE::NiPoint3 goal{ it->second.x, it->second.y, it->second.z };
            const RE::NiPoint3 bodyPos = actor->GetPosition();
            const RE::NiPoint3 delta = goal - bodyPos;
            const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

            const auto& rp = it->second;
            BodyAnimState& st = g_animState[pid];

            // The sender's speed (jumps a lot frame to frame): only for the freeze test below. The
            // walk/run animation follows how fast the body really moves on this screen (further down).
            const float rawSpeed = (rp.speed > 0.0f && rp.speed < 1000.0f) ? rp.speed : 0.0f;

            // ---- MOVEMENT: SMOOTH position following ----
            // SetPosition, because the character controller's velocity moved the body in jumps.
            // The animation is handled below: the graph is told the body is "synced" (bIsSynced) so
            // it keeps the speed we set even though physics doesn't move the body.
            // Test aid (bridge f4mp:freezebody <s>): stop placing bodies and log how far the
            // animation alone moves them - its natural speed for the current Speed value.
            if (std::chrono::steady_clock::now() < g_freezeUntil) {
                static RE::NiPoint3 s_start{};
                static std::chrono::steady_clock::time_point s_t0{};
                static bool s_on = false;
                if (!s_on) { s_on = true; s_start = bodyPos; s_t0 = std::chrono::steady_clock::now(); }
                const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_t0).count();
                if (secs > 0.5f) {
                    const float moved = std::sqrt((bodyPos.x - s_start.x) * (bodyPos.x - s_start.x) + (bodyPos.y - s_start.y) * (bodyPos.y - s_start.y));
                    static float s_lastLog = 0.0f;
                    if (secs - s_lastLog >= 0.5f) {
                        s_lastLog = secs;
                        REX::INFO("[Trace] frozen body {}: moved {:.0f} in {:.1f}s = {:.0f}/s (graph Speed {:.0f})", pid, moved, secs,
                            moved / secs, st.smoothSpeed);
                    }
                }
                if (std::chrono::steady_clock::now() + std::chrono::milliseconds(50) >= g_freezeUntil) s_on = false;
                st.smoothSpeed = st.smoothSpeed * 0.85f + rawSpeed * 0.15f;
            } else if (dist > 1500.0f) {
                actor->SetPosition(goal, true);   // teletransporte (spawn / viaje rapido)
                st.havePos = false;
            } else {
                // Its player stands still and the body is nearly there: leave it. Something solid
                // (the other player, a wall, a slope) kept it a few units off, it was pushed again
                // every tick, and that push read as walking: it walked in place.
                // Also when it stopped getting closer (blocked further off): checked every 0.3 s.
                const float lagXY = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                // A lone 0 among moving packets (sender frames and sends out of step) pulled the gait speed
                // down for a moment: the jitter players saw. Keep the last
                // speed until 0 has lasted 100 ms.
                float rxv = it->second.speed;
                if (rxv > 10.0f) st.lastRxSpeed = rxv;
                else if (clockNow - st.lastRxMoveAt < std::chrono::milliseconds(100)) rxv = st.lastRxSpeed;
                st.rxSmooth = st.rxSmooth * 0.9f + rxv * 0.1f;
                if (it->second.speed > 10.0f) st.lastRxMoveAt = clockNow;
                const bool idle = clockNow - st.lastRxMoveAt > std::chrono::milliseconds(250);
                st.senderIdle = idle;
                if (idle) st.rxSmooth = 0.0f;
                if (!idle) {
                    st.blocked = false;
                    st.parked = false;
                    st.idleCheckAt = {};
                } else if (st.idleCheckAt == std::chrono::steady_clock::time_point{}) {
                    st.idleCheckAt = clockNow;
                    st.idleCheckLag = lagXY;
                    st.idleCheckTarget = goal;
                } else if (clockNow - st.idleCheckAt > std::chrono::milliseconds(300)) {
                    // Close by and < 4 units closer in 0.3 s. Not further off: a body left alone with a
                    // walk still playing walks off on the animation's own motion, and must be brought back.
                    // Only while its spot really stays put (a sender's speed of 0 alone isn't proof).
                    const RE::NiPoint3 tm = goal - st.idleCheckTarget;
                    const bool spotStill = tm.x * tm.x + tm.y * tm.y + tm.z * tm.z < 8.0f * 8.0f;
                    const bool noProgress = lagXY > st.idleCheckLag - 4.0f;
                    // Not while its walk still plays: right after its player stopped, the walk's own motion
                    // cancelled our pull ("no progress"), the body was let go as blocked, walked on ~50 units
                    // and was snapped back.
                    st.blocked = lagXY < 64.0f && noProgress && !st.moving;
                    // Further off, or at another height (stairs, a floor inside), and not getting there: it
                    // was pushed every tick for good, that push read as walking, and the body walked in place
                    // after every interior load. Put it on the spot once and leave it.
                    if (spotStill && noProgress && !st.parked && !(st.blocked && std::fabs(delta.z) < 48.0f) && (lagXY >= 24.0f || std::fabs(delta.z) >= 48.0f)) {
                        actor->SetPosition(goal, true);
                        st.parked = true;
                        st.havePos = false;
                        static int s_logged = 0;
                        if (s_logged++ < 20)
                            REX::INFO("[FathersFallout] body of player {} stuck {:.0f} units off (height {:.0f}) while its player stands: placed", pid, lagXY, delta.z);
                    }
                    if (st.parked && (lagXY > 200.0f || !spotStill)) st.parked = false;   // drifted off / its spot moved
                    st.idleCheckAt = clockNow;
                    st.idleCheckLag = lagXY;
                    st.idleCheckTarget = goal;
                }
                const bool settled = idle && (st.parked || (std::fabs(delta.z) < 48.0f && (lagXY < 24.0f || st.blocked)));
                if (!settled) {
                    const float alpha = 0.35f;            // smoothing per tick (~60/s)
                    RE::NiPoint3 stepPos{ bodyPos.x + delta.x * alpha,
                                        bodyPos.y + delta.y * alpha,
                                        bodyPos.z + delta.z * alpha };
                    actor->SetPosition(stepPos, true);
                }
                // Animation speed = how fast the body really moves here: its position as the game
                // left it, tick to tick (not the step we asked for, which a blocked body never makes).
                // The body trails its player a little; driven by the sender's speed, the feet stopped
                // while the body still glided the last steps: the sliding players saw.
                if (settled) {
                    st.smoothSpeed *= 0.7f;   // standing: no walk (its own motion would feed itself)
                } else if (st.havePos) {
                    const float secs = std::chrono::duration<float>(clockNow - st.lastPosAt).count();
                    const float mx = bodyPos.x - st.lastPos.x, my = bodyPos.y - st.lastPos.y;
                    if (secs > 0.004f) {
                        float measured = std::sqrt(mx * mx + my * my) / secs;
                        // Under way: the player's own speed picks the gait. Ground and collision eat part of
                        // every step, so the body measured ~25% slow and ran with the walk animation.
                        // Not while it's only starting off (measured still low): it walked in place then.
                        const float rx = st.rxSmooth;
                        if (!idle && rx > 30.0f && measured > rx * 0.5f) measured = (std::max)(measured, rx * 0.95f);
                        // Players move at fixed gaits (walk ~95-130, jog 373, sprint 520+). Faster than a walk
                        // = the jog: the graph showed the walk animation up to ~300.
                        if (!idle && rx > 220.0f && measured > 150.0f) measured = (std::max)(measured, 380.0f);
                        if (measured < 1200.0f) st.smoothSpeed = st.smoothSpeed * 0.7f + measured * 0.3f;
                    }
                }
                st.lastPos = bodyPos;
                st.lastPosAt = clockNow;
                st.havePos = true;
            }

            // Rotation: face where the player LOOKS (SetHeading doesn't restart the animation).
            // Turn smoothly toward the player's facing (an instant snap makes the feet slide while
            // the body spins). Fast enough to keep up with a mouse flick: closes 30% of the gap per
            // tick, at least 0.6 rad/s, and snaps the last 0.02 rad.
            {
                const float want = it->second.angleZ;
                auto hit = g_lastHeading.find(pid);
                float cur = hit != g_lastHeading.end() ? hit->second : want;
                float diff = want - cur;
                while (diff > 3.14159265f) diff -= 6.2831853f;
                while (diff < -3.14159265f) diff += 6.2831853f;
                float step = diff * 0.30f;
                const float minStep = 0.01f;   // ~0.6 rad/s at 60 ticks/s
                if (std::fabs(diff) < 0.02f) step = diff;
                else if (std::fabs(step) < minStep) step = diff > 0 ? minStep : -minStep;
                cur += step;
                g_lastHeading[pid] = cur;
                actor->SetHeading(cur);
            }
            // Looking up/down like its player, into the graph's Pitch. NOT yet a visible aim: the body's
            // AI still aims its gun at the nearest player (testing: Pitch, look-at point, head-tracking marker
            // and AimPitchCurrent/bAimActive all failed to move it). Eased like the heading.
            {
                const float want = std::clamp(it->second.pitch, -1.4f, 1.4f);
                st.pitch += (want - st.pitch) * 0.35f;
                // Not the reference's own angle.x: that tipped the whole body over.
                actor->data.angle.x = 0.0f;
                GraphOverride::SetPitch(actor, st.pitch);
                // Where its replayed shots go: its player's facing and look pitch (AimHook).
                AimHook::SetBodyAim(actor, g_lastHeading[pid], st.pitch);
            }

            // ---- ANIMACION ----
            // Tell the graph the body is SYNCED / driven from outside, so it USES the speed we set
            // instead of recomputing it as 0 (which froze the animation, since physics doesn't move
            // the body).
            // [Debug] BodyTrace=1: 4 lines/s per body. The graph's Speed is read BEFORE we set it
            // again, i.e. what the engine left there since last frame (did it keep ours?).
            if (g_bodyTrace) {
                static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_lastTrace;
                if (std::chrono::duration<float>(clockNow - s_lastTrace[pid]).count() >= 0.25f) {
                    s_lastTrace[pid] = clockNow;
                    float gSpeed = -1.0f, gDir = -99.0f;
                    actor->GetGraphVariableImplFloat("Speed", gSpeed);
                    actor->GetGraphVariableImplFloat("Direction", gDir);
                    const float lag = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                    float eap = 0.0f, eah = 0.0f;
                    GraphOverride::EngineAim(eap, eah);
                    const long long stepAge = GraphOverride::LastStepMs(actor) ? NowMs() - GraphOverride::LastStepMs(actor) : -1;
                    REX::INFO("[Trace] p{} step {} ms ago | rx {:.0f} smooth {:.0f} graphSpeed {:.0f} | lag {:.0f} | moving {} sprint {} (rx {}) | "
                              "dir rx {:.2f} graph {:.2f} | heading {:.2f} | pitch rx {:.2f} | engine aim p {:.2f} h {:.2f} | hook {} / {} | blocked {}",
                        pid, stepAge, rp.speed, st.smoothSpeed, gSpeed, lag, st.moving, st.sprinting, rp.isSprinting, rp.moveDir, gDir, rp.angleZ,
                        rp.pitch, eap, eah, GraphOverride::CallCount(), GraphOverride::OverrideCount(), GraphOverride::BlockedCount());
                }
            }

            actor->SetGraphVariableBool("bIsSynced", true);
            g_lastOkSpeed = actor->SetGraphVariableFloat("Speed", st.smoothSpeed);
            // The engine zeroes Speed every frame (the body doesn't move by physics); the graph
            // hook writes ours again right before the graph samples it.
            {
                // Direction as the graph wants it: fraction of a full turn, 0 = forward, 0.25 = right.
                float dirFrac = std::fmod(rp.moveDir / 6.2831853f, 1.0f);
                if (dirFrac < 0.0f) dirFrac += 1.0f;
                GraphOverride::Set(actor, st.smoothSpeed, st.smoothSpeed > 8.0f ? dirFrac : 0.0f);
            }
            // Direction: written by GraphOverride (fraction of a turn), not here.

            // State with HYSTERESIS (different start and stop thresholds, no flicker).
            // Its player stopped: stop the walk now. Our pull toward the last spot kept the measured speed
            // up, so the body stepped down to a walk and carried on past its player instead.
            const bool wantMoving = !st.senderIdle && (st.moving ? (st.smoothSpeed > 8.0f) : (st.smoothSpeed > 30.0f));
            const bool wantSprint = rp.isSprinting && wantMoving;   // sender's own sprint state

            actor->SetGraphVariableBool("IsSprinting", wantSprint);
            // Sneaking: variables every frame + an EVENT on the change (the variable alone isn't enough; the
            // graph enters sneak mode on the event, like walking on moveStart).
            actor->SetGraphVariableBool("IsSneaking", rp.isSneaking);
            actor->SetGraphVariableBool("bIsSneaking", rp.isSneaking);
            // Re-assert the sneaking STATE EVERY frame: set only on the change, the body stands up again by
            // itself. This keeps it crouched as long as the player is.
            if (actor->IsSneaking() != rp.isSneaking) {
                actor->SetSneaking(rp.isSneaking);
            }
            if (st.sneaking != rp.isSneaking) {
                GraphOverride::SendEvent(actor, rp.isSneaking ? "SneakStart" : "SneakStop");
                st.sneaking = rp.isSneaking;
            }

            // Weapon out: draw/holster on the change (changes the body's stance).
            // Weapon out or away like its player. Checked against the body's real state twice a
            // second too: equipping or a graph reset can holster it behind our back, and a body
            // with no weapon out can't show the player's aiming and shots.
            // No head tracking: a body's AI looked at the nearest player and its replayed shots went
            // there, not where it faced.
            actor->headTracking = false;
            if (auto* proc = actor->currentProcess; proc && proc->high) {
                for (auto& h : proc->high->headTrackingTargets) h.reset();
                proc->high->lastTarget.reset();
            }
            // A body must never fight on its own: an enemy hitting it put
            // its AI into combat, and after the fight the AI kept holstering the weapon while this
            // code drew it again, every 0.7 s, and the busy graph refused the player's shots.
            if (actor->IsInCombat()) {
                actor->StopCombat();
                if (st.recalmAt == std::chrono::steady_clock::time_point{})
                    REX::INFO("[FathersFallout] body of player {} was pulled into combat: stopped", pid);
                st.recalmAt = clockNow + std::chrono::milliseconds(300);
            } else if (st.recalmAt != std::chrono::steady_clock::time_point{} && clockNow >= st.recalmAt) {
                actor->InitiateDoNothingPackage();   // combat replaced it; without it the AI holsters
                st.recalmAt = {};
                st.lastDrawCheck = {};               // put the weapon back the way its player has it
            }
            // Not while its equipment is still on the way: a draw asked for before the weapon was given
            // (its player arrived with the gun out) was cut by the weapon's 3D rebuild and the body
            // stayed half-drawn for good, refusing every aim and shot.
            GraphOverride::SetWantDrawn(actor, rp.weaponDrawn);
            if (!Coop::EquipmentSettled(pid, fid)) {
                st.lastDrawCheck = clockNow;
            } else {
                if (rp.weaponDrawn && st.drawReached && KeepDrawn(actor)) {
                    static int s_kept = 0;
                    if (s_kept++ < 20) REX::INFO("[FathersFallout] body of player {}: its AI started holstering - kept drawn", pid);
                }
                const bool bodyPos = WeaponStateOf(actor) >= 3;
                const bool changed = st.weaponDrawn != rp.weaponDrawn;
                // Reached the wanted state since our last request? Then a later difference means
                // something else changed it (0.5 s to correct). Not reached yet: the draw animation
                // may still be playing, so 1.5 s. Re-asking every 0.5 s restarted the draw, and the
                // guard below then reset the AI, which holstered it: hands stayed empty.
                if (bodyPos == st.weaponDrawn) st.drawReached = true;
                const float since = std::chrono::duration<float>(clockNow - st.lastDrawCheck).count();
                const bool drifted = bodyPos != rp.weaponDrawn && since > (st.drawReached ? 0.5f : 1.5f);
                const bool undone = drifted && st.drawReached;   // was right, then changed behind our back
                if (changed || drifted) {
                    // Stuck half-way (wants to draw / drawing, or the same for holstering) since our last
                    // request: asking again does nothing, the state machine thinks it's under way. Put
                    // the state back to where the request starts from first.
                    const int ws = WeaponStateOf(actor);
                    const bool stuck = drifted && !st.drawReached && ws != 0 && ws != 3;
                    if (stuck) {
                        actor->SetWeaponState(rp.weaponDrawn ? RE::WEAPON_STATE::kSheathed : RE::WEAPON_STATE::kDrawn);
                        // Inside nothing ever picked the request up: it sat at "want to
                        // draw" for good. The graph's own draw/holster event works there.
                        GraphOverride::SendEvent(actor, rp.weaponDrawn ? "WeapEquip" : "Unequip");
                        static int s_stuck = 0;
                        if (s_stuck++ < 20)
                            REX::INFO("[FathersFallout] body of player {}: weapon stuck in state {} - reset, {} again", pid,
                                ws, rp.weaponDrawn ? "drawing" : "holstering");
                    }
                    actor->DrawWeaponMagicHands(rp.weaponDrawn);
                    st.quickUntilMs = NowMs() + 3000;
                    st.weaponDrawn = rp.weaponDrawn;
                    st.lastDrawCheck = clockNow;
                    st.drawReached = false;
                    if (drifted && g_bodyTrace)
                        REX::INFO("[Trace] p{} weapon {} again (state {}, process {})", pid, rp.weaponDrawn ? "drawn" : "holstered",
                            ws, actor->currentProcess ? static_cast<int>(actor->currentProcess->processLevel) : -1);
                }
                // Something else still fights the weapon state: 3 undone in 5 s = re-calm the AI.
                if (undone) {
                    st.redraws.push_back(clockNow);
                    std::erase_if(st.redraws, [&](auto t) { return clockNow - t > std::chrono::seconds(5); });
                    if (st.redraws.size() >= 3) {
                        st.redraws.clear();
                        actor->StopCombat();
                        actor->InitiateDoNothingPackage();
                        static int s_warned = 0;
                        if (s_warned++ < 20) REX::WARN("[FathersFallout] body of player {}: weapon keeps changing on its own; AI calmed again", pid);
                    }
                }
            }

            if (!st.init) {
                // A body that appears while its player is already moving still needs the start
                // events, or its graph stays in idle while it glides along.
                // Not before the graph exists, or the event is silently dropped.
                float graphReady = 0.0f;
                if (!actor->GetGraphVariableImplFloat("Speed", graphReady)) continue;
                REX::INFO("[FathersFallout] body {} ready: moving {}, sprint {}", pid, wantMoving, wantSprint);
                st.moving = false;      // the transition code below sends moveStart/SprintStart
                st.sprinting = false;
                // Standing: say so. A body spawned while the engine had it walking (testing: right after an interior
                // load, graph Speed 80 at spawn) kept walking on the spot, since "standing" was never sent.
                if (!wantMoving) GraphOverride::SendEvent(actor, "moveStop");
                st.init = true;
            }
            // The engine reset the graph's state (relaxed stance, weapon (un)equip...): resend ours.
            if (GraphOverride::TakeResync(actor)) {
                st.moving = false;
                st.sprinting = false;
            }
            // A state counts as sent only once the graph accepts it; refused events are retried
            // next tick (the graph refuses e.g. moveStart while it's still in a start-up state).
            if (wantMoving != st.moving) {
                bool ok = wantMoving ? StartMoving(actor, rp.isSneaking) : GraphOverride::SendEvent(actor, "moveStop");
                // Refused for a quarter second while the body is being moved = it slides. Seen right after the
                // local player crouched (the body's AI copies that): if the graph's stance isn't its player's,
                // put it back first, then start walking again. Only then: resending sneakStart every quarter
                // second restarted the crouch each time, and a crouching body glided along for good.
                // Refused because it already walks: a landing (jumpLandToRun) or a sprint put the graph in its
                // locomotion itself, and moveStart is only taken from standing. Fresh footsteps say so: count
                // it as moving, so the gait events go out (seen refused for 6 s, sliding meanwhile).
                if (wantMoving && !ok && st.moveRefused >= 4) {
                    const long long step = GraphOverride::LastStepMs(actor);
                    if (step && NowMs() - step < 700) {
                        ok = true;
                        static int s_logged = 0;
                        if (s_logged++ < 20) REX::INFO("[FathersFallout] body of player {}: walk refused but it is walking already (footsteps): taken as moving", pid);
                    }
                }
                if (wantMoving && !ok && ++st.moveRefused >= 15) {
                    st.moveRefused = 0;
                    std::int32_t inSneak = 0;
                    const bool known = actor->GetGraphVariableImplInt("iIsInSneak", inSneak);
                    const bool off = known && (inSneak != 0) != rp.isSneaking;
                    if (off) GraphOverride::SendEvent(actor, rp.isSneaking ? "sneakStart" : "sneakStop");
                    ok = StartMoving(actor, rp.isSneaking);
                    // Still refused with no footsteps: the graph sits in a locomotion state that doesn't play
                    // (refused for 4 s after the watchdog's restart). Stop it, then start from standing.
                    if (!ok && GraphOverride::SendEvent(actor, "moveStop")) ok = StartMoving(actor, rp.isSneaking);
                    static int s_logged = 0;
                    if (s_logged++ < 30)
                        REX::INFO("[FathersFallout] body of player {}: walk refused 0.25 s - stance {}{}, start -> {}", pid,
                            rp.isSneaking ? "sneaking" : "standing", off ? " resent" : " already right", ok);
                }
                if (ok) st.moveRefused = 0;
                if (ok || !wantMoving) st.moving = wantMoving;
                if (g_bodyTrace && ok) REX::INFO("[Trace] p{} {} accepted", pid, wantMoving ? "moveStart" : "moveStop");
            }
            if (wantSprint != st.sprinting && (st.moving || !wantSprint)) {
                const bool ok = GraphOverride::SendEvent(actor, wantSprint ? "SprintStart" : "SprintStop");
                if (ok || !wantSprint) st.sprinting = wantSprint;
                // Out of a sprint the graph refused "Run"/"Walk" for over a second and the body slid at jog
                // speed in the walk: start the locomotion again (moveStart, retried until taken), then the gait.
                if (!wantSprint) st.moving = false;
                if (ok) st.gait = 0;
            }
            // Gait while moving: Speed alone never left the walk animation, so a jogging player's body
            // walked and slid. The body's
            // third-person graph takes "Walk" / "Run" (its jog; "Jog" is the first-person graph's
            // name and is refused). Hysteresis on the smoothed speed.
            if (!st.moving || st.sprinting) {
                if (!st.moving) { st.gait = 0; st.gaitTries = 0; }
            } else {
                if (st.gait == 0) { st.gait = 1; st.gaitWant = 1; st.gaitWantSince = clockNow; }   // moveStart = the walk
                const int want = st.gait == 2 ? (st.rxSmooth > 180.0f ? 2 : 1) : (st.rxSmooth > 240.0f ? 2 : 1);
                // A change has to hold 0.3 s: a speed spike on starting flipped walk to jog and back.
                if (want == st.gait || want != st.gaitWant) { st.gaitWant = want; st.gaitWantSince = clockNow; }
                const bool held = clockNow - st.gaitWantSince > std::chrono::milliseconds(300);
                if (want != st.gait && held) {
                    // The weapon stance has its own names: holstered takes "Run"/"Walk", weapon out takes
                    // "runStart"/"walkStart". Whichever this stance accepts.
                    const bool drawn = actor->GetWeaponMagicDrawn();
                    auto send = [&](int g) {
                        const char* first = g == 2 ? (drawn ? "runStart" : "Run") : (drawn ? "walkStart" : "Walk");
                        const char* second = g == 2 ? (drawn ? "Run" : "runStart") : (drawn ? "Walk" : "walkStart");
                        return GraphOverride::SendEvent(actor, first) || GraphOverride::SendEvent(actor, second);
                    };
                    const char* ev = want == 2 ? "jog" : "walk";
                    bool ok = send(want);
                    // Refused: already in that gait (the graph refuses "Walk" while walking), or busy
                    // (just out of a sprint). Going through the other gait gets in either way; still
                    // refused = busy, retried next tick (taken as set only after ~1 s).
                    if (!ok && send(want == 2 ? 1 : 2)) ok = send(want);
                    const bool given = !ok && ++st.gaitTries >= 60;
                    if (ok || given) { st.gait = want; st.gaitTries = 0; }
                    static int s_logged = 0;
                    if ((ok || given) && s_logged++ < 60)
                        REX::INFO("[FathersFallout] body of player {}: {} -> {}", pid, ev, ok ? "accepted" : "refused for 1 s, taken as set");
                }
            }

            // Jump: variable every frame + an event on take-off. Several event names are tried and the one the
            // graph accepts (returns true) is kept.
            actor->SetGraphVariableBool("bInJumpState", rp.isJumping);
            actor->SetGraphVariableBool("IsJumping", rp.isJumping);
            if (st.jumping != rp.isJumping) {
                if (rp.isJumping) {
                    // jumpStart is the body graph's. As
                    // F4MP: sent raw, the hook took it for the engine resetting the graph and the walk was restarted
                    // all through the jump (refused every quarter second until the landing).
                    const bool ok = GraphOverride::SendEvent(actor, "jumpStart") || GraphOverride::SendEvent(actor, "JumpUp");
                    static int s_logged = 0;
                    if (s_logged++ < 20) REX::INFO("[FathersFallout] body of player {} jumps: {}", pid, ok);
                } else {
                    // Landed. Without this the body stayed in the jump's fall pose (arms up) for
                    // good and refused every later jump. Same events the player's own
                    // graph gets: jumpLandToRun/ToWalk while moving, else jumpLand.
                    const char* land = !wantMoving ? "jumpLand" : (wantSprint || st.smoothSpeed > 250.0f ? "jumpLandToRun" : "jumpLandToWalk");
                    bool ok = GraphOverride::SendEvent(actor, land);
                    if (!ok) ok = GraphOverride::SendEvent(actor, "jumpLand");
                    static int s_logged = 0;
                    if (s_logged++ < 20) REX::INFO("[FathersFallout] body of player {} landed: {} -> {}", pid, land, ok);
                    st.moving = false;      // the landing replaces the locomotion state: resend it
                    st.sprinting = false;
                }
                st.jumping = rp.isJumping;
            }

            // Not while crouched: the sneak walk/run clips have no footstep events.
            const bool senderStands = clockNow - st.lastRxMoveAt > std::chrono::milliseconds(800);
            switch (StepWatch(actor, st, st.moving, st.smoothSpeed, rp.isJumping || rp.isSneaking || Activity::BodyInPowerArmor(pid), senderStands)) {
            case 1: {
                static int s_logged = 0;
                if (s_logged++ < 40) REX::INFO("[StepWatch] body of player {}: moving ({:.0f}) but no footsteps for 1.5 s - walk/run sent again", pid, st.smoothSpeed);
                st.moving = false;      // resent below next tick, gait and sprint after it
                st.sprinting = false;
                st.gait = 0;
                break;
            }
            case 2: {
                const bool ok = GraphOverride::SendEvent(actor, "moveStop");
                st.moving = false;
                st.sprinting = false;
                static int s_logged = 0;
                if (s_logged++ < 40) REX::INFO("[StepWatch] body of player {}: walking on the spot while its player stands - moveStop -> {}", pid, ok);
                break;
            }
            default: break;
            }

            // Throttled log (1/s): how far the body is from its target. If 'desync' grows while the player
            // moves, the body falls behind or overshoots; a body walking the wrong way shows here too.
            static std::chrono::steady_clock::time_point lastLog{};
            if (std::chrono::duration<float>(clockNow - lastLog).count() > 10.0f) {
                lastLog = clockNow;
                // DIAGNOSTICS: the graph's Speed NOW (after setting it). ~0 means the engine overwrote it (it
                // doesn't see the body move). cc = whether the physics controller was found.
                float gSpeed = -1.0f;
                actor->GetGraphVariableImplFloat("Speed", gSpeed);
                float gDir = -99.0f;
                actor->GetGraphVariableImplFloat("Direction", gDir);
                const bool hasCC = actor->currentProcess && actor->currentProcess->middleHigh &&
                                   actor->currentProcess->middleHigh->charController.get();
                bool gSneak = false; actor->GetGraphVariableImplBool("IsSneaking", gSneak);
                REX::INFO("[FathersFallout] player {} | rxSpeed={:.0f} | Speed={:.1f} | snkRx={} snkBody={} snkGraph={} | jump={}",
                    pid, it->second.speed, gSpeed, rp.isSneaking, actor->IsSneaking(), gSneak, rp.isJumping);
            }
        }

        GraphOverride::KeepOnly(liveBodies);
        {
            std::lock_guard l(g_tagLock);
            g_tagBodyPos.clear();
            for (auto& [pid, fid] : g_bodies) {
                auto* f = RE::TESForm::GetFormByID(fid);
                if (auto* a = f ? f->As<RE::Actor>() : nullptr; a && a->Get3D()) g_tagBodyPos[pid] = a->GetPosition();
            }
        }

        // 2. Finish a pending spawn (placeatme is asynchronous).
        if (g_spawnPending) {
            const float waited = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_spawnAt).count();
            if (waited > 0.4f) {
                RE::Actor* actor = FindNearbyActor(g_spawnBase, ClaimedFormIds());
                // Not there yet: right after a load placeatme took longer
                // than 0.4 s. Giving up at once let SweepOrphanBodies delete the late body as a "leftover",
                // and the next spawn the same way: the friend's body flickered out after travelling.
                if (!actor && waited < 3.0f) return;
                if (!actor) REX::WARN("[FathersFallout] body for player {} didn't appear within 3 s; trying again", g_spawnFor);
                if (actor && RE::TESForm::GetFormByID(actor->GetFormID()) != actor) {
                    // The respawn check looks bodies up by FormID; if that can't find this actor
                    // it would be "lost" every tick. Refuse it loudly instead.
                    REX::WARN("[FathersFallout] spawned actor {:#x} not resolvable by FormID; not using it", actor->GetFormID());
                } else if (actor && remotes.find(g_spawnFor) != remotes.end()) {
                    g_bodies[g_spawnFor] = actor->GetFormID();
                    g_bodyBase[g_spawnFor] = g_spawnBase;
                    // CALM the body's AI: it's an NPC and would otherwise wander off on its own. The "do nothing"
                    // package stops it moving by itself; it still animates when we move it.
                    actor->InitiateDoNothingPackage();
                    actor->StopCombat();
                    // Never starts a fight, never runs or panics when hit (its player decides that).
                    if (auto* avs = RE::ActorValue::GetSingleton()) {
                        if (avs->aggression) actor->SetBaseActorValue(*avs->aggression, 0.0f);   // unaggressive
                        if (avs->confidence) actor->SetBaseActorValue(*avs->confidence, 4.0f);   // foolhardy: no fleeing
                        if (avs->assistance) actor->SetBaseActorValue(*avs->assistance, 0.0f);   // helps nobody
                    }
                    // Replayed shots/swings are for show: the real hit is the sender's own, already
                    // forwarded as damage. Zero damage stops the body's copy from counting twice.
                    if (auto* avs = RE::ActorValue::GetSingleton(); avs && avs->attackDamageMult)
                        actor->SetBaseActorValue(*avs->attackDamageMult, 0.0f);
                    // A body must never die (or fall into bleedout) on this screen: its player's
                    // health lives in their own game. Hits are still measured and forwarded to them.
                    if (auto* avs = RE::ActorValue::GetSingleton(); avs && avs->health)
                        actor->SetBaseActorValue(*avs->health, 100000.0f);
                    // Enemies must see a remote player like the local one: PlayerFaction makes
                    // everything hostile to the player hostile to this body too.
                    {
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "%X.addtofaction 1C21C 0", actor->GetFormID());
                        RE::Console::ExecuteCommand(cmd);
                    }
                    REX::INFO("[FathersFallout] body assigned to player {} (actor {:#x}), AI calmed, in PlayerFaction", g_spawnFor, actor->GetFormID());
                }
                g_spawnPending = false;
            }
            return;  // one spawn at a time
        }

        // 3. Start a spawn for the first remote player without a body.
        // Guard rails: at most one spawn every 3 s and 20 per session, so a body that keeps
        // "vanishing" can never flood the world with copies.
        {
            const auto t = std::chrono::steady_clock::now();
            while (!g_recentSpawns.empty() && t - g_recentSpawns.front() > std::chrono::minutes(5)) g_recentSpawns.pop_front();
            if (g_recentSpawns.size() >= 15) return;   // something keeps eating bodies: don't flood the world
        }
        if (std::chrono::steady_clock::now() - g_spawnAt < std::chrono::seconds(3)) return;
        // Not during or right after a loading screen: a body placed while the cell was still
        // coming in (the joiner's fast travel to the host) existed but was never drawn.
        {
            if (std::chrono::steady_clock::now() - g_lastLoading < std::chrono::seconds(2)) return;
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto* cell = pl ? pl->GetParentCell() : nullptr;
            if (!cell || !cell->IsAttached()) return;
        }
        for (auto& [pid, pos] : remotes) {
            if (g_bodies.find(pid) == g_bodies.end() && IsNearLocalPlayer(pos, Range::BodySpawn())) {
                ++g_spawnCount;
                g_recentSpawns.push_back(std::chrono::steady_clock::now());
                if (g_recentSpawns.size() == 15) REX::WARN("[FathersFallout] 15 body spawns in 5 minutes: pausing body spawns for a while");
                // The player's own character if we have their appearance, else a neutral body.
                g_spawnBase = Appearance::GetBodyBase(pid);
                if (!g_spawnBase) {
                    Network::PlayerAppearanceMsg ap{};
                    const bool female = net.GetAppearance(pid, ap) && ap.sex == 1;
                    g_spawnBase = female ? kBodyBaseFemale : kBodyBaseMale;
                }
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "player.placeatme %X 1", g_spawnBase);
                RE::Console::ExecuteCommand(cmd);
                g_spawnPending = true;
                g_spawnFor = pid;
                g_spawnAt = std::chrono::steady_clock::now();
                REX::INFO("[FathersFallout] spawning body for player {}", pid);
                break;
            }
        }
    }

    // ================= NPCs (host autoritativo) =================
    // FormIDs of NPCs this game has "taken over" (AI off, driven by the host's state).
    static std::unordered_set<uint32_t> g_drivenNpcs;
    struct NpcAnimState {   // keyed by local FormID
        bool moving = false; float smoothSpeed = 0.0f;
        bool init = false; float heading = 0.0f;
        bool sneaking = false; int gait = 0;   // 0 none, 1 walk, 2 run
        bool drawn = false; bool ownerDrawn = false; std::chrono::steady_clock::time_point drawAt{};
        bool prevMoving = false; long long movingSinceMs = 0, stillSinceMs = 0, lastKickMs = 0;   // footstep watchdog
        long long lastStep = 0, stillSteps = 0, stillStepsFrom = 0, quickUntilMs = 0;
        int idleKicks = 0;   // StepWatch walk kicks in a row that brought no footstep
        int moveRefused = 0;  // ticks moveStart was refused in a row
        int gaitTries = 0;    // ticks the gait event was refused in a row (~1 s: taken as set)
        bool channelsOff = false; bool bodyLike = false;   // Speed/Direction unbound (SetPosition mode): its own walk can't run
        bool aiOn = false; RE::NiPoint3 aiOffset{}; float aiAngle = 0.0f; std::chrono::steady_clock::time_point aiAt{}, aiMarkerAt{};
        bool haveSample = false; RE::NiPoint3 sample{}, vel{}; std::chrono::steady_clock::time_point sampleAt{};
    };
    static std::unordered_map<uint32_t, NpcAnimState> g_npcAnim;
    // Mirrored NPCs replay their owner's attacks (CoopNpcAnim) for show: the owner's game deals the
    // real damage, so a mirror hits for 0. Its own multiplier comes back when it's released.
    static std::unordered_map<uint32_t, float> g_npcDamageMult;   // local FormID -> original base value

    // Host: last broadcast position per NPC, to derive speed for client-side animation.
    struct NpcSample { RE::NiPoint3 pos; std::chrono::steady_clock::time_point at; };
    static std::unordered_map<uint32_t, NpcSample> g_npcSamples;

    // HOST: broadcasts the state of nearby placed NPCs, by FormID.
    static void BroadcastNpcs()
    {
        auto& net = Network::NetworkClient::GetInstance();
        if (!net.IsConnected() || Coop::IsLoadOrderMismatch()) return;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl) return;
        auto* tes = RE::TES::GetSingleton();
        if (!tes) return;
        const RE::NiPoint3 myPos = pl->GetPosition();
        const auto claimed = ClaimedFormIds();   // player bodies are NOT NPCs

        // Whole loaded area (not just the cell we stand in: enemies across a cell border, or a
        // corpse that ragdolled into the next cell, used to drop out of sync), nearest first.
        std::vector<std::pair<float, RE::TESObjectREFR*>> nearby;
        tes->ForEachReferenceInRange(myPos, Range::Loaded(), [&](RE::TESObjectREFR* ref) {
            if (ref && ref->As<RE::Actor>()) {
                const auto p = ref->GetPosition();
                const float dx = p.x - myPos.x, dy = p.y - myPos.y, dz = p.z - myPos.z;
                nearby.emplace_back(dx * dx + dy * dy + dz * dz, ref);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        std::sort(nearby.begin(), nearby.end(), [](auto& a, auto& b) { return a.first < b.first; });

        // Far NPCs update every 5th call (2/s) to keep bandwidth flat as the radius grows.
        static uint32_t s_tick = 0;
        const bool fullTick = (++s_tick % 5) == 0;
        int count = 0;
        for (auto& [d2, ref] : nearby) {
            if (count >= 128) break;                   // limite de NPCs
            if (!fullTick && d2 > Range::kNearNpc * Range::kNearNpc) continue;
            if (!ref || ref == static_cast<RE::TESObjectREFR*>(pl))
                continue;
            const uint32_t fid = ref->GetFormID();
            if (claimed.find(fid) != claimed.end())    // another player's body, not an NPC
                continue;
            auto* actor = ref->As<RE::Actor>();
            if (!actor)                                // actors only (NPCs/creatures)
                continue;
            // Disabled = not in our world (yet): story NPCs placed for later quests. Sending them made
            // a guest enable and pull them, which hung her game's load.
            if (actor->IsDisabled())
                continue;
            // Runtime actor: FormIDs differ per machine, so clients get a spawn announcement and
            // map our ID to their copy (sync/DynamicActors).
            // Our companion: ours wherever we go; the others place a copy of it (sync/DynamicActors).
            const bool companion = DynamicActors::IsOurCompanion(actor);
            if (companion) {
                DynamicActors::AnnounceCompanion(actor);
            } else if (fid >= DynamicActors::kRuntimeIdStart) {
                auto* base = ref->GetObjectReference();
                const uint32_t b = base ? base->GetFormID() : 0;
                if (b == kDummyBase || b == kBodyBaseMale || b == kBodyBaseFemale || b >= 0xFFF00000)
                    continue;
                DynamicActors::HostAnnounce(actor);
            }

            if (!companion && !Authority::IOwn(actor)) continue;    // another player's game runs this one
            Network::NpcStateMsg msg{};
            msg.formId = fid;
            msg.ownerId = net.GetPlayerId();
            const auto p = actor->GetPosition();
            msg.x = p.x; msg.y = p.y; msg.z = p.z;
            msg.angleZ = actor->GetHeading();
            const auto now = std::chrono::steady_clock::now();
            auto& sample = g_npcSamples[fid];
            const float dt = std::chrono::duration<float>(now - sample.at).count();
            if (dt > 0.01f && dt < 1.0f) {
                const float dx = p.x - sample.pos.x, dy = p.y - sample.pos.y;
                msg.speed = std::sqrt(dx * dx + dy * dy) / dt;
            }
            sample = { p, now };
            msg.isMoving = msg.speed > 10.0f;
            msg.isSneaking = actor->IsSneaking();
            msg.isDead = actor->IsDead(false);
            msg.flags = (actor->GetWeaponMagicDrawn() ? Network::kNpcWeaponDrawn : 0) |
                        (msg.speed > 260.0f ? Network::kNpcRunning : 0) |   // NPC runs are ~300-450, walks ~80-140
                        (companion ? Network::kNpcCompanion : 0);
            msg.health = Coop::GetHealth(actor);
            net.SendPacket(Network::MessageType::NpcState, &msg, sizeof(msg));
            count++;
        }
    }

    // Mirror the NPCs other players own (sync/Authority): find our copy, calm its AI and drive it
    // to the owner's state. NPCs we own run normally and are broadcast by BroadcastNpcs.

    static void ReleaseDriven(uint32_t localFid)
    {
        // Give the NPC its own AI back (we had it on a do-nothing package).
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "%X.evp", localFid);
        RE::Console::ExecuteCommand(cmd);
        GraphOverride::SetMirror(localFid, false);
        g_npcAnim.erase(localFid);
        // Back on its own AI with Speed unbound: Speed from how it really moves (else it slid wherever it walked).
        if (auto* f = RE::TESForm::GetFormByID(localFid); auto* a = f ? f->As<RE::Actor>() : nullptr) {
            GraphOverride::ReleaseMirrorSpeed(a);
            FreeMarker(localFid);
        }
        if (auto it = g_npcDamageMult.find(localFid); it != g_npcDamageMult.end()) {
            auto* f = RE::TESForm::GetFormByID(localFid);
            auto* a = f ? f->As<RE::Actor>() : nullptr;
            if (auto* avs = RE::ActorValue::GetSingleton(); a && avs && avs->attackDamageMult)
                a->SetBaseActorValue(*avs->attackDamageMult, it->second);
            g_npcDamageMult.erase(it);
        }
    }

    // ---- The host's world, NPCs: a guest sees the host's people where the host has them. ----
    // An NPC the host has near us but that our world keeps elsewhere (Preston still in Concord in a
    // newer save), disabled (not met yet) or dead is brought here; from then on NpcsSync drives it.
    // This changes the guest's own save: while playing as a guest, the host's world is the world.
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_lastPull;
    static std::unordered_map<uint32_t, int> g_pullTries;   // pulls in a row that left it without 3D

    // Whether the host is in the same interior cell / worldspace as us. Interiors have their own
    // coordinates, so distances between players in different spaces mean nothing.
    static bool HostInMySpace()
    {
        const auto players = Network::NetworkClient::GetInstance().GetRemotePlayers();
        auto host = players.find(Authority::HostId());
        return host != players.end() && host->second.cellId == Authority::LocalSpace();
    }

    static void PullHostNpc(RE::Actor* actor, const Network::NpcStateMsg& s)
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pl || g_isHost || !HostInMySpace()) return;
        const auto here = pl->GetPosition();
        const float dx = s.x - here.x, dy = s.y - here.y;
        if (dx * dx + dy * dy > Range::Loaded() * Range::Loaded() * 0.64f) return;   // not near us
        const auto now = std::chrono::steady_clock::now();
        auto& last = g_lastPull[actor->GetFormID()];
        if (now - last < std::chrono::seconds(10)) return;
        last = now;
        // A few NPCs (bodies the host had dragged) never got their 3D here and were moved
        // again every 10 s for the whole session, sliding and floating on the guest's screen.
        int& tries = g_pullTries[actor->GetFormID()];
        if (tries >= 3) return;
        if (++tries == 3)
            REX::WARN("[World] host's NPC {:#x} still not shown after 3 tries: left where our world has it", actor->GetFormID());
        char cmd[64];
        const bool wasDisabled = actor->IsDisabled();
        if (wasDisabled) actor->Enable(false);
        const bool wasDead = actor->IsDead(false) && !s.isDead;
        if (wasDead) actor->Resurrect(false, true);
        std::snprintf(cmd, sizeof(cmd), "%X.moveto player", actor->GetFormID());
        RE::Console::ExecuteCommand(cmd);
        actor->SetPosition({ s.x, s.y, s.z }, true);
        REX::INFO("[World] host's NPC {:#x} brought here ({}{}was elsewhere in our world)", actor->GetFormID(),
            wasDisabled ? "enabled, " : "", wasDead ? "revived, " : "");
    }

    // Our own NPCs around us that the host's world doesn't have here (our Codsworth when the host's
    // is elsewhere, people our story put here): hidden while connected (shown again on disconnect
    // and on every load). Only while the host stands close enough to be sending this whole area.
    static void HideNpcsTheHostLacks()
    {
        auto& net = Network::NetworkClient::GetInstance();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* tes = RE::TES::GetSingleton();
        if (g_isHost || !pl || !tes || !net.IsConnected() || Coop::IsLoadOrderMismatch()) return;
        static std::chrono::steady_clock::time_point s_next{}, s_since{};
        static std::unordered_map<uint32_t, int> s_missing;   // consecutive sweeps not sent by the host
        const auto now = std::chrono::steady_clock::now();
        if (now < s_next) return;
        s_next = now + std::chrono::seconds(2);

        const auto players = net.GetRemotePlayers();
        auto host = players.find(Authority::HostId());
        const auto here = pl->GetPosition();
        const auto npcs = net.GetRemoteNpcs();
        if (host == players.end() || npcs.empty() || host->second.cellId != Authority::LocalSpace()) {
            s_since = now; s_missing.clear(); return;
        }
        const float hx = host->second.x - here.x, hy = host->second.y - here.y;
        if (hx * hx + hy * hy > 2500.0f * 2500.0f) { s_since = now; s_missing.clear(); return; }
        if (now - s_since < std::chrono::seconds(8)) return;   // the host's stream has to settle first

        // The host's companion comes as a copy: our own Piper standing here isn't the host's (in the host's
        // world she's at the host's side), so it doesn't count as sent.
        std::unordered_set<uint32_t> sent;
        for (auto& [key, st] : npcs)
            if (!(st.flags & Network::kNpcCompanion)) sent.insert(static_cast<uint32_t>(key));
        const auto claimed = ClaimedFormIds();
        const float radius = Range::Loaded() * 0.5f;   // well inside what the host sends (nearest 128)
        std::vector<RE::Actor*> ghosts;
        tes->ForEachReferenceInRange(here, radius, [&](RE::TESObjectREFR* ref) {
            auto* a = ref ? ref->As<RE::Actor>() : nullptr;
            const uint32_t fid = ref ? ref->GetFormID() : 0;
            if (!a || a == pl || fid >= DynamicActors::kRuntimeIdStart || claimed.contains(fid) || a->IsDisabled() || !a->Get3D())
                return RE::BSContainer::ForEachResult::kContinue;
            if (sent.contains(fid)) { s_missing.erase(fid); return RE::BSContainer::ForEachResult::kContinue; }
            // Our own companion (Dogmeat, Piper...) came with us: the host's world has them elsewhere, but
            // hiding them made them vanish the moment we got near the host.
            static auto* s_companions = RE::TESForm::GetFormByID(0x00023C01) ? RE::TESForm::GetFormByID(0x00023C01)->As<RE::TESFaction>() : nullptr;
            if (s_companions && a->IsInFaction(s_companions)) return RE::BSContainer::ForEachResult::kContinue;
            if (++s_missing[fid] >= 2) ghosts.push_back(a);
            return RE::BSContainer::ForEachResult::kContinue;
        });
        for (auto* a : ghosts) {
            Workshop::HideRef(a);
            s_missing.erase(a->GetFormID());
            REX::INFO("[World] our NPC {:#x} isn't here in the host's world: hidden while connected", a->GetFormID());
        }
    }

    // Mirrored NPCs walked by their own AI: Actor.PathToReference to a marker kept where the owner's copy stands,
    // so the game walks and runs them with their own animations. Placed every
    // frame instead (SetPosition), vanilla NPCs' legs played a slow walk whatever Speed we gave them (testing, the
    // user watching Preston and Dogmeat beside a body that stepped right). Bridge f4mp:npcai 0/1 switches
    // (g_npcAiMove, declared with the bridge).

    static void NpcsSync()
    {
        auto& net = Network::NetworkClient::GetInstance();
        if (!net.IsConnected() || Coop::IsLoadOrderMismatch()) {
            for (auto& [fid, owner] : g_drivenOwner) ReleaseDriven(fid);
            g_drivenOwner.clear();
            GraphOverride::KeepMirrorSpeeds({});   // released ones checked against their actors even now
            return;
        }

        if (!TravelBusy()) HideNpcsTheHostLacks();
        const uint32_t me = net.GetPlayerId();
        auto npcs = net.GetRemoteNpcs();
        std::unordered_set<uint32_t> seen;
        std::vector<RE::Actor*> mirroredNow;
        int aiCalls = 0;   // PathToReference calls this tick (Papyrus queue: a crowd mustn't flood it)
        auto* myChar = RE::PlayerCharacter::GetSingleton();

        for (auto& [key, s] : npcs) {
            const uint32_t owner = static_cast<uint32_t>(key >> 32);
            const uint32_t fid = static_cast<uint32_t>(key);
            if (owner == me) continue;
            // Their companion: only ever our copy of it (never the same NPC in our world, who may be ours).
            const bool companion = (s.flags & Network::kNpcCompanion) != 0;
            RE::Actor* actor = companion ? DynamicActors::Resolve(owner, fid) : DynamicActors::ResolveNpc(owner, fid);
            // The same NPC is OUR companion here (both players took Dogmeat): ours runs with us, theirs is a copy.
            if (actor && DynamicActors::IsOurCompanion(actor)) {
                if (g_drivenOwner.erase(actor->GetFormID())) ReleaseDriven(actor->GetFormID());
                continue;
            }
            if (actor && !actor->Get3D() && !companion && fid < DynamicActors::kRuntimeIdStart && owner == Authority::HostId() && !TravelBusy())
                PullHostNpc(actor, s);                        // elsewhere (or gone) in our world
            else if (actor && actor->Get3D())
                g_pullTries.erase(actor->GetFormID());
            {
                static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_why;
                auto& w = s_why[fid];
                // Every 30 s per NPC filled shared logs; the first 80 tell the story.
                static int s_lines = 0;
                if (s_lines < 80 && std::chrono::steady_clock::now() - w > std::chrono::seconds(30) && (!actor || !actor->Get3D() || Authority::IOwn(actor))) {
                    ++s_lines;
                    w = std::chrono::steady_clock::now();
                    REX::INFO("[World] host NPC {:#x} from player {}: actor {} 3D {} ours {} disabled {}", fid, owner, actor != nullptr,
                        actor && actor->Get3D(), actor && Authority::IOwn(actor), actor && actor->IsDisabled());
                }
            }
            if (!actor || !actor->Get3D()) continue;          // not loaded here
            if (Authority::IOwn(actor)) continue;             // our area now: it runs on its own AI

            const uint32_t local = actor->GetFormID();
            seen.insert(local);
            if (!g_drivenOwner.contains(local)) {
                actor->InitiateDoNothingPackage();
                actor->StopCombat();
                if (auto* avs = RE::ActorValue::GetSingleton(); avs && avs->attackDamageMult && !g_npcDamageMult.contains(local)) {
                    g_npcDamageMult[local] = actor->GetBaseActorValue(*avs->attackDamageMult);
                    actor->SetBaseActorValue(*avs->attackDamageMult, 0.0f);
                }
                GraphOverride::SetMirror(local, true);   // idles only as its owner's copy plays them
                REX::INFO("[FathersFallout] NPC {:#x} mirrored from player {}", local, owner);
            }
            g_drivenOwner[local] = owner;
            // Its own AI must never fight: it went for a body or the player on sight (testing: a mirrored radroach
            // bit the bot's body nonstop; : a copy shot the guest). The owner's attacks are replayed.
            if (actor->IsInCombat()) {
                actor->StopCombat();
                actor->InitiateDoNothingPackage();
                static int s_calmed = 0;
                if (s_calmed++ < 20) REX::INFO("[FathersFallout] mirrored NPC {:#x} went into combat on its own: stopped", local);
            }

            // Dead here but alive in its owner's game (the museum raiders were long dead in
            // the guest's save, yet the host's shot at him while she saw corpses). Not right away: our
            // own killing blow is on its way to the owner and its death comes back a moment later.
            {
                static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_deadSince, s_revived;
                const auto now = std::chrono::steady_clock::now();
                if (actor->IsDead(false) && !s.isDead && s.health > 0.0f) {
                    auto [d, fresh] = s_deadSince.try_emplace(local, now);
                    if (!fresh && now - d->second > std::chrono::seconds(3) && now - s_revived[local] > std::chrono::seconds(15)) {
                        s_revived[local] = now;
                        s_deadSince.erase(local);
                        actor->Resurrect(false, true);
                        actor->InitiateDoNothingPackage();   // a mirror: its owner's game runs it
                        REX::INFO("[World] NPC {:#x} is alive in player {}'s game: revived here", local, owner);
                    }
                } else {
                    s_deadSince.erase(local);
                }
            }
            if (!(g_mirrorOff & 1)) Coop::ApplyNpcVitals(actor, owner, fid, s.health, s.isDead);
            GraphOverride::SetMirror(local, !(g_mirrorOff & 4));
            if (s.isDead || actor->IsDead(false)) continue;   // corpses ragdoll locally
            if (Activity::MirrorSeated(local)) continue;      // on its owner's seat: the furniture places and animates it

            // Drive position (smoothly interpolated) + heading, like player bodies. NPC states come ~10/s (bodies
            // every frame): chasing each new spot moved them in surges, gliding and stopping between packets
            //. Between packets the goal runs on at the owner's measured velocity (a fifth of a
            // second at most, height as sent).
            auto& anim = g_npcAnim[local];
            const auto tnow = std::chrono::steady_clock::now();
            {
                const RE::NiPoint3 at{ s.x, s.y, s.z };
                const RE::NiPoint3 d = at - anim.sample;
                if (!anim.haveSample || d.x * d.x + d.y * d.y + d.z * d.z > 1.0f) {
                    const float dt = std::chrono::duration<float>(tnow - anim.sampleAt).count();
                    if (anim.haveSample && dt > 0.03f && dt < 0.6f)
                        anim.vel = RE::NiPoint3{ anim.vel.x * 0.5f + d.x / dt * 0.5f, anim.vel.y * 0.5f + d.y / dt * 0.5f, 0.0f };
                    else
                        anim.vel = {};
                    anim.sample = at;
                    anim.sampleAt = tnow;
                    anim.haveSample = true;
                } else if (tnow - anim.sampleAt > std::chrono::milliseconds(300)) {
                    anim.vel = {};   // no new spot for a while: it stands
                }
                if (s.speed < 10.0f) anim.vel = {};
            }
            const float lead = (std::min)(std::chrono::duration<float>(tnow - anim.sampleAt).count(), 0.2f);
            const RE::NiPoint3 goal{ s.x + anim.vel.x * lead, s.y + anim.vel.y * lead, s.z };
            const RE::NiPoint3 bodyPos = actor->GetPosition();
            const RE::NiPoint3 delta = goal - bodyPos;
            const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            RE::TESObjectREFR* marker = g_npcAiMove && myChar ? MarkerFor(local) : nullptr;
            if (marker) {
                if (anim.channelsOff) {   // switched over live: its graph needs its own Speed back
                    anim.channelsOff = false;
                    GraphOverride::ClearMirrorSpeed(actor);
                    actor->Reset3D(false, 0, true, 0);
                }
                if (dist > 600.0f) actor->SetPosition(goal, true);   // too far behind (blocked, a jump): on the spot
                const bool ownerMoves = s.speed > 10.0f;
                const bool run = (s.flags & Network::kNpcRunning) || s.speed > 150.0f;
                // The marker follows the owner's copy (~3/s); the walk follows the marker. A new PathToReference
                // cancels the one under way: asked every 0.35 s they only turned on the spot, re-planning.
                const RE::NiPoint3 moved = goal - anim.aiOffset;
                if (aiCalls < 8 && tnow - anim.aiMarkerAt > std::chrono::milliseconds(300) &&
                    (!anim.aiOn || moved.x * moved.x + moved.y * moved.y > 24.0f * 24.0f)) {
                    ++aiCalls;
                    anim.aiMarkerAt = tnow;
                    anim.aiOffset = goal;
                    char cmd[64];
                    std::snprintf(cmd, sizeof(cmd), "%X.setpos x %.1f", marker->GetFormID(), goal.x);
                    RE::Console::ExecuteCommand(cmd);
                    std::snprintf(cmd, sizeof(cmd), "%X.setpos y %.1f", marker->GetFormID(), goal.y);
                    RE::Console::ExecuteCommand(cmd);
                    std::snprintf(cmd, sizeof(cmd), "%X.setpos z %.1f", marker->GetFormID(), goal.z);
                    RE::Console::ExecuteCommand(cmd);
                }
                const bool gaitChanged = run != (anim.aiAngle > 0.5f);
                const bool farOff = dist > 60.0f;
                if (!anim.aiOn || gaitChanged || (farOff && tnow - anim.aiAt > std::chrono::seconds(4))) {
                    if (aiCalls < 8 && (ownerMoves || farOff || !anim.aiOn)) {
                        ++aiCalls;
                        // Off the do-nothing package first: on it, the walk order only turned it toward the marker.
                        if (!anim.aiOn) PapyrusOnActor(actor, "EvaluatePackage");
                        anim.aiOn = true;
                        anim.aiAngle = run ? 1.0f : 0.0f;
                        anim.aiAt = tnow;
                        const bool sent = PapyrusOnActor(actor, "PathToReference", marker, run ? 1.0f : 0.0f);
                        static int s_logged = 0;
                        if (s_logged++ < 12)
                            REX::INFO("[FathersFallout] mirrored NPC {:#x}: walks to its owner's spot ({}, {:.0f} off) -> {}", local, run ? "run" : "walk", dist, sent);
                    }
                }
                // Standing where its owner's copy stands: face the same way.
                if (!ownerMoves && dist < 60.0f) {
                    if (!anim.init) { anim.init = true; anim.heading = s.angleZ; }
                    float diff = s.angleZ - anim.heading;
                    while (diff > 3.14159265f) diff -= 6.2831853f;
                    while (diff < -3.14159265f) diff += 6.2831853f;
                    anim.heading += std::fabs(diff) < 0.02f ? diff : diff * 0.2f;
                    actor->SetHeading(anim.heading);
                } else {
                    anim.heading = actor->GetHeading();
                }
            } else if (g_npcAiMove && myChar) {
                // Its marker is on its way: leave its graph alone (cutting Speed here left its own walk without one).
                if (dist > 600.0f) actor->SetPosition(goal, true);
            } else {
            if (anim.aiOn) { anim.aiOn = false; FreeMarker(local); actor->InitiateDoNothingPackage(); }
            if (dist > 1500.0f) {
                actor->SetPosition(goal, true);
            } else {
                const float alpha = 0.35f;
                actor->SetPosition(RE::NiPoint3{ bodyPos.x + delta.x * alpha,
                                                 bodyPos.y + delta.y * alpha,
                                                 bodyPos.z + delta.z * alpha }, true);
            }
            // Turn toward the owner's heading over a few ticks (a snap every update looked like twitching).
            if (!anim.init) { anim.init = true; anim.heading = s.angleZ; }
            {
                float diff = s.angleZ - anim.heading;
                while (diff > 3.14159265f) diff -= 6.2831853f;
                while (diff < -3.14159265f) diff += 6.2831853f;
                anim.heading += std::fabs(diff) < 0.02f ? diff : diff * 0.4f;
                actor->SetHeading(anim.heading);
            }

            // Animation: same bIsSynced trick as player bodies, so the graph honors Speed.
            anim.smoothSpeed = anim.smoothSpeed * 0.8f + s.speed * 0.2f;
            actor->SetGraphVariableBool("bIsSynced", true);
            actor->SetGraphVariableFloat("Speed", anim.smoothSpeed);
            // Its own AI's wish to turn (we set the heading) kept it stepping round on the spot; walking is forward.
            if (g_mirrorOff & 0x10) {   // test: exactly as bodies (Speed/Direction only; turning left to the engine)
                if (!anim.bodyLike) { anim.bodyLike = true; actor->Reset3D(false, 0, true, 0); }
                GraphOverride::DetachMovementChannels(actor);
            } else {
                GraphOverride::DetachMirrorChannels(actor);
                actor->SetGraphVariableFloat("TurnDelta", 0.0f);
            }
            anim.channelsOff = true;
            GraphOverride::SetMirrorSpeed(actor, anim.smoothSpeed * g_npcSpeedMult);
            mirroredNow.push_back(actor);
            const bool wantMoving = anim.moving ? (anim.smoothSpeed > 8.0f) : (anim.smoothSpeed > 30.0f);
            if (wantMoving != anim.moving) {
                // Once, as the game does for its own walk. The graph answers "refused" even when it starts walking,
                // and resending every tick restarted the walk from its first frame 30 times a second: the slow-motion
                // NPCs.
                if (wantMoving) StartMovingNpc(actor, anim.sneaking);
                else GraphOverride::SendEvent(actor, "moveStop");
                static int s_logged = 0;
                if (s_logged++ < 60)
                    REX::INFO("[FathersFallout] mirrored NPC {:#x}: {} at {:.0f}", local, wantMoving ? "moves" : "stops", anim.smoothSpeed);
                anim.moving = wantMoving;
                anim.gait = 1;   // walkStart is the walk; the run goes out below when it runs
            }
            // Some NPC walks step only every ~3 s: the
            // 1.5 s watchdog restarted his walk each time, a hitch every few seconds.
            switch (StepWatch(actor, anim, anim.moving, anim.smoothSpeed, anim.sneaking, s.speed < 10.0f && anim.smoothSpeed < 15.0f, 4000)) {
            case 1: {
                static int s_logged = 0;
                if (s_logged++ < 40) REX::INFO("[StepWatch] mirrored NPC {:#x}: moving ({:.0f}) but no footsteps for 4 s - walk sent again", local, anim.smoothSpeed);
                anim.moving = false;   // moveStart again next tick
                anim.gait = 0;
                break;
            }
            case 2: {
                const bool ok = GraphOverride::SendEvent(actor, "moveStop");
                static int s_logged = 0;
                if (s_logged++ < 40) REX::INFO("[StepWatch] mirrored NPC {:#x}: walking on the spot - moveStop -> {}", local, ok);
                break;
            }
            default: break;
            }
            }   // SetPosition mode
            // Crouched like its owner's copy (sent since the start, never applied).
            if (!(g_mirrorOff & 8) && anim.sneaking != s.isSneaking) {
                actor->SetSneaking(s.isSneaking);
                GraphOverride::SendEvent(actor, s.isSneaking ? "SneakStart" : "SneakStop");
                anim.sneaking = s.isSneaking;
            }
            // Running, not walking fast: the walk animation slid at a run's speed. Same event names as bodies.
            // NPCs walk at ~75; moved faster than ~140 their walk legs plainly
            // trailed the ground, so that is already the run.
            if (anim.moving) {
                const bool run = (s.flags & Network::kNpcRunning) || anim.smoothSpeed > (anim.gait == 2 ? 110.0f : 140.0f);
                const int want = run ? 2 : 1;
                if (want != anim.gait) {
                    const bool drawn = actor->GetWeaponMagicDrawn();
                    const char* a = want == 2 ? (drawn ? "runStart" : "Run") : (drawn ? "walkStart" : "Walk");
                    const char* b = want == 2 ? (drawn ? "Run" : "runStart") : (drawn ? "Walk" : "walkStart");
                    // Once per change (resent every tick it restarted the gait, the slow-motion legs): the first name the
                    // graph knows, the other only if that one was refused.
                    if (!GraphOverride::SendEvent(actor, a)) GraphOverride::SendEvent(actor, b);
                    static int s_logged = 0;
                    if (s_logged++ < 60) REX::INFO("[FathersFallout] mirrored NPC {:#x}: {} at {:.0f}", local, want == 2 ? "run" : "walk", anim.smoothSpeed);
                    anim.gait = want;
                }
            }
            // Weapon out or away like the owner's copy (its AI is on a do-nothing package and kept whatever
            // it had: raiders with holstered guns shooting, settlers with guns out). Twice a second at most.
            if (!(g_mirrorOff & 2)) {
                const bool want = (s.flags & Network::kNpcWeaponDrawn) != 0;
                const auto t = std::chrono::steady_clock::now();
                GraphOverride::SetMirrorDrawn(local, want);
                if (want != anim.ownerDrawn) {
                    anim.ownerDrawn = want;
                    static int s_logged = 0;
                    if (s_logged++ < 40) REX::INFO("[FathersFallout] mirrored NPC {:#x}: owner's copy has its weapon {} (flags {:#x}, ours drawn {} state {})", local,
                        want ? "out" : "away", s.flags, actor->GetWeaponMagicDrawn(), static_cast<int>(actor->weaponState) & 7);
                }
                const bool kept = want && KeepDrawn(actor);
                if (kept) {
                    static int s_kept = 0;
                    if (s_kept++ < 20) REX::INFO("[FathersFallout] mirrored NPC {:#x}: its AI started holstering - kept drawn", local);
                }
                if (Coop::TakeNpcRedraw(local) && want) {
                    anim.drawAt = t;
                    anim.quickUntilMs = NowMs() + 3000;
                    actor->DrawWeaponMagicHands(true);
                } else if (!kept && (WeaponStateOf(actor) >= 3) != want && t - anim.drawAt > std::chrono::milliseconds(1500)) {
                    // Asked before and still not there: stuck half-way ("want to draw" that nothing picks up,
                    // as bodies did; testing: a mirrored Preston never drew). Reset the state and use the graph's
                    // own draw/holster event, then ask again.
                    const int ws = WeaponStateOf(actor);
                    if (anim.drawAt != std::chrono::steady_clock::time_point{} && ws != 0 && ws != 3) {
                        actor->SetWeaponState(want ? RE::WEAPON_STATE::kSheathed : RE::WEAPON_STATE::kDrawn);
                        GraphOverride::SendEvent(actor, want ? "WeapEquip" : "Unequip");
                        static int s_stuck = 0;
                        if (s_stuck++ < 20)
                            REX::INFO("[FathersFallout] mirrored NPC {:#x}: weapon stuck in state {} - reset, {} again", local,
                                ws, want ? "drawing" : "holstering");
                    }
                    anim.drawAt = t;
                    anim.quickUntilMs = NowMs() + 3000;
                    actor->DrawWeaponMagicHands(want);
                    static int s_draws = 0;
                    if (s_draws++ < 30)
                        REX::INFO("[FathersFallout] mirrored NPC {:#x}: weapon {} (state {} -> {}, drawn now {})", local, want ? "drawn" : "away",
                            ws, WeaponStateOf(actor), WeaponStateOf(actor) >= 3);
                }
            }
        }

        GraphOverride::KeepMirrorSpeeds(mirroredNow);
        // No longer mirrored (owner stopped sending it, or the area is ours now): free its AI.
        for (auto it = g_drivenOwner.begin(); it != g_drivenOwner.end();) {
            if (!seen.contains(it->first)) { ReleaseDriven(it->first); it = g_drivenOwner.erase(it); }
            else ++it;
        }
    }

    static void SpawnDummy()
    {
        // 1. Spawnear via consola (funciona de forma fiable).
        REX::INFO("[FathersFallout] spawn: placeatme");
        RE::Console::ExecuteCommand("player.placeatme 7 1");

        // 2. The spawn is asynchronous: wait a little, then on the main thread find the actor and move
        //    it aside (spawn + find + move test).
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            F4SE::GetTaskInterface()->AddTask([]() {
                RE::Actor* actor = FindNearbyActor(kDummyBase, ClaimedFormIds());
                if (!actor) { REX::WARN("[FathersFallout] spawn: spawned actor not found"); return; }
                g_dummies.insert(actor->GetFormID());

                auto* pl = RE::PlayerCharacter::GetSingleton();
                RE::NiPoint3 target = pl ? pl->GetPosition() : RE::NiPoint3{};
                target.x += 300.0f;  // moverlo 300 unidades a un lado
                actor->SetPosition(target, true);
                REX::INFO("[FathersFallout] spawn: actor {:#x} wasFound y movido", actor->GetFormID());
            });
        }).detach();
    }

    // ---- "Go to host": each player plays their own save, so joiners start wherever their save is.
    // Travel uses the game's own commands: coc <cell> (interior) / cow <worldspace> x y (exterior),
    // then setpos. The setpos task is queued through F4SE, which holds tasks until the new area has
    // finished loading.
    static bool g_travelOnJoin = true;          // FathersFallout.ini [Connect] TravelOnJoin
    static std::atomic<bool> g_travelDone{ false };
    static std::mutex g_travelLock;
    static bool g_travelPending = false;
    static Network::PlayerPositionMsg g_travelTarget{};
    static std::chrono::steady_clock::time_point g_travelStarted{};

    // The host's-world changes (pulling/hiding NPCs) wait until a guest has arrived and the area has
    // settled: moving NPCs out of unloaded cells while a trip's load starts hung a guest's game.
    static bool TravelBusy()
    {
        if (g_isHost) return false;
        {
            std::lock_guard l(g_travelLock);
            if (g_travelPending) return true;
        }
        if (g_travelOnJoin && !g_travelDone) return true;
        return !SettledFor(5);
    }

    static bool GoToHost(bool a_quiet = false)
    {
        auto& net = Network::NetworkClient::GetInstance();
        const uint32_t hostId = Authority::HostId();
        auto remotes = net.GetRemotePlayers();
        auto it = remotes.find(hostId);
        if (!net.IsConnected() || g_isHost || it == remotes.end()) {
            if (!a_quiet) Notify("FathersFallout: the host's position isn't known yet");
            return false;
        }
        const auto target = it->second;
        auto* space = RE::TESForm::GetFormByID(target.cellId);
        std::string travel;
        if (auto* cell = space ? space->As<RE::TESObjectCELL>() : nullptr) {
            const char* id = cell->GetFormEditorID();
            if (id && *id) travel = std::string("coc ") + id;
        } else if (auto* ws = space ? space->As<RE::TESWorldSpace>() : nullptr) {
            const char* id = ws->editorID.c_str();
            if (id && *id) {
                const int cx = static_cast<int>(std::floor(target.x / 4096.0f));
                const int cy = static_cast<int>(std::floor(target.y / 4096.0f));
                travel = std::string("cow ") + id + " " + std::to_string(cx) + " " + std::to_string(cy);
            }
        }
        // Already in the host's space and close by: no loading screen needed.
        // Same space and close by: just step over. Far away in the same worldspace: still use
        // cow (a loading screen) - a setpos across the map hung a joiner's game.
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool sameSpace = player && Authority::LocalSpace() == target.cellId;
        float distance = 0.0f;
        if (player) {
            const auto me = player->GetPosition();
            distance = std::sqrt((me.x - target.x) * (me.x - target.x) + (me.y - target.y) * (me.y - target.y));
        }
        if (sameSpace && (distance < 8000.0f || travel.rfind("coc ", 0) == 0 || travel.empty())) travel.clear();
        else if (sameSpace && a_quiet) {
            // Joining in the same worldspace but far away: no automatic trip. The game's cow is a
            // black screen that took 80-110 s in testing and over 5 minutes for a guest.
            Notify(std::format("FathersFallout: the host is {:.0f} m away - walk over, or use Go to host in the menu",
                distance * 0.0142875f));
            REX::INFO("[FathersFallout] joined {:.0f} units from the host in the same worldspace: no automatic trip", distance);
            return false;
        } else if (travel.empty()) {
            Notify("FathersFallout: can't travel to the host's location (unknown place)");
            REX::WARN("[FathersFallout] go to host: no editor id for space {:#x}", target.cellId);
            return false;
        }
        REX::INFO("[FathersFallout] go to host {} at {:.0f} {:.0f} {:.0f} (space {:#x}): {}", hostId, target.x, target.y, target.z,
            target.cellId, travel.empty() ? "same space" : travel);
        Notify(travel.empty() ? "FathersFallout: travelling to the host..."
                              : "FathersFallout: travelling to the host - the black screen can take a minute or two");
        {
            std::lock_guard l(g_travelLock);
            g_travelTarget = target;
            g_travelPending = true;
            g_travelStarted = std::chrono::steady_clock::now();
        }
        if (!travel.empty())
            F4SE::GetTaskInterface()->AddTask([travel]() { RE::Console::ExecuteCommand(travel.c_str()); });
        return true;
    }

    // Second half of the trip: once we're in the host's space and nothing is loading, step next to
    // the host (not inside them). Polled from the network thread.
    static void FinishTravel()
    {
        Network::PlayerPositionMsg target{};
        {
            std::lock_guard l(g_travelLock);
            if (!g_travelPending) return;
            if (std::chrono::steady_clock::now() - g_travelStarted > std::chrono::seconds(90)) { g_travelPending = false; return; }
            if (std::chrono::steady_clock::now() - g_travelStarted < std::chrono::milliseconds(1500)) return;
            target = g_travelTarget;
        }
        if (Authority::LocalSpaceAtomic() != target.cellId) return;
        g_travelPending = false;
        F4SE::GetTaskInterface()->AddTask([target]() {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->GetMenuOpen("LoadingMenu")) {   // still loading: try again next poll
                std::lock_guard l(g_travelLock);
                g_travelPending = true;
                return;
            }
            // Only a short step: the space matched before a same-worldspace trip's load even began (a door
            // taken while joining, testing), and a setpos across the map hangs the game on a black screen.
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                const auto me = pl->GetPosition();
                const float dx = me.x - target.x, dy = me.y - target.y;
                if (dx * dx + dy * dy > 12000.0f * 12000.0f) {
                    std::lock_guard l(g_travelLock);
                    g_travelPending = true;   // the trip's own load hasn't landed us yet (gives up after 90 s)
                    return;
                }
            }
            const float x = target.x + std::sin(target.angleZ + 1.57f) * 120.0f;
            const float y = target.y + std::cos(target.angleZ + 1.57f) * 120.0f;
            const std::string cmds[] = {
                "player.setpos x " + std::to_string(x),
                "player.setpos y " + std::to_string(y),
                "player.setpos z " + std::to_string(target.z + 20.0f),
            };
            for (auto& c : cmds) RE::Console::ExecuteCommand(c.c_str());
            REX::INFO("[FathersFallout] arrived next to the host ({:.0f} {:.0f} {:.0f})", x, y, target.z);
            Notify("FathersFallout: you're next to the host");
        });
    }

    // Joiners: travel once per connection, as soon as the host's position arrives.
    static void MaybeTravelOnJoin()
    {
        FinishTravel();
        auto& net = Network::NetworkClient::GetInstance();
        if (!net.IsConnected()) { g_travelDone = false; return; }
        if (g_isHost || !g_travelOnJoin || g_travelDone || g_inIntro) return;
        auto remotes = net.GetRemotePlayers();
        auto host = remotes.find(Authority::HostId());
        if (host == remotes.end() || host->second.cellId == 0) return;   // where the host is isn't known yet
        if (!SettledFor(5)) return;   // let the save finish loading before starting another load
        // Mid-load (a door just taken) we're nowhere: a trip decided now picked the wrong kind.
        if (Authority::LocalSpaceAtomic() == 0) return;
        g_travelDone = true;
        GoToHost(true);
    }

    // ---- Co-op characters: cached list + "create a new character" through the game's editor ----
    static std::vector<Appearance::Character> g_characters;
    static bool g_charactersDirty = true;
    static void RefreshCharacters()
    {
        g_characters = Appearance::ListCharacters();
        g_charactersDirty = false;
    }

    // When the character editor (LooksMenu) closes after "Create a new character", save the result.
    class LooksMenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event.opening && a_event.menuName == "LooksMenu" && g_creatingCharacter.exchange(false)) {
                F4SE::GetTaskInterface()->AddTask([]() {
                    const auto file = Appearance::SaveCurrentAsCharacter(g_newCharacterName);
                    if (!file.empty() && Appearance::SelectCharacter(file)) {
                        Notify("FathersFallout: created " + Appearance::MyLookName() + " - you now play as this character");
                    } else {
                        Notify("FathersFallout: could not save the new character (see FathersFallout.log)");
                    }
                    g_charactersDirty = true;
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    static LooksMenuWatcher g_looksWatcher;

    // Remember connection choices in FathersFallout.ini so the next session connects by itself.
    static void SaveConnectIni(bool a_autoConnect)
    {
        const std::string ini = IniPath();
        WritePrivateProfileStringA("Connect", "AutoConnect", a_autoConnect ? "1" : "0", ini.c_str());
        WritePrivateProfileStringA("Connect", "Server", g_serverAddr, ini.c_str());
        WritePrivateProfileStringA("Connect", "Port", std::to_string(g_serverPort).c_str(), ini.c_str());
        WritePrivateProfileStringA("Connect", "Name", g_playerName, ini.c_str());
        WritePrivateProfileStringA("Connect", "Host", g_isHost ? "1" : "0", ini.c_str());
        WritePrivateProfileStringA("Connect", "Password", g_password, ini.c_str());
    }

    // Host without ServerExe set: look for the server in the usual places.
    static void FindServerExe()
    {
        if (!g_serverExe.empty()) return;
        const std::string plugins = IniPath().substr(0, IniPath().find_last_of("\\/"));
        const std::string candidates[] = {
            plugins + "\\server\\FathersFalloutServer.exe",
            plugins + "\\F4MP-server\\FathersFalloutServer.exe",
            plugins + "\\F4MP-server\\F4MPServer.exe",
            plugins + "\\..\\..\\..\\F4MP-server\\F4MPServer.exe",   // game folder\F4MP-server (old versions)
        };
        for (auto& c : candidates) {
            if (GetFileAttributesA(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_serverExe = c;
                WritePrivateProfileStringA("Connect", "ServerExe", c.c_str(), IniPath().c_str());
                return;
            }
        }
    }

    // ---- Menu page (drawn by the menu framework) ----
    static void RenderPlayTogether()
    {
        auto& net = Network::NetworkClient::GetInstance();
        ImGuiMCP::SeparatorText("Play together");

        if (net.IsConnected()) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Connected to %s as %s (%s)",
                net.GetTarget().c_str(), g_playerName, g_isHost ? "host" : "joined");
            auto remotes = net.GetRemotePlayers();
            ImGuiMCP::Text("Other players here: %d", (int)remotes.size());
            if (!g_isHost) {
                if (ImGuiMCP::Button("Go to host")) GoToHost();
                ImGuiMCP::SameLine();
                // Day here, night there: the host's time of day and weather right now.
                if (ImGuiMCP::Button("Sync time with host"))
                    Notify(Coop::SyncTimeWithHost() ? "FathersFallout: time of day synced with the host"
                                                    : "FathersFallout: the host hasn't sent its time yet - try again in a moment");
                ImGuiMCP::SameLine();
            }
            if (ImGuiMCP::Button("Disconnect")) net.Disconnect();
            return;
        }
        if (net.IsConnecting()) {
            ImGuiMCP::Text("Connecting to %s...", net.GetTarget().c_str());
            if (ImGuiMCP::Button("Cancel")) {
                g_retriesLeft = 0;
                net.Disconnect();
            }
            return;
        }

        const std::string err = net.GetLastError();
        if (!err.empty()) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Could not connect to %s: %s",
                net.GetTarget().c_str(), err.c_str());
            ImGuiMCP::TextWrapped("%s", ConnectHint().c_str());
        }

        static int s_mode = -1;   // 0 = join, 1 = host
        if (s_mode < 0) s_mode = g_isHost ? 1 : 0;
        ImGuiMCP::RadioButton("Join a friend", &s_mode, 0);
        ImGuiMCP::SameLine();
        ImGuiMCP::RadioButton("Host (others join me)", &s_mode, 1);
        ImGuiMCP::InputText("Your name", g_playerName, sizeof(g_playerName));

        static bool s_auto = true;
        if (s_mode == 0) {
            if (IsLocalAddress(g_serverAddr)) g_serverAddr[0] = '\0';
            static bool s_viaSteam = IsSteamAddress(g_serverAddr) || (g_serverAddr[0] == '\0' && Network::SteamLink::Available());
            static char s_steamWho[64] = "";
            static bool s_steamInit = false;
            if (!s_steamInit) {
                s_steamInit = true;
                if (IsSteamAddress(g_serverAddr) && g_serverAddr[5] == ':') strncpy_s(s_steamWho, g_serverAddr + 6, _TRUNCATE);
            }
            ImGuiMCP::BeginDisabled(!Network::SteamLink::Available());
            ImGuiMCP::Checkbox("Join through Steam (no IP needed)", &s_viaSteam);
            ImGuiMCP::EndDisabled();
            if (!Network::SteamLink::Available()) {
                s_viaSteam = false;
                ImGuiMCP::TextDisabled("Steam isn't running for this copy of the game (GOG?): join by IP.");
            }
            if (s_viaSteam) {
                ImGuiMCP::InputTextWithHint("Host (optional)", "friend code or Steam name - empty = whoever is hosting", s_steamWho, sizeof(s_steamWho));
                if (s_steamWho[0]) std::snprintf(g_serverAddr, sizeof(g_serverAddr), "steam:%s", s_steamWho);
                else std::snprintf(g_serverAddr, sizeof(g_serverAddr), "steam");
            } else {
                if (IsSteamAddress(g_serverAddr)) g_serverAddr[0] = '\0';
                ImGuiMCP::InputTextWithHint("Host's IP", "e.g. 203.0.113.25", g_serverAddr, sizeof(g_serverAddr));
                ImGuiMCP::InputInt("Port", &g_serverPort);
            }
            ImGuiMCP::InputTextWithHint("Password", "only if the host set one", g_password, sizeof(g_password), ImGuiMCP::ImGuiInputTextFlags_Password);
            ImGuiMCP::Checkbox("Join automatically next time a save loads", &s_auto);
            if (ImGuiMCP::Checkbox("Travel to the host when I join", &g_travelOnJoin))
                WritePrivateProfileStringA("Connect", "TravelOnJoin", g_travelOnJoin ? "1" : "0", IniPath().c_str());
            ImGuiMCP::BeginDisabled(g_serverAddr[0] == '\0');
            if (ImGuiMCP::Button("Join")) {
                g_isHost = false;
                SaveConnectIni(s_auto);
                RefreshInIntro();
                ConnectNow(2);
            }
            ImGuiMCP::EndDisabled();
            ImGuiMCP::TextDisabled("Load YOUR OWN save (your character, level and gear). Use a copy for co-op:");
            ImGuiMCP::TextDisabled("joining catches your quests up to the host's progress.");
        } else {
            FindServerExe();
            ImGuiMCP::InputInt("Port", &g_serverPort);
            ImGuiMCP::InputTextWithHint("Password", "empty = anyone who finds your game can join", g_password, sizeof(g_password), ImGuiMCP::ImGuiInputTextFlags_Password);
            ImGuiMCP::TextDisabled("Friends type the same password when they join. Don't use your Steam or email password.");
            ImGuiMCP::Checkbox("Host automatically next time a save loads", &s_auto);
            if (ImGuiMCP::Button("Host a game")) {
                g_isHost = true;
                g_serverRestarted = false;
                std::snprintf(g_serverAddr, sizeof(g_serverAddr), "127.0.0.1");
                SaveConnectIni(s_auto);
                RefreshInIntro();
                ConnectNow(2);
            }
            if (g_serverExe.empty())
                ImGuiMCP::TextWrapped("Start FathersFalloutServer.exe first (or set ServerExe= in FathersFallout.ini to have it started for you).");
            else
                ImGuiMCP::TextDisabled("Starts the server for you: %s", g_serverExe.c_str());
            if (Network::SteamLink::Available()) {
                if (ImGuiMCP::Checkbox("Let Steam friends join (no IP or port forwarding)", &g_steamHosting))
                    WritePrivateProfileStringA("Connect", "Steam", g_steamHosting ? "1" : "0", IniPath().c_str());
                bool friendsOnly = Network::SteamLink::FriendsOnly();
                if (ImGuiMCP::Checkbox("Steam: only people on my friends list", &friendsOnly)) {
                    Network::SteamLink::SetFriendsOnly(friendsOnly);
                    WritePrivateProfileStringA("Connect", "SteamFriendsOnly", friendsOnly ? "1" : "0", IniPath().c_str());
                }
                ImGuiMCP::TextDisabled("Your friend code: %llu  (%s)", Network::SteamLink::MySteamId() & 0xFFFFFFFFull, Network::SteamLink::MyName().c_str());
                if (Network::SteamLink::IsHosting())
                    ImGuiMCP::TextDisabled("Hosting through Steam: %d friend(s) joined that way.", Network::SteamLink::RelayedPlayers());
            }
            ImGuiMCP::TextDisabled("Friends joining by IP (GOG) need your public IP and UDP port %d forwarded to this PC.", g_serverPort);
        }
    }

    static void RenderCharacters()
    {
        ImGuiMCP::SeparatorText("Your co-op character");
        if (g_charactersDirty) RefreshCharacters();
        ImGuiMCP::TextWrapped("Others see the character you pick here, whatever character the shared save gives you.");

        bool anySelected = false;
        for (auto& c : g_characters) anySelected = anySelected || c.selected;
        if (ImGuiMCP::RadioButton("The character I'm playing right now", !anySelected)) {
            Appearance::SelectCharacter({});
            g_charactersDirty = true;
        }
        std::string toDelete;
        for (auto& c : g_characters) {
            ImGuiMCP::PushID(c.file.c_str());
            const std::string label = c.name + (c.female ? "  (female)" : "  (male)");
            if (ImGuiMCP::RadioButton(label.c_str(), c.selected)) {
                Appearance::SelectCharacter(c.file);
                Notify("FathersFallout: you now play as " + c.name);
                g_charactersDirty = true;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::SmallButton("Delete")) toDelete = c.file;
            ImGuiMCP::PopID();
        }
        if (!toDelete.empty()) {
            Appearance::DeleteCharacter(toDelete);
            g_charactersDirty = true;
        }

        if (ImGuiMCP::Button("Save the character I'm playing")) {
            F4SE::GetTaskInterface()->AddTask([]() {
                if (Appearance::SaveMyLook()) Notify("FathersFallout: saved " + Appearance::MyLookName() + " - you now play as this character");
                else Notify("FathersFallout: could not save this character (see FathersFallout.log)");
                g_charactersDirty = true;
            });
        }
        ImGuiMCP::TextDisabled("Tip: load one of YOUR OWN saves, press this once, then join with any save.");

        static char s_newName[64] = "";
        ImGuiMCP::InputTextWithHint("New character's name", "e.g. Nora", s_newName, sizeof(s_newName));
        ImGuiMCP::BeginDisabled(s_newName[0] == '\0');
        if (ImGuiMCP::Button("Create a new character")) {
            g_newCharacterName = s_newName;
            g_creatingCharacter = true;
            F4SE::GetTaskInterface()->AddTask([]() { RE::Console::ExecuteCommand("showlooksmenu player 1"); });
            Notify("FathersFallout: close this menu (F1) and make your character; it is saved when you finish");
        }
        ImGuiMCP::EndDisabled();
        ImGuiMCP::TextDisabled("Opens the character editor. It changes the face of the character you are playing,");
        ImGuiMCP::TextDisabled("so don't save over your own save afterwards if you don't want that.");
    }

    static void __stdcall RenderMenu()
    {
        auto& net = Network::NetworkClient::GetInstance();
        ImGuiMCP::TextDisabled("FathersFallout by FatherLamb - Copyright (C) 2026 FatherLamb");
        RenderPlayTogether();
        RenderCharacters();

        ImGuiMCP::SeparatorText("Sync");
        ImGuiMCP::Text("FPS: %.0f (capped at %d for sync)", FrameLimiter::GetMeasuredFps(), FrameLimiter::kTargetFps);
        auto& cs = Coop::GetSettings();
        ImGuiMCP::Checkbox("Equipment", &cs.equipment);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox("Combat damage", &cs.combat);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox("Items", &cs.items);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox("Quests", &cs.quests);
        ImGuiMCP::Checkbox("When I join, catch my quests up to the host's (changes my save)", &cs.questCatchUp);
        ImGuiMCP::Checkbox("Send my own quest progress to the host (changes the host's save)", &cs.shareMyQuests);
        ImGuiMCP::Checkbox("Time of day + weather (follow the host)", &cs.worldTime);
        ImGuiMCP::Checkbox("Show other players' attacks, aiming and reloads", &cs.combatAnims);
        ImGuiMCP::Checkbox("Friendly fire (other players' shots and swings can hurt me)", &cs.friendlyFire);
        if (Coop::IsLoadOrderMismatch())
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "LOAD ORDER DIFFERS FROM THE HOST - item/quest/NPC sync paused (see FathersFallout.log)");
        else if (Coop::MismatchedPlayers() > 0)
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "%d player(s) have a different load order - their item/quest/NPC sync is ignored",
                (int)Coop::MismatchedPlayers());

        if (ImGuiMCP::CollapsingHeader("Advanced")) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                const auto pos = player->GetPosition();
                ImGuiMCP::Text("Your position:  x=%.0f  y=%.0f  z=%.0f", pos.x, pos.y, pos.z);
            }
            const auto& st = Coop::GetStats();
            ImGuiMCP::Text("Co-op: sent %u, applied %u | quests %u tracked, %u stages applied",
                st.sent, st.applied, st.trackedQuests, st.questStagesApplied);
            ImGuiMCP::Text("       containers %u ops | damage forwarded %u", st.containerOps, st.damageForwarded);
            ImGuiMCP::Checkbox("Clients: hide own random spawns (host owns spawns)", &DynamicActors::SuppressLocalSpawns());
            ImGuiMCP::Text("       runtime actors mirrored %u | local-only hidden %u",
                DynamicActors::MappedCount(), DynamicActors::SuppressedCount());
            if (net.IsConnected()) {
                for (const auto& [id, p] : net.GetRemotePlayers())
                    ImGuiMCP::Text("  player #%u  x=%.0f y=%.0f z=%.0f", id, p.x, p.y, p.z);
                ImGuiMCP::Text("Role: %s", g_isHost ? "host (my game runs the shared world)" : "guest (the host's world)");
                if (!g_isHost) ImGuiMCP::Text("NPCs mirrored: %d", (int)net.GetRemoteNpcs().size());
                // Test tools: two hosts in one session break it, so players never see these.
                if (g_consoleBridge || g_bodyTrace) {
                    if (ImGuiMCP::Button(g_isHost ? "Stop being host (test)" : "Become NPC host (test)")) g_isHost = !g_isHost;
                    ImGuiMCP::Text("Visibility: %s", g_hideSelf ? "HIDDEN (dedicated host)" : "visible");
                    if (ImGuiMCP::Button(g_hideSelf ? "Show me (normal player)" : "Hide me (dedicated host, test)")) g_hideSelf = !g_hideSelf;
                }
            }
            if ((g_consoleBridge || g_bodyTrace) && ImGuiMCP::Button("Spawn dummy (test)")) {
                F4SE::GetTaskInterface()->AddTask([]() { SpawnDummy(); });
            }
        }
    }

    // ---- HUD: a floating tag over each remote player (world -> screen) ----
    static void __stdcall RenderHud()
    {
        auto& net = Network::NetworkClient::GetInstance();
        if (!net.IsConnected()) return;

        auto* local = RE::PlayerCharacter::GetSingleton();
        if (!local) return;
        const RE::NiPoint3 myPos = local->GetPosition();

        // Not over a loading screen, the F1 menu
        // (tags were drawn over its text) or the game's full-screen menus.
        if (F4SEMenuFramework::IsAnyBlockingWindowOpened()) return;
        if (auto* ui = RE::UI::GetSingleton()) {
            static constexpr const char* kCovering[] = { "LoadingMenu", "PipboyMenu", "PauseMenu", "MainMenu", "LooksMenu",
                "ContainerMenu", "BarterMenu", "TerminalMenu", "LevelUpMenu", "SPECIALMenu", "CookingMenu", "ExamineMenu",
                "Console", "BookMenu" };
            for (const char* m : kCovering)
                if (ui->GetMenuOpen(m)) return;
        }

        auto* io = ImGuiMCP::GetIO();
        if (!io) return;
        const float W = io->DisplaySize.x;
        const float H = io->DisplaySize.y;

        ImGuiMCP::ImDrawList* dl = ImGuiMCP::GetForegroundDrawList();
        if (!dl) return;

        // Colours are ABGR. Pip-Boy green for names; the bar goes yellow at half, red at a quarter.
        constexpr ImGuiMCP::ImU32 kName = 0xFF80FF1Au, kDim = 0xB080FF1Au, kShadow = 0xC0000000u;
        constexpr ImGuiMCP::ImU32 kBarBack = 0xA0000000u, kGreen = 0xFF40E040u, kYellow = 0xFF00D2FFu, kRed = 0xFF3C3CFFu;
        using ImGuiMCP::ImVec2;
        namespace DL = ImGuiMCP::ImDrawListManager;
        auto textWidth = [](const char* t) { ImVec2 sz{}; ImGuiMCP::CalcTextSize(&sz, t, nullptr, false, -1.0f); return sz.x; };
        auto shadowed = [&](float x, float y, ImGuiMCP::ImU32 c, const char* t) {
            DL::AddText(dl, ImVec2(x + 1.0f, y + 1.0f), kShadow, t);
            DL::AddText(dl, ImVec2(x, y), c, t);
        };

        auto remotes = net.GetRemotePlayers();
        const uint32_t mySpace = Authority::LocalSpaceAtomic();
        std::unordered_map<uint32_t, RE::NiPoint3> bodyAt;
        {
            std::lock_guard l(g_tagLock);
            bodyAt = g_tagBodyPos;
        }
        for (const auto& [id, p] : remotes) {
            // Another interior / worldspace: its coordinates mean nothing here (the tag floated in a
            // random spot of ours).
            if (p.cellId && mySpace && p.cellId != mySpace) continue;
            // Over the body as drawn (it trails its player's packets a little), else where they are.
            RE::NiPoint3 at{ p.x, p.y, p.z };
            if (auto b = bodyAt.find(id); b != bodyAt.end()) at = b->second;
            // Anchor a little above the head.
            RE::NiPoint3 anchorS = RE::HUDMenuUtils::WorldPtToScreenPt3(RE::NiPoint3{ at.x, at.y, at.z + 135.0f });
            if (anchorS.z <= 0.0f) continue;  // behind the camera
            if (anchorS.x < -0.1f || anchorS.x > 1.1f || anchorS.y < -0.1f || anchorS.y > 1.1f) continue;
            const float ax = anchorS.x * W, ay = (1.0f - anchorS.y) * H;

            const float dx = at.x - myPos.x, dy = at.y - myPos.y, dz = at.z - myPos.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.0142875f;   // game units -> metres

            std::string name = Appearance::NameOf(id);
            if (name.empty()) name = "Player " + std::to_string(id);
            float health = 0, maxHealth = 0;
            bool dead = false;
            const bool haveVitals = Coop::GetPlayerVitals(id, health, maxHealth, dead);

            // Bottom-up: distance, health bar, name - spaced by the font's real height (resolution).
            ImVec2 lineSz{};
            ImGuiMCP::CalcTextSize(&lineSz, "Ag", nullptr, false, -1.0f);
            const float lineH = lineSz.y > 0.0f ? lineSz.y : 16.0f, gap = lineH * 0.2f;
            char distText[32];
            snprintf(distText, sizeof(distText), "%.0f m", dist);
            float y = ay - lineH;
            shadowed(ax - textWidth(distText) * 0.5f, y, kDim, distText);
            if (haveVitals && maxHealth > 0.0f) {
                const float frac = dead ? 0.0f : std::clamp(health / maxHealth, 0.0f, 1.0f);
                const float bw = lineH * 5.0f, bh = (std::max)(4.0f, lineH * 0.3f), bx = ax - bw * 0.5f, by = y - gap - bh;
                DL::AddRectFilled(dl, ImVec2(bx - 1.0f, by - 1.0f), ImVec2(bx + bw + 1.0f, by + bh + 1.0f), kBarBack, 2.0f, 0);
                if (frac > 0.0f)
                    DL::AddRectFilled(dl, ImVec2(bx, by), ImVec2(bx + bw * frac, by + bh), frac > 0.5f ? kGreen : frac > 0.25f ? kYellow : kRed, 2.0f, 0);
                y = by - gap;
            }
            if (dead) name += "  (down)";
            y -= lineH;
            shadowed(ax - textWidth(name.c_str()) * 0.5f, y, kName, name.c_str());
        }
    }

    // Runs once per rendered frame (HUD pass): enforce the 60 FPS cap.
    static void __stdcall OnFrameHud() { FrameLimiter::OnFrame(); }

    static void RegisterMenu()
    {
        // Called on both kPostLoad and kPostPostLoad. Registered twice, the limiter ran twice per
        // frame and held the game at 30 FPS while its own counter (time between calls) said 60.
        static bool s_registered = false;
        if (s_registered) return;
        s_registered = true;
        if (!F4SEMenuFramework::IsInstalled()) {
            REX::INFO("[FathersFallout] F4SE Menu Framework NOT installed: no menu");
            return;
        }
        F4SEMenuFramework::AddHudElement(OnFrameHud);
        F4SEMenuFramework::SetSection("FathersFallout");
        F4SEMenuFramework::AddSectionItem("Menu", RenderMenu);
        F4SEMenuFramework::AddHudElement(RenderHud);
        REX::INFO("[FathersFallout] menu + HUD registered");
    }

    // Optional Data\F4SE\Plugins\FathersFallout.ini:
    //   [Connect]
    //   AutoConnect=1        connect as soon as a save finishes loading
    //   Server=127.0.0.1
    //   Port=7779
    //   Name=Wastelander
    //   Host=1               act as NPC/quest host
    static void AutoConnectFromIni()
    {
        static bool s_done = false;
        if (s_done) return;

        const std::string iniPath = IniPath();
        const char* ini = iniPath.c_str();
        static bool s_readOnce = false;
        if (!s_readOnce) {  // used by the menu's Conectar button too
            s_readOnce = true;
            char exe[MAX_PATH]{};
            GetPrivateProfileStringA("Connect", "ServerExe", "", exe, sizeof(exe), ini);
            g_serverExe = exe;
            g_isHost = GetPrivateProfileIntA("Connect", "Host", 0, ini) != 0;
            g_steamHosting = GetPrivateProfileIntA("Connect", "Steam", 1, ini) != 0;
            GetPrivateProfileStringA("Connect", "Password", "", g_password, sizeof(g_password), ini);
            Network::SteamLink::SetFriendsOnly(GetPrivateProfileIntA("Connect", "SteamFriendsOnly", 1, ini) != 0);
            g_travelOnJoin = GetPrivateProfileIntA("Connect", "TravelOnJoin", 1, ini) != 0;
            Coop::GetSettings().friendlyFire = GetPrivateProfileIntA("Sync", "FriendlyFire", 1, ini) != 0;
        }
        if (!GetPrivateProfileIntA("Connect", "AutoConnect", 0, ini)) return;
        if (RefreshInIntro()) { NotifyIntro(); REX::INFO("[FathersFallout] auto-connect skipped: this save is in the pre-war intro"); return; }
        s_done = true;

        GetPrivateProfileStringA("Connect", "Server", g_serverAddr, g_serverAddr, sizeof(g_serverAddr), ini);
        GetPrivateProfileStringA("Connect", "Name", g_playerName, g_playerName, sizeof(g_playerName), ini);
        g_serverPort = static_cast<int>(GetPrivateProfileIntA("Connect", "Port", g_serverPort, ini));
        g_isHost = GetPrivateProfileIntA("Connect", "Host", 0, ini) != 0;

        ConnectNow(5);  // ~1 min of attempts: covers starting the game before the server
        REX::INFO("[FathersFallout] auto-connect to {}:{} as '{}' ({})", g_serverAddr, g_serverPort, g_playerName,
            g_isHost ? "host" : "client");
    }

    static void OnMessage(F4SE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) return;

        if (a_msg->type == F4SE::MessagingInterface::kPreSaveGame) {
            DespawnBodiesForSave();
        }

        // Saves only: a New Game starts in the pre-war intro, and travelling to the host from there
        // would break it. New players join once they've left Vault 111 and saved.
        if (a_msg->type == F4SE::MessagingInterface::kPostLoadGame) {
            F4MP::CrashLog::Reassert();
            F4MP::Workshop::OnGameLoaded();
            F4MP::Coop::OnGameLoaded();
            AutoConnectFromIni();
        }

        if (a_msg->type == F4SE::MessagingInterface::kPostLoad ||
            a_msg->type == F4SE::MessagingInterface::kPostPostLoad) {
            RegisterMenu();
        }

        if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
            F4MP::CrashLog::Reassert();
            // Keep simulating while the window is unfocused. A host that alt-tabs would
            // otherwise freeze the shared world and queue every incoming sync message.
            auto* alwaysActive = RE::GetINISetting("bAlwaysActive:General");
            if (alwaysActive && alwaysActive->GetType() == RE::Setting::SETTING_TYPE::kBinary) {
                alwaysActive->SetBinary(true);
                REX::INFO("[FathersFallout] bAlwaysActive forced on (game keeps running when unfocused)");
            } else {
                REX::WARN("[FathersFallout] bAlwaysActive:General not found; game will pause when unfocused");
            }

            if (!g_netThreadStarted.exchange(true)) {
                Network::NetworkClient::GetInstance().Initialize();
                // testing: [Debug] SteamLoopback=1 relays our own Steam account to a server on this PC, so this game
                // can join it as a guest with Server=steam:<own friend code> (the guest's Steam path, one PC).
                if (GetPrivateProfileIntA("Debug", "SteamLoopback", 0, IniPath().c_str()) != 0)
                    Network::SteamLink::StartHosting(Network::NetworkClient::GetInstance().Sockets(),
                        static_cast<uint16_t>(GetPrivateProfileIntA("Connect", "Port", 7779, IniPath().c_str())), "loopback");
                Coop::RegisterSinks();
                // Bodies' shots leave the way their players aimed (turned before the bullet's ray is cast).
                AimHook::Install();
                g_consoleBridge = GetPrivateProfileIntA("Debug", "ConsoleBridge", 0, IniPath().c_str()) != 0;
                g_bodyTrace = GetPrivateProfileIntA("Debug", "BodyTrace", 0, IniPath().c_str()) != 0;
                Coop::GetSettings().hitTrace = g_bodyTrace || GetPrivateProfileIntA("Debug", "HitTrace", 0, IniPath().c_str()) != 0;
                AimHook::SetTrace(GetPrivateProfileIntA("Debug", "AimTrace", 0, IniPath().c_str()) != 0);
                PlayerEvents::SetLogAll(g_bodyTrace);
                PlayerEvents::Install();
                if (auto* ui = RE::UI::GetSingleton()) ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_looksWatcher);
                if (g_consoleBridge) REX::WARN("[FathersFallout] console bridge ON (debug/testing only)");
                LoadSaveFromLauncher();
                std::thread(NetThread).detach();
                REX::INFO("[FathersFallout] network ready, network thread running");
            }
        }
    }
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4MP::CrashLog::KeepPreviousLog();
    F4SE::Init(a_f4se);
    REX::INFO("[FathersFallout] FathersFallout by FatherLamb - Copyright (C) 2026 FatherLamb (GPL v3 + NOTICE)");
    REX::INFO("[FathersFallout] plugin loaded (CommonLibF4)");
    F4MP::CrashLog::Install();

    F4SE::GetMessagingInterface()->RegisterListener(F4MP::OnMessage);
    F4MP::Workshop::RegisterSerialization();
    return true;
}
