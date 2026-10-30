// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "network/SteamLink.h"
#include "network/NetworkClient.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Network::SteamLink
{
    namespace
    {
        constexpr uint32_t kFallout4AppId = 377160;
        constexpr int kVirtualPort = 7779;
        constexpr const char* kPresenceKey = "ffcoop";       // "host" while hosting a co-op game
        constexpr int kFriendFlagImmediate = 0x04;           // EFriendFlags::k_EFriendFlagImmediate
        constexpr int kSendReliable = 8;                     // k_nSteamNetworkingSend_Reliable

        // Steam's flat C API, from the game's own steam_api64.dll (no Steamworks SDK needed). The
        // structs are GameNetworkingSockets' (same layout: it is the open-source build of this API).
        using GetIface_t = void* (*)();
        using GetSteamID_t = uint64_t (*)(void*);
        using InitRelay_t = void (*)(void*);
        using ListenP2P_t = HSteamListenSocket (*)(void*, int, int, const SteamNetworkingConfigValue_t*);
        using ConnectP2P_t = HSteamNetConnection (*)(void*, const SteamNetworkingIdentity&, int, int, const SteamNetworkingConfigValue_t*);
        using Accept_t = int (*)(void*, HSteamNetConnection);
        using CloseConn_t = bool (*)(void*, HSteamNetConnection, int, const char*, bool);
        using CloseListen_t = bool (*)(void*, HSteamListenSocket);
        using SendMsg_t = int (*)(void*, HSteamNetConnection, const void*, uint32_t, int, int64_t*);
        using RecvMsg_t = int (*)(void*, HSteamNetConnection, SteamNetworkingMessage_t**, int);
        using ConnInfo_t = bool (*)(void*, HSteamNetConnection, SteamNetConnectionInfo_t*);
        using RunCb_t = void (*)(void*);
        using FriendCount_t = int (*)(void*, int);
        using FriendByIndex_t = uint64_t (*)(void*, int, int);
        using FriendGame_t = bool (*)(void*, uint64_t, void*);
        using FriendPresence_t = const char* (*)(void*, uint64_t, const char*);
        using FriendName_t = const char* (*)(void*, uint64_t);
        using MyName_t = const char* (*)(void*);
        using SetPresence_t = bool (*)(void*, const char*, const char*);
        using ClearPresence_t = void (*)(void*);
        using ReqPresence_t = void (*)(void*, uint64_t);
        using HasFriend_t = bool (*)(void*, uint64_t, int);

        struct Api
        {
            void* sockets = nullptr;
            void* utils = nullptr;
            void* friends = nullptr;
            void* user = nullptr;
            GetSteamID_t getSteamID = nullptr;
            InitRelay_t initRelay = nullptr;
            ListenP2P_t listenP2P = nullptr;
            ConnectP2P_t connectP2P = nullptr;
            Accept_t accept = nullptr;
            CloseConn_t closeConn = nullptr;
            CloseListen_t closeListen = nullptr;
            SendMsg_t send = nullptr;
            RecvMsg_t recv = nullptr;
            ConnInfo_t info = nullptr;
            RunCb_t runCallbacks = nullptr;
            FriendCount_t friendCount = nullptr;
            FriendByIndex_t friendByIndex = nullptr;
            FriendGame_t friendGame = nullptr;
            FriendPresence_t friendPresence = nullptr;
            FriendName_t friendName = nullptr;
            MyName_t myName = nullptr;
            SetPresence_t setPresence = nullptr;
            ClearPresence_t clearPresence = nullptr;
            ReqPresence_t reqPresence = nullptr;
            HasFriend_t hasFriend = nullptr;
        };
        Api g_api;
        bool g_ready = false;

        // ---- "Join Game" in the Steam friends list / accepted Steam invites ----
        // The host's rich presence "connect" makes Steam offer Join Game (and the overlay's Invite to Game). A click
        // reaches the friend's running game as GameRichPresenceJoinRequested_t; a game Steam starts for it gets the
        // same text on its command line. Same declaration order as Steam's CCallbackBase: the vtable must match.
        class CCallbackBase
        {
        public:
            virtual void Run(void* a_param) = 0;
            virtual void Run(void* a_param, bool a_ioFailure, uint64_t a_call) = 0;
            virtual int GetCallbackSizeBytes() = 0;
            uint8_t m_nCallbackFlags = 0;
            int m_iCallback = 0;
        };
        constexpr int kJoinRequested = 337;   // k_iSteamFriendsCallbacks + 37: GameRichPresenceJoinRequested_t
        struct JoinRequested { uint64_t friendId; char connect[256]; };
        std::atomic<uint64_t> g_joinRequest{ 0 };

        uint64_t ParseJoin(const char* a_text)
        {
            const char* p = a_text ? std::strstr(a_text, "+ffjoin ") : nullptr;
            return p ? std::strtoull(p + 8, nullptr, 10) : 0;
        }

        class JoinCallback : public CCallbackBase
        {
        public:
            JoinCallback() { m_iCallback = kJoinRequested; }
            void Run(void* a_param) override
            {
                const auto* r = static_cast<const JoinRequested*>(a_param);
                const uint64_t host = r ? ParseJoin(r->connect) : 0;
                REX::INFO("[Steam] Join Game from the friends list: '{}' -> host {}", r ? r->connect : "", host);
                if (host) g_joinRequest = host;
            }
            void Run(void* a_param, bool, uint64_t) override { Run(a_param); }
            int GetCallbackSizeBytes() override { return static_cast<int>(sizeof(JoinRequested)); }
        };
        JoinCallback g_joinCallback;
        std::once_flag g_initOnce;

        template <class T>
        bool Load(HMODULE a_dll, const char* a_name, T& a_out)
        {
            a_out = reinterpret_cast<T>(GetProcAddress(a_dll, a_name));
            if (!a_out) REX::WARN("[Steam] {} missing in steam_api64.dll", a_name);
            return a_out != nullptr;
        }

        // ---- host relay: one Steam connection <-> one local connection to the co-op server ----
        struct Pair
        {
            HSteamNetConnection steam = k_HSteamNetConnection_Invalid;
            HSteamNetConnection local = k_HSteamNetConnection_Invalid;
            uint64_t steamId = 0;
            std::string name;
            std::deque<std::vector<uint8_t>> toLocal;   // held until the local connection is up
            bool localUp = false;
        };
        std::mutex g_lock;
        std::vector<Pair> g_pairs;
        HSteamListenSocket g_listen = k_HSteamListenSocket_Invalid;
        ISteamNetworkingSockets* g_gns = nullptr;
        uint16_t g_serverPort = 0;
        std::atomic<bool> g_hosting{ false };
        std::atomic<bool> g_relayThread{ false };
        std::atomic<bool> g_friendsOnly{ true };   // [Connect] SteamFriendsOnly (default on)
        constexpr size_t kMaxRelayed = 16;
        // Wrong passwords per Steam account (the server sees every Steam guest as 127.0.0.1, so it
        // can't tell them apart): 5 in a minute and that account waits a minute.
        struct BadTries { int count = 0; std::chrono::steady_clock::time_point first, lockedUntil; };
        std::unordered_map<uint64_t, BadTries> g_badTries;

        std::string NameOf(uint64_t a_id)
        {
            const char* n = g_api.friendName ? g_api.friendName(g_api.friends, a_id) : nullptr;
            return n && *n ? n : std::to_string(a_id);
        }

        void ClosePair(Pair& p, const char* a_why)
        {
            if (p.steam != k_HSteamNetConnection_Invalid) g_api.closeConn(g_api.sockets, p.steam, 0, a_why, false);
            if (p.local != k_HSteamNetConnection_Invalid && g_gns) g_gns->CloseConnection(p.local, 0, a_why, false);
            REX::INFO("[Steam] relay for {} closed ({})", p.name, a_why);
        }

        // Steam connection state changes (listen socket and its connections). May run on the game's
        // main thread (SteamAPI_RunCallbacks) or the relay thread (RunCallbacks below).
        void OnSteamStatus(SteamNetConnectionStatusChangedCallback_t* a_info)
        {
            std::lock_guard l(g_lock);
            const auto st = a_info->m_info.m_eState;
            const uint64_t who = a_info->m_info.m_identityRemote.GetSteamID64();
            auto it = std::find_if(g_pairs.begin(), g_pairs.end(), [&](const Pair& p) { return p.steam == a_info->m_hConn; });
            if (st == k_ESteamNetworkingConnectionState_Connecting && it == g_pairs.end() &&
                a_info->m_info.m_hListenSocket == g_listen && g_listen != k_HSteamListenSocket_Invalid) {
                // Friends only: anyone who knows the host's Steam ID could otherwise connect.
                const bool self = who == MySteamId();   // [Debug] SteamSelfTest
                if (g_friendsOnly.load() && !self && !(g_api.hasFriend && g_api.hasFriend(g_api.friends, who, kFriendFlagImmediate))) {
                    REX::WARN("[Steam] refused {}: not on the host's Steam friends list (friends only)", NameOf(who));
                    g_api.closeConn(g_api.sockets, a_info->m_hConn, 1007, "This host only lets Steam friends join", false);
                    return;
                }
                if (auto b = g_badTries.find(who); b != g_badTries.end() && std::chrono::steady_clock::now() < b->second.lockedUntil) {
                    REX::WARN("[Steam] refused {}: too many wrong passwords", NameOf(who));
                    g_api.closeConn(g_api.sockets, a_info->m_hConn, 1004, "Too many wrong passwords - wait a minute and try again", false);
                    return;
                }
                if (g_pairs.size() >= kMaxRelayed) {
                    g_api.closeConn(g_api.sockets, a_info->m_hConn, 1002, "Server full", false);
                    return;
                }
                if (g_api.accept(g_api.sockets, a_info->m_hConn) != 1) {   // k_EResultOK
                    REX::WARN("[Steam] could not accept {}", NameOf(who));
                    g_api.closeConn(g_api.sockets, a_info->m_hConn, 0, "accept failed", false);
                    return;
                }
                Pair p;
                p.steam = a_info->m_hConn;
                p.steamId = who;
                p.name = NameOf(who);
                SteamNetworkingIPAddr addr;
                addr.SetIPv4(0x7F000001, g_serverPort);   // the co-op server on this PC
                p.local = g_gns ? g_gns->ConnectByIPAddress(addr, 0, nullptr) : k_HSteamNetConnection_Invalid;
                REX::INFO("[Steam] {} is joining through Steam (relayed to the server, local connection {})", p.name, p.local);
                g_pairs.push_back(std::move(p));
            } else if ((st == k_ESteamNetworkingConnectionState_ClosedByPeer || st == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) &&
                       it != g_pairs.end()) {
                REX::INFO("[Steam] {} left: {}", it->name, a_info->m_info.m_szEndDebug);
                ClosePair(*it, "Steam player left");
                g_pairs.erase(it);
            } else if (st == k_ESteamNetworkingConnectionState_Connected && it != g_pairs.end()) {
                REX::INFO("[Steam] {} connected through Steam", it->name);
            }
        }

        // Passed on the way it came: positions/NPC states unreliable (newest wins), events reliable.
        int RelayFlags(const SteamNetworkingMessage_t* a_msg)
        {
            return (a_msg->m_nFlags & kSendReliable) ? kSendReliable : 1;   // 1 = unreliable, no Nagle delay
        }

        void PumpRelay()
        {
            std::lock_guard l(g_lock);
            SteamNetworkingMessage_t* msgs[32];
            for (auto it = g_pairs.begin(); it != g_pairs.end();) {
                Pair& p = *it;
                // Steam -> server
                int n = g_api.recv(g_api.sockets, p.steam, msgs, 32);
                for (int i = 0; i < n; ++i) {
                    const auto* d = static_cast<const uint8_t*>(msgs[i]->m_pData);
                    if (p.localUp)
                        g_gns->SendMessageToConnection(p.local, d, static_cast<uint32_t>(msgs[i]->m_cbSize), RelayFlags(msgs[i]), nullptr);
                    else
                        p.toLocal.emplace_back(d, d + msgs[i]->m_cbSize);
                    msgs[i]->Release();
                }
                if (!p.localUp && p.local != k_HSteamNetConnection_Invalid) {
                    SteamNetConnectionInfo_t ci{};
                    if (g_gns->GetConnectionInfo(p.local, &ci) && ci.m_eState == k_ESteamNetworkingConnectionState_Connected) {
                        p.localUp = true;
                        for (auto& b : p.toLocal)
                            g_gns->SendMessageToConnection(p.local, b.data(), static_cast<uint32_t>(b.size()), k_nSteamNetworkingSend_Reliable, nullptr);
                        p.toLocal.clear();
                    }
                }
                // server -> Steam
                if (p.localUp) {
                    n = g_gns->ReceiveMessagesOnConnection(p.local, msgs, 32);
                    for (int i = 0; i < n; ++i) {
                        g_api.send(g_api.sockets, p.steam, msgs[i]->m_pData, static_cast<uint32_t>(msgs[i]->m_cbSize), RelayFlags(msgs[i]), nullptr);
                        msgs[i]->Release();
                    }
                }
                ++it;
            }
        }

        void RelayThread()
        {
            REX::INFO("[Steam] relay thread running");
            while (g_hosting.load()) {
                g_api.runCallbacks(g_api.sockets);
                PumpRelay();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            g_relayThread.store(false);
        }

        void SetHostPresence(const std::string& a_hostName)
        {
            if (!g_api.setPresence) return;
            g_api.setPresence(g_api.friends, kPresenceKey, "host");
            g_api.setPresence(g_api.friends, "ffhost", a_hostName.c_str());
            g_api.setPresence(g_api.friends, "status", "Hosting FathersFallout co-op");
            g_api.setPresence(g_api.friends, "connect", ("+ffjoin " + std::to_string(MySteamId())).c_str());
        }
    }

    bool Init()
    {
        std::call_once(g_initOnce, []() {
            HMODULE dll = GetModuleHandleA("steam_api64.dll");
            if (!dll) {
                REX::INFO("[Steam] no steam_api64.dll in this game (GOG?): Steam join off, IP join only");
                return;
            }
            GetIface_t sockets = nullptr, utils = nullptr, friends = nullptr, user = nullptr;
            bool ok = Load(dll, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012", sockets) &&
                      Load(dll, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", utils) &&
                      Load(dll, "SteamAPI_SteamFriends_v017", friends) &&
                      Load(dll, "SteamAPI_SteamUser_v021", user) &&
                      Load(dll, "SteamAPI_ISteamUser_GetSteamID", g_api.getSteamID) &&
                      Load(dll, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", g_api.initRelay) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P", g_api.listenP2P) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_ConnectP2P", g_api.connectP2P) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_AcceptConnection", g_api.accept) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_CloseConnection", g_api.closeConn) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket", g_api.closeListen) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection", g_api.send) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection", g_api.recv) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo", g_api.info) &&
                      Load(dll, "SteamAPI_ISteamNetworkingSockets_RunCallbacks", g_api.runCallbacks) &&
                      Load(dll, "SteamAPI_ISteamFriends_GetFriendCount", g_api.friendCount) &&
                      Load(dll, "SteamAPI_ISteamFriends_GetFriendByIndex", g_api.friendByIndex) &&
                      Load(dll, "SteamAPI_ISteamFriends_GetFriendGamePlayed", g_api.friendGame) &&
                      Load(dll, "SteamAPI_ISteamFriends_GetFriendRichPresence", g_api.friendPresence) &&
                      Load(dll, "SteamAPI_ISteamFriends_GetFriendPersonaName", g_api.friendName) &&
                      Load(dll, "SteamAPI_ISteamFriends_GetPersonaName", g_api.myName) &&
                      Load(dll, "SteamAPI_ISteamFriends_SetRichPresence", g_api.setPresence) &&
                      Load(dll, "SteamAPI_ISteamFriends_ClearRichPresence", g_api.clearPresence) &&
                      Load(dll, "SteamAPI_ISteamFriends_RequestFriendRichPresence", g_api.reqPresence) &&
                      Load(dll, "SteamAPI_ISteamFriends_HasFriend", g_api.hasFriend);
            if (!ok) return;
            g_api.sockets = sockets();
            g_api.utils = utils();
            g_api.friends = friends();
            g_api.user = user();
            if (!g_api.sockets || !g_api.utils || !g_api.friends || !g_api.user) {
                REX::WARN("[Steam] Steam is not running for this game: Steam join off");
                return;
            }
            g_api.initRelay(g_api.utils);   // start finding Valve's relays now: the first join is faster
            g_ready = true;
            using Register_t = void (*)(CCallbackBase*, int);
            if (auto reg = reinterpret_cast<Register_t>(GetProcAddress(dll, "SteamAPI_RegisterCallback"))) {
                reg(&g_joinCallback, kJoinRequested);
                REX::INFO("[Steam] Join Game / invites handled");
            }
            // Steam started this game for a Join Game click: the host is on our command line.
            if (const uint64_t host = ParseJoin(GetCommandLineA())) {
                g_joinRequest = host;
                REX::INFO("[Steam] started to join host {}", host);
            }
            REX::INFO("[Steam] ready: {} ({})", MyName(), MySteamId());
        });
        return g_ready;
    }

    bool Available() { return g_ready; }

    uint64_t TakeJoinRequest() { return g_joinRequest.exchange(0); }

    void SetGuestPresence(const std::string& a_hostName)
    {
        if (!g_ready || !g_api.setPresence) return;
        g_api.setPresence(g_api.friends, "status", a_hostName.empty() ? "" : ("Playing FathersFallout co-op with " + a_hostName).c_str());
    }

    uint64_t MySteamId() { return g_ready ? g_api.getSteamID(g_api.user) : 0; }

    std::string MyName()
    {
        const char* n = g_ready ? g_api.myName(g_api.friends) : nullptr;
        return n ? n : "";
    }

    void StartHosting(ISteamNetworkingSockets* a_gns, uint16_t a_serverPort, const std::string& a_hostName)
    {
        if (!g_ready || !a_gns || g_hosting.load()) return;
        {
            std::lock_guard l(g_lock);
            g_gns = a_gns;
            g_serverPort = a_serverPort;
            SteamNetworkingConfigValue_t opt;
            opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&OnSteamStatus));
            g_listen = g_api.listenP2P(g_api.sockets, kVirtualPort, 1, &opt);
            if (g_listen == k_HSteamListenSocket_Invalid) {
                REX::WARN("[Steam] could not open the Steam listen socket: Steam join off (IP join still works)");
                return;
            }
        }
        SetHostPresence(a_hostName);
        g_hosting.store(true);
        if (!g_relayThread.exchange(true)) std::thread(RelayThread).detach();
        REX::INFO("[Steam] hosting through Steam as {} (friend code {})", MyName(), MySteamId() & 0xFFFFFFFFull);
    }

    void StopHosting()
    {
        if (!g_hosting.exchange(false)) return;
        std::lock_guard l(g_lock);
        for (auto& p : g_pairs) ClosePair(p, "host stopped");
        g_pairs.clear();
        if (g_listen != k_HSteamListenSocket_Invalid) g_api.closeListen(g_api.sockets, g_listen);
        g_listen = k_HSteamListenSocket_Invalid;
        if (g_api.clearPresence) g_api.clearPresence(g_api.friends);
        REX::INFO("[Steam] stopped hosting");
    }

    bool IsHosting() { return g_hosting.load(); }

    void SetFriendsOnly(bool a_on) { g_friendsOnly.store(a_on); }
    bool FriendsOnly() { return g_friendsOnly.load(); }

    int RelayedPlayers()
    {
        std::lock_guard l(g_lock);
        return static_cast<int>(g_pairs.size());
    }

    bool OnGnsStatus(SteamNetConnectionStatusChangedCallback_t* a_info)
    {
        std::lock_guard l(g_lock);
        auto it = std::find_if(g_pairs.begin(), g_pairs.end(), [&](const Pair& p) { return p.local == a_info->m_hConn; });
        if (it == g_pairs.end()) return false;
        const auto st = a_info->m_info.m_eState;
        if (st == k_ESteamNetworkingConnectionState_ClosedByPeer || st == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            REX::INFO("[Steam] server closed {}'s relay: {}", it->name, a_info->m_info.m_szEndDebug);
            if (a_info->m_info.m_eEndReason == 1003) {   // wrong password (server's kEndPassword)
                auto& b = g_badTries[it->steamId];
                const auto now = std::chrono::steady_clock::now();
                if (now - b.first > std::chrono::minutes(1)) { b.first = now; b.count = 0; }
                if (++b.count >= 5) b.lockedUntil = now + std::chrono::minutes(1);
            }
            ClosePair(*it, a_info->m_info.m_szEndDebug[0] ? a_info->m_info.m_szEndDebug : "server closed");
            g_pairs.erase(it);
        }
        return true;
    }

    uint64_t FindHost(const std::string& a_target, std::string& a_outName, std::string& a_outError)
    {
        if (!g_ready) { a_outError = "Steam is not available in this game (GOG copy?) - use the host's IP"; return 0; }
        std::string t = a_target;
        t.erase(0, t.find_first_not_of(" \t"));
        t.erase(t.find_last_not_of(" \t") + 1);
        const bool automatic = t.empty() || _stricmp(t.c_str(), "auto") == 0;
        // A number: friend code (Steam > Add a Friend) or a full SteamID64.
        if (!automatic && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            const uint64_t n = std::stoull(t);
            const uint64_t id = n < 0x100000000ull ? 76561197960265728ull + n : n;
            a_outName = NameOf(id);
            return id;
        }
        const int count = g_api.friendCount(g_api.friends, kFriendFlagImmediate);
        std::vector<std::pair<uint64_t, std::string>> hosts, named;
        for (int i = 0; i < count; ++i) {
            const uint64_t id = g_api.friendByIndex(g_api.friends, i, kFriendFlagImmediate);
            uint8_t game[32]{};   // FriendGameInfo_t: CGameID first
            if (!g_api.friendGame(g_api.friends, id, game)) continue;
            uint64_t gameId = 0;
            std::memcpy(&gameId, game, sizeof(gameId));
            if ((gameId & 0xFFFFFF) != kFallout4AppId) continue;
            g_api.reqPresence(g_api.friends, id);
            const std::string name = NameOf(id);
            const char* pres = g_api.friendPresence(g_api.friends, id, kPresenceKey);
            const bool hosting = pres && std::strcmp(pres, "host") == 0;
            if (hosting) hosts.emplace_back(id, name);
            if (!automatic) {
                std::string a = name, b = t;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                if (a.find(b) != std::string::npos) named.emplace_back(id, name);
            }
        }
        if (!automatic) {
            if (named.empty()) { a_outError = "no Steam friend named \"" + t + "\" is playing Fallout 4"; return 0; }
            a_outName = named.front().second;
            return named.front().first;
        }
        if (hosts.empty()) { a_outError = "no Steam friend is hosting FathersFallout right now"; return 0; }
        if (hosts.size() > 1) REX::INFO("[Steam] {} friends are hosting; joining {} (pick one by name in the launcher)", hosts.size(), hosts.front().second);
        a_outName = hosts.front().second;
        return hosts.front().first;
    }

    HSteamNetConnection Connect(uint64_t a_hostSteamId)
    {
        if (!g_ready || !a_hostSteamId) return k_HSteamNetConnection_Invalid;
        SteamNetworkingIdentity id;
        id.Clear();
        id.SetSteamID64(a_hostSteamId);
        return g_api.connectP2P(g_api.sockets, id, kVirtualPort, 0, nullptr);
    }

    bool Send(HSteamNetConnection a_conn, const void* a_data, uint32_t a_size, int a_flags)
    {
        return g_ready && g_api.send(g_api.sockets, a_conn, a_data, a_size, a_flags, nullptr) == 1;
    }

    int Receive(HSteamNetConnection a_conn, SteamNetworkingMessage_t** a_out, int a_max)
    {
        return g_ready ? g_api.recv(g_api.sockets, a_conn, a_out, a_max) : 0;
    }

    int State(HSteamNetConnection a_conn, std::string& a_endDebug)
    {
        if (!g_ready) return 0;
        SteamNetConnectionInfo_t ci{};
        if (!g_api.info(g_api.sockets, a_conn, &ci)) return 0;
        a_endDebug = ci.m_szEndDebug;
        return static_cast<int>(ci.m_eState);
    }

    void Close(HSteamNetConnection a_conn)
    {
        if (g_ready && a_conn != k_HSteamNetConnection_Invalid) g_api.closeConn(g_api.sockets, a_conn, 0, "Disconnecting", false);
    }

    void RunCallbacks()
    {
        if (g_ready) g_api.runCallbacks(g_api.sockets);
    }

    void SelfTest()
    {
        if (!g_ready) return;
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const auto conn = Connect(MySteamId());
            REX::INFO("[Steam] self-test: connecting to our own Steam account (connection {})", conn);
            bool sent = false;
            for (int i = 0; i < 1500; ++i) {   // 30 s
                std::string why;
                const int st = State(conn, why);
                if (st == k_ESteamNetworkingConnectionState_Connected && !sent) {
                    sent = true;
                    REX::INFO("[Steam] self-test: Steam connection up after {} ms, sending a handshake", i * 20);
                    struct { MessageHeader h; ConnectionRequestMsg r; } pkt{};
                    pkt.h.type = MessageType::ConnectionRequest;
                    pkt.h.size = sizeof(pkt);
                    std::strncpy(pkt.r.playerName, "SteamSelfTest", sizeof(pkt.r.playerName) - 1);
                    pkt.r.protocolVersion = PROTOCOL_VERSION;
                    Send(conn, &pkt, sizeof(pkt));
                }
                if (st == 0 || st >= k_ESteamNetworkingConnectionState_ClosedByPeer) {
                    REX::WARN("[Steam] self-test: connection ended in state {}: {}", st, why);
                    return;
                }
                SteamNetworkingMessage_t* msgs[16];
                const int n = Receive(conn, msgs, 16);
                for (int k = 0; k < n; ++k) {
                    if (msgs[k]->m_cbSize >= static_cast<int>(sizeof(MessageHeader))) {
                        const auto* h = static_cast<const MessageHeader*>(msgs[k]->m_pData);
                        if (h->type == MessageType::ConnectionAccepted) {
                            const auto* acc = reinterpret_cast<const ConnectionAcceptedMsg*>(h + 1);
                            REX::INFO("[Steam] self-test: PASSED - the server accepted us through Steam as player {}", acc->playerId);
                            msgs[k]->Release();
                            for (int j = k + 1; j < n; ++j) msgs[j]->Release();
                            std::this_thread::sleep_for(std::chrono::seconds(3));
                            Close(conn);
                            return;
                        }
                    }
                    msgs[k]->Release();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            REX::WARN("[Steam] self-test: no answer in 30 s");
            Close(conn);
        }).detach();
    }
}
