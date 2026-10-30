#include "GameServer.h"
#include "../Heartbeat.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace {
    constexpr int kHandshakeSeconds = 10;        // a connection must say who it is within this
    constexpr size_t kMaxPending = 16;           // connections waiting for their handshake at once
    constexpr int kMaxBadPasswords = 5;          // per address per minute, then it waits a minute
    constexpr uint32_t kMaxMessagesPerSecond = 6000;   // a host in a busy area sends ~1500/s
    // Close reasons (GNS application range 1000+); the text reaches the other game as the end reason.
    constexpr int kEndProtocol = 1001, kEndFull = 1002, kEndPassword = 1003, kEndLocked = 1004,
                  kEndTimeout = 1005, kEndNeedsUpdate = 1006;

    // Player names come from the other game: printable, trimmed, never empty.
    std::string CleanName(const char* raw, size_t cap) {
        std::string out;
        for (size_t i = 0; i < cap && raw[i]; ++i) {
            const unsigned char c = static_cast<unsigned char>(raw[i]);
            if (c >= 0x20 && c != 0x7F) out += static_cast<char>(c);
        }
        out.erase(0, out.find_first_not_of(' '));
        out.erase(out.find_last_not_of(' ') + 1);
        return out.empty() ? std::string("Wanderer") : out;
    }

    // Same time whatever matches, so a wrong password can't be guessed a letter at a time.
    bool SamePassword(const std::string& a, const char* b, size_t cap) {
        const size_t lb = strnlen(b, cap);
        unsigned diff = static_cast<unsigned>(a.size() ^ lb);
        for (size_t i = 0; i < cap; ++i)
            diff |= static_cast<unsigned char>(i < a.size() ? a[i] : 0) ^ static_cast<unsigned char>(i < lb ? b[i] : 0);
        return diff == 0;
    }
}

GameServer& GameServer::GetInstance() {
    static GameServer instance;
    return instance;
}

bool GameServer::Initialize(uint16_t port) {
    if (m_initialized) return true;

    spdlog::info("[Server] Initializing on port {}", port);

    SteamNetworkingErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        spdlog::error("[Server] Failed to init GameNetworkingSockets: {}", errMsg);
        return false;
    }

    m_sockets = SteamNetworkingSockets();
    if (!m_sockets) {
        spdlog::error("[Server] Failed to get networking interface");
        return false;
    }

    // Allow plain IP connections (no Steam authentication).
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

    // Global callback for connection state changes.
    SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&GameServer::OnConnStatusChangedStatic);

    SteamNetworkingIPAddr bindAddr;
    bindAddr.Clear();
    bindAddr.m_port = port;

    m_listenSocket = m_sockets->CreateListenSocketIP(bindAddr, 0, nullptr);
    if (m_listenSocket == k_HSteamListenSocket_Invalid) {
        spdlog::error("[Server] Failed to create listen socket on port {}", port);
        GameNetworkingSockets_Kill();
        return false;
    }

    m_pollGroup = m_sockets->CreatePollGroup();
    if (m_pollGroup == k_HSteamNetPollGroup_Invalid) {
        spdlog::error("[Server] Failed to create poll group");
        GameNetworkingSockets_Kill();
        return false;
    }

    m_port = port;
    m_startTime = std::chrono::steady_clock::now();
    m_lastStatus = m_startTime;
    m_initialized = true;
    if (m_wars) {
        m_teams = std::make_unique<Wars::Teams>();
        m_teams->Load(Wars::kTeamsPath);
        StartALife();
    }
    m_running = true;

    // Heartbeat thread to the master server (when one is set).
    if (!m_master.empty()) {
        std::thread([this]() {
            while (m_running) {
                uint32_t players;
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    players = GetPlayerCount();
                }
                Heartbeat::Send(m_master, m_name, m_port, players, m_maxPlayers);
                // sleep 10 s, checking m_running every 100 ms to quit quickly
                for (int i = 0; i < 100 && m_running; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
        spdlog::info("[Server] master server heartbeat enabled: {}", m_master);
    }

    spdlog::info("========================================");
    spdlog::info("  {}", m_name);
    spdlog::info("  Port        : {}", port);
    spdlog::info("  Players     : 0/{}", m_maxPlayers);
    spdlog::info("  Tick rate   : {}/s", m_tickRate);
    spdlog::info("  Password    : {}", m_password.empty() ? "none (anyone who can reach this port can join)" : "on");
    spdlog::info("  Listening for connections...");
    spdlog::info("  Type 'help' for commands.");
    spdlog::info("========================================");
    return true;
}

void GameServer::Shutdown() {
    m_running = false;
    if (m_alife) {
        m_alife->SaveState(Wars::kStatePath);
        spdlog::info("[ALife] world state saved");
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& [conn, client] : m_clients) {
            m_sockets->CloseConnection(conn, 0, "Server shutting down", false);
        }
        m_clients.clear();
    }

    if (m_pollGroup != k_HSteamNetPollGroup_Invalid) {
        m_sockets->DestroyPollGroup(m_pollGroup);
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    if (m_listenSocket != k_HSteamListenSocket_Invalid) {
        m_sockets->CloseListenSocket(m_listenSocket);
        m_listenSocket = k_HSteamListenSocket_Invalid;
    }

    if (m_initialized) {
        GameNetworkingSockets_Kill();
        m_initialized = false;
    }

    spdlog::info("[Server] Shutdown complete");
}

void GameServer::Run() {
    auto lastTick = std::chrono::steady_clock::now();

    while (m_running) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - lastTick).count();

        if (elapsed >= 1.0f / static_cast<float>(m_tickRate)) {
            Update();
            lastTick = now;
        }

        TickALife();

        // Status line every 30 s.
        if (std::chrono::duration<float>(now - m_lastStatus).count() >= 30.0f) {
            PrintStatus();
            m_lastStatus = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void GameServer::Update() {
    // Connection state changes (fires OnConnStatusChanged), then messages.
    m_sockets->RunCallbacks();
    DropStaleHandshakes();
    ProcessMessages();
    ProcessConsoleCommands();
}

uint32_t GameServer::GetPlayerCount() const {
    uint32_t count = 0;
    for (const auto& [conn, client] : m_clients) {
        if (client.isConnected) count++;
    }
    return count;
}

void GameServer::Refuse(HSteamNetConnection conn, int reason, const char* why) {
    m_pending.erase(conn);
    m_rates.erase(conn);
    m_sockets->CloseConnection(conn, reason, why, false);
}

void GameServer::DropStaleHandshakes() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now - it->second > std::chrono::seconds(kHandshakeSeconds)) {
            const HSteamNetConnection conn = it->first;
            it = m_pending.erase(it);
            m_rates.erase(conn);
            m_sockets->CloseConnection(conn, kEndTimeout, "No handshake", false);
            spdlog::warn("[Server] a connection sent no handshake in {} s: closed", kHandshakeSeconds);
        } else {
            ++it;
        }
    }
}

// A connection sending more than kMaxMessagesPerSecond has the rest of that second dropped (true = drop).
bool GameServer::OverRateLimit(HSteamNetConnection conn) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    // Neither joined nor waiting for its handshake (refused or closed a moment ago): nothing to read.
    if (!m_clients.count(conn) && !m_pending.count(conn)) return true;
    const auto now = std::chrono::steady_clock::now();
    auto& r = m_rates[conn];
    if (now - r.windowStart >= std::chrono::seconds(1)) {
        if (r.dropped)
            spdlog::warn("[Server] connection {} sent too much: {} messages dropped last second", conn, r.dropped);
        r.windowStart = now;
        r.count = 0;
        r.dropped = 0;
    }
    if (++r.count <= kMaxMessagesPerSecond) return false;
    ++r.dropped;
    return true;
}

void GameServer::HandleConnectionRequest(HSteamNetConnection conn, const ConnectionRequestMsg& msg, bool hasPassword) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    if (m_clients.count(conn)) return;   // a second handshake on the same connection: already in

    SteamNetConnectionInfo_t info{};
    m_sockets->GetConnectionInfo(conn, &info);
    char addrBuf[SteamNetworkingIPAddr::k_cchMaxString]{};
    info.m_addrRemote.ToString(addrBuf, sizeof(addrBuf), false);
    // Steam guests come in through the host's game on this PC (127.0.0.1): never locked out here,
    // or the host would be too; the host's Steam relay counts those per Steam account instead.
    const bool local = info.m_addrRemote.IsLocalHost();
    const std::string addr = addrBuf;
    const std::string name = CleanName(msg.playerName, sizeof(msg.playerName));
    const auto now = std::chrono::steady_clock::now();

    const uint32_t expected = PROTOCOL_VERSION + (m_wars ? kWarsProtocolOffset : 0);
    if (msg.protocolVersion != expected) {
        spdlog::warn("[Server] Connection rejected: protocol {} (server {})", msg.protocolVersion, expected);
        const bool theyWar = msg.protocolVersion >= kWarsProtocolOffset;
        const std::string why = theyWar != m_wars
            ? std::string(m_wars ? "This host is playing Wasteland Wars - choose WASTELAND WARS in the launcher"
                                 : "This host is playing the campaign - choose CAMPAIGN CO-OP in the launcher")
            : "Different FathersFallout version (host protocol " + std::to_string(expected) +
                  ", yours " + std::to_string(msg.protocolVersion) + ") - everyone needs the same update";
        Refuse(conn, kEndProtocol, why.c_str());
        return;
    }

    if (!m_password.empty()) {
        auto& bad = m_badTries[addr];
        if (!local && now < bad.lockedUntil) {
            spdlog::warn("[Server] {} refused: too many wrong passwords from this address", name);
            Refuse(conn, kEndLocked, "Too many wrong passwords - wait a minute and try again");
            return;
        }
        if (!hasPassword) {
            spdlog::warn("[Server] {} refused: their FathersFallout is too old for a password", name);
            Refuse(conn, kEndNeedsUpdate, "This host uses a password: update FathersFallout to Update 23 or newer");
            return;
        }
        if (!SamePassword(m_password, msg.password, sizeof(msg.password))) {
            if (now - bad.first > std::chrono::minutes(1)) { bad.first = now; bad.count = 0; }
            if (++bad.count >= kMaxBadPasswords && !local) bad.lockedUntil = now + std::chrono::minutes(1);
            spdlog::warn("[Server] {} refused: wrong password", name);
            Refuse(conn, kEndPassword, "Wrong password");
            return;
        }
        m_badTries.erase(addr);
    }

    if (m_clients.size() >= m_maxPlayers) {
        spdlog::warn("[Server] Connection rejected: server full");
        Refuse(conn, kEndFull, "Server full");
        return;
    }
    m_pending.erase(conn);

    ClientInfo client{};
    client.id = m_nextClientId++;
    client.connection = conn;
    client.name = name;
    client.isConnected = true;
    client.lastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;

    m_clients[conn] = client;

    ConnectionAcceptedMsg acceptMsg{};
    acceptMsg.playerId = client.id;
    acceptMsg.currentPlayers = GetPlayerCount();
    acceptMsg.maxPlayers = m_maxPlayers;

    // ConnectionAccepted with its header, the way the game reads it.
    MessageHeader header;
    header.type = MessageType::ConnectionAccepted;
    header.size = sizeof(MessageHeader) + sizeof(acceptMsg);
    header.timestamp = 0;
    std::vector<uint8_t> buffer(header.size);
    memcpy(buffer.data(), &header, sizeof(MessageHeader));
    memcpy(buffer.data() + sizeof(MessageHeader), &acceptMsg, sizeof(acceptMsg));
    m_sockets->SendMessageToConnection(conn, buffer.data(), (uint32_t)buffer.size(),
        k_nSteamNetworkingSend_Reliable, nullptr);

    spdlog::info("[+] {} joined  (ID {}) - players {}/{}",
        client.name.c_str(), client.id, GetPlayerCount(), m_maxPlayers);

    ChatMessageMsg welcomeMsg{};
    welcomeMsg.senderId = 0;
    snprintf(welcomeMsg.message, sizeof(welcomeMsg.message), "%s has joined the wasteland", client.name.c_str());
    welcomeMsg.channel = 0;

    BroadcastMessage(MessageType::ChatMessage, &welcomeMsg, sizeof(welcomeMsg), client.id);
}

void GameServer::HandlePlayerPosition(HSteamNetConnection conn, const PlayerPositionMsg* msg) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    auto it = m_clients.find(conn);
    if (it != m_clients.end()) {
        it->second.x = msg->x;
        it->second.y = msg->y;
        it->second.z = msg->z;
        it->second.cellId = msg->cellId;
        it->second.lastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;

        // The playerId is overwritten with the server's own id for this connection: no relying on the
        // sender to fill it in, and no posing as someone else.
        PlayerPositionMsg out = *msg;
        out.playerId = it->second.id;
        BroadcastMessage(MessageType::PlayerPosition, &out, sizeof(out), it->second.id);
    }
}

void GameServer::HandleChatMessage(HSteamNetConnection conn, const ChatMessageMsg* msg) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    auto it = m_clients.find(conn);
    if (it != m_clients.end()) {
        ChatMessageMsg broadcastMsg = *msg;
        broadcastMsg.senderId = it->second.id;
        broadcastMsg.message[sizeof(broadcastMsg.message) - 1] = '\0';   // never relay an unterminated string
        BroadcastMessage(MessageType::ChatMessage, &broadcastMsg, sizeof(broadcastMsg), 0);
    }
}

void GameServer::HandleNpcState(HSteamNetConnection conn, const NpcStateMsg* msg) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) return;
    // Relayed as is to everyone else (the NPC's owner sends it); the server doesn't read it.
    BroadcastMessage(MessageType::NpcState, msg, sizeof(NpcStateMsg), it->second.id);
}

// Co-op messages carry a senderId in their first 4 bytes. The server overwrites it with
// the authoritative id (no spoofing) and forwards the payload to everyone else untouched.
void GameServer::RelayCoop(HSteamNetConnection conn, MessageType type, const void* payload, uint32_t size) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(conn);
    if (it == m_clients.end() || size < sizeof(uint32_t) || size > 16384) return;  // appearance ~8 KB max
    std::vector<uint8_t> data(static_cast<const uint8_t*>(payload), static_cast<const uint8_t*>(payload) + size);
    const uint32_t sender = it->second.id;
    memcpy(data.data(), &sender, sizeof(sender));
    BroadcastMessage(type, data.data(), size, sender);
}

void GameServer::HandlePlayerAppearance(HSteamNetConnection conn, const PlayerAppearanceMsg* msg) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) return;

    // Stored with the server's id for this connection.
    PlayerAppearanceMsg mine = *msg;
    mine.playerId = it->second.id;
    it->second.appearance = mine;
    it->second.hasAppearance = true;

    // 1. Send this player's appearance to the others.
    BroadcastMessage(MessageType::PlayerAppearance, &mine, sizeof(mine), it->second.id);

    // 2. Send the newcomer the appearances already known (straight to its connection: not through
    //    SendMessageToClient, which would lock the mutex again).
    for (auto& [c, cl] : m_clients) {
        if (c == conn || !cl.hasAppearance) continue;
        MessageHeader header;
        header.type = MessageType::PlayerAppearance;
        header.size = sizeof(MessageHeader) + sizeof(cl.appearance);
        header.timestamp = 0;
        std::vector<uint8_t> buffer(header.size);
        memcpy(buffer.data(), &header, sizeof(MessageHeader));
        memcpy(buffer.data() + sizeof(MessageHeader), &cl.appearance, sizeof(cl.appearance));
        m_sockets->SendMessageToConnection(conn, buffer.data(), (uint32_t)buffer.size(),
            k_nSteamNetworkingSend_Reliable, nullptr);
    }

    spdlog::info("[appearance] {} (ID {}): sex={} headparts={}",
        it->second.name.c_str(), it->second.id, mine.sex, mine.numHeadParts);
}

void GameServer::BroadcastMessage(MessageType type, const void* data, uint32_t size, uint32_t excludeId) {
    MessageHeader header;
    header.type = type;
    header.size = sizeof(MessageHeader) + size;
    header.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<uint8_t> buffer(header.size);
    memcpy(buffer.data(), &header, sizeof(MessageHeader));
    if (data && size > 0) {
        memcpy(buffer.data() + sizeof(MessageHeader), data, size);
    }

    // Positions, NPC states and voice: unreliable, newest wins (a lost one must not hold up the rest).
    const int flags = (type == MessageType::PlayerPosition || type == MessageType::NpcState || type == MessageType::CoopVoice)
                          ? k_nSteamNetworkingSend_UnreliableNoNagle : k_nSteamNetworkingSend_Reliable;
    for (const auto& [conn, client] : m_clients) {
        if (client.isConnected && client.id != excludeId) {
            m_sockets->SendMessageToConnection(conn, buffer.data(), (uint32_t)buffer.size(), flags, nullptr);
        }
    }
}

void GameServer::SendMessageToClient(uint32_t clientId, MessageType type, const void* data, uint32_t size) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    for (const auto& [conn, client] : m_clients) {
        if (client.id == clientId) {
            MessageHeader header;
            header.type = type;
            header.size = sizeof(MessageHeader) + size;
            header.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            std::vector<uint8_t> buffer(header.size);
            memcpy(buffer.data(), &header, sizeof(MessageHeader));
            if (data && size > 0) {
                memcpy(buffer.data() + sizeof(MessageHeader), data, size);
            }

            m_sockets->SendMessageToConnection(conn, buffer.data(), (uint32_t)buffer.size(),
                k_nSteamNetworkingSend_Reliable, nullptr);
            break;
        }
    }
}

// Payload at least as big as the message's struct: a short or malformed packet is dropped instead of
// read past its end.
template <class T>
static const T* PayloadAs(const SteamNetworkingMessage_t* msg)
{
    return (size_t)msg->m_cbSize >= sizeof(MessageHeader) + sizeof(T)
               ? reinterpret_cast<const T*>(static_cast<const uint8_t*>(msg->m_pData) + sizeof(MessageHeader)) : nullptr;
}

void GameServer::ProcessMessages() {
    // Everything waiting, not 32 per tick: with the host's NPC stream the queue grew faster than that
    // (~1000-2000 messages/s at most) and every player fell further behind.
    for (int batch = 0; batch < 256; ++batch) {
    SteamNetworkingMessage_t* msgs[64];
    int msgCount = m_sockets->ReceiveMessagesOnPollGroup(m_pollGroup, msgs, 64);
    if (msgCount <= 0) break;

    for (int i = 0; i < msgCount; ++i) {
        SteamNetworkingMessage_t* msg = msgs[i];
        if (OverRateLimit(msg->m_conn)) { msg->Release(); continue; }
        if (msg->m_cbSize >= (int)sizeof(MessageHeader)) {
            const auto* header = reinterpret_cast<const MessageHeader*>(msg->m_pData);
            const void* payload = reinterpret_cast<const uint8_t*>(msg->m_pData) + sizeof(MessageHeader);

            switch (header->type) {
                case MessageType::ConnectionRequest: {
                    // Update 22 and older send the request without the password field.
                    const size_t got = (size_t)msg->m_cbSize - sizeof(MessageHeader);
                    if (got < kConnectionRequestV1Size) break;
                    ConnectionRequestMsg req{};
                    memcpy(&req, payload, std::min(got, sizeof(req)));
                    HandleConnectionRequest(msg->m_conn, req, got >= sizeof(req));
                    break;
                }
                case MessageType::WarsPresence:
                    if (auto* m = PayloadAs<WarsPresenceMsg>(msg)) HandleWarsPresence(msg->m_conn, m);
                    break;
                case MessageType::WarsSquadReport:
                    if (auto* m = PayloadAs<WarsSquadMsg>(msg)) HandleWarsReport(msg->m_conn, m);
                    break;
                case MessageType::WarsTeamAction:
                    if (auto* m = PayloadAs<WarsTeamActionMsg>(msg); m && m_teams) HandleWarsTeam(msg->m_conn, m);
                    break;
                case MessageType::WarsRadioTune:
                    if (auto* m = PayloadAs<WarsRadioTuneMsg>(msg); m && m_wars) HandleWarsTune(msg->m_conn, m);
                    break;
                case MessageType::WarsVoice:
                    if (m_wars) HandleWarsVoice(msg->m_conn, static_cast<const uint8_t*>(payload), (uint32_t)(msg->m_cbSize - sizeof(MessageHeader)));
                    break;
                case MessageType::WarsSquadOnline:
                case MessageType::WarsSquadOffline:
                case MessageType::WarsSquadGoal:
                case MessageType::WarsChatter:
                case MessageType::WarsCamp:
                case MessageType::WarsCampGone:
                case MessageType::WarsCapture:
                case MessageType::WarsClaim:
                case MessageType::WarsTeamInfo:
                case MessageType::WarsCrew:
                case MessageType::WarsNotice:
                    break;   // the server's own word: never taken from (or relayed for) a player
                case MessageType::PlayerPosition:
                    if (auto* m = PayloadAs<PlayerPositionMsg>(msg)) HandlePlayerPosition(msg->m_conn, m);
                    break;
                case MessageType::NpcState:
                    if (auto* m = PayloadAs<NpcStateMsg>(msg)) HandleNpcState(msg->m_conn, m);
                    break;
                case MessageType::PlayerAppearance:
                    if (auto* m = PayloadAs<PlayerAppearanceMsg>(msg)) HandlePlayerAppearance(msg->m_conn, m);
                    break;
                case MessageType::ChatMessage:
                    if (auto* m = PayloadAs<ChatMessageMsg>(msg)) HandleChatMessage(msg->m_conn, m);
                    break;
                default:
                    if (static_cast<uint16_t>(header->type) >= static_cast<uint16_t>(MessageType::CoopFirst)) {
                        RelayCoop(msg->m_conn, header->type, payload, (uint32_t)(msg->m_cbSize - sizeof(MessageHeader)));
                        break;
                    }
                    spdlog::debug("[Server] Unhandled message type: {}", static_cast<int>(header->type));
                    break;
            }
        }
        msg->Release();
    }
    if (msgCount < 64) break;
    }
}

void GameServer::OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info) {
    GetInstance().OnConnStatusChanged(info);
}

void GameServer::OnConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting: {
            // New incoming connection: accept it and add it to the poll group.
            if (m_sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                m_sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                spdlog::warn("[Server] Failed to accept connection");
                break;
            }
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                if (m_pending.size() >= kMaxPending) {
                    m_sockets->CloseConnection(info->m_hConn, kEndFull, "Too many connections waiting", false);
                    spdlog::warn("[Server] refused a connection: {} already waiting for their handshake", m_pending.size());
                    break;
                }
                m_pending[info->m_hConn] = std::chrono::steady_clock::now();
            }
            m_sockets->SetConnectionPollGroup(info->m_hConn, m_pollGroup);
            spdlog::info("[Server] Connection accepted, waiting for handshake");
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_pending.erase(info->m_hConn);
            m_rates.erase(info->m_hConn);
            auto it = m_clients.find(info->m_hConn);
            if (it != m_clients.end()) {
                std::string leftName = it->second.name;

                ChatMessageMsg leaveMsg{};
                leaveMsg.senderId = 0;
                snprintf(leaveMsg.message, sizeof(leaveMsg.message), "%s has left the wasteland", it->second.name.c_str());
                leaveMsg.channel = 0;

                uint32_t playerId = it->second.id;
                m_clients.erase(it);

                BroadcastMessage(MessageType::ChatMessage, &leaveMsg, sizeof(leaveMsg), playerId);
                // Tell the others so they remove the body AT ONCE.
                PlayerLeftMsg leftPkt{ playerId };
                BroadcastMessage(MessageType::Disconnect, &leftPkt, sizeof(leftPkt), playerId);
                spdlog::info("[-] {} left  - players {}/{}", leftName, GetPlayerCount(), m_maxPlayers);
            }
            m_sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
            break;
        }

        default:
            break;
    }
}

// ===================== Admin console =====================

double GameServer::UptimeSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();
}

void GameServer::PrintStatus() {
    int total = static_cast<int>(UptimeSeconds());
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;

    uint32_t players;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        players = GetPlayerCount();
    }
    spdlog::info("[status] {} | up {:02d}:{:02d}:{:02d} | players {}/{} | {} tick",
        m_name, h, m, s, players, m_maxPlayers, m_tickRate);
}

void GameServer::QueueCommand(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_commands.push(cmd);
}

void GameServer::ProcessConsoleCommands() {
    for (;;) {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(m_cmdMutex);
            if (m_commands.empty()) break;
            line = std::move(m_commands.front());
            m_commands.pop();
        }
        HandleCommand(line);
    }
}

void GameServer::HandleCommand(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return;

    std::transform(cmd.begin(), cmd.end(), cmd.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (cmd == "help") {
        spdlog::info("Commands:");
        spdlog::info("  help          - show this help");
        spdlog::info("  status / list - list connected players");
        spdlog::info("  say <message> - send a message to every player");
        spdlog::info("  kick <id>     - kick a player by ID");
        spdlog::info("  stop / quit   - shut the server down");
        if (m_alife) {
            spdlog::info("  alife                 - Wasteland Wars world: squads by faction");
            spdlog::info("  alife near <place>    - squads around a place (e.g. alife near Concord)");
            spdlog::info("  alife events [n]      - the latest n world events");
            spdlog::info("  alife speed <x>       - run the world x times faster (1 = normal)");
            spdlog::info("  alife spawn <f> <place> - a new squad of faction f at a place");
            spdlog::info("  alife go <id> <place> - send a squad somewhere now");
            spdlog::info("  alife build <n> <place> - build a place's camp up by n things (negative wrecks)");
            spdlog::info("  alife save            - save the world state now");
            spdlog::info("  alife clear|unclear|attack|unclaim <place> - cleared places, attacks, claims");
            spdlog::info("  teams                 - crews and who plays which side");
            spdlog::info("  teams reset <name>    - let a player change side again now (no cooldown)");
        }
    } else if (cmd == "alife") {
        CmdALife(iss);
    } else if (cmd == "teams") {
        if (!m_teams) { spdlog::warn("[Teams] only on a Wasteland Wars server"); return; }
        std::string sub, who;
        iss >> sub;
        std::getline(iss, who);
        if (!who.empty() && who.front() == ' ') who.erase(0, 1);
        if (sub == "reset") {
            if (m_teams->ResetCooldown(who)) spdlog::info("[Teams] {} may change side again now", who);
            else spdlog::warn("Usage: teams reset <player name> (a player the server knows)");
        } else {
            for (const auto& [id, c] : m_teams->Crews())
                spdlog::info("[Teams] crew #{} [{}] {} - leader {}, {} member(s), {}", id, c.tag, c.name, c.leader, c.members.size(), c.open ? "open" : "invite only");
        }
    } else if (cmd == "status" || cmd == "list") {
        CmdList();
    } else if (cmd == "say") {
        std::string msg;
        std::getline(iss, msg);
        if (!msg.empty() && msg.front() == ' ') msg.erase(0, 1);
        if (msg.empty()) spdlog::warn("Usage: say <message>");
        else CmdSay(msg);
    } else if (cmd == "kick") {
        uint32_t id = 0;
        if (iss >> id) CmdKick(id);
        else spdlog::warn("Usage: kick <id>");
    } else if (cmd == "stop" || cmd == "quit" || cmd == "exit") {
        spdlog::info("Shutting the server down...");
        Stop();
    } else {
        spdlog::warn("Unknown command: '{}'. Type 'help'.", cmd);
    }
}

void GameServer::CmdList() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    spdlog::info("Players connected: {}/{}", GetPlayerCount(), m_maxPlayers);
    for (const auto& [conn, c] : m_clients) {
        if (c.isConnected)
            spdlog::info("  ID {:<4} {}", c.id, c.name);
    }
}

void GameServer::CmdKick(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->second.id == id) {
            std::string name = it->second.name;
            m_sockets->CloseConnection(it->first, 0, "Kicked by admin", false);
            m_rates.erase(it->first);
            m_clients.erase(it);
            // Tell the others so they remove the body AT ONCE.
            PlayerLeftMsg leftPkt{ id };
            BroadcastMessage(MessageType::Disconnect, &leftPkt, sizeof(leftPkt), id);
            spdlog::info("[admin] '{}' (ID {}) kicked", name, id);
            return;
        }
    }
    spdlog::warn("[admin] no player with ID {}", id);
}

void GameServer::CmdSay(const std::string& msg) {
    ChatMessageMsg m{};
    m.senderId = 0;
    snprintf(m.message, sizeof(m.message), "[Server] %s", msg.c_str());
    m.channel = 0;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        BroadcastMessage(MessageType::ChatMessage, &m, sizeof(m), 0);
    }
    spdlog::info("[Server] {}", msg);
}

// --- Wasteland Wars A-Life ---

void GameServer::StartALife() {
    auto alife = std::make_unique<Wars::ALife>();
    if (!alife->LoadWorld(Wars::kWorldPath)) {
        spdlog::warn("[ALife] {} not found: the world has no NPC simulation (run tools/wars/make_world.py)", Wars::kWorldPath);
        return;
    }
    alife->LoadState(Wars::kStatePath);
    m_alife = std::move(alife);
    m_alifeTick = m_alifeSaved = std::chrono::steady_clock::now();
}

// Once a second; saved every minute.
void GameServer::TickALife() {
    if (!m_alife) return;
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - m_alifeTick).count();
    if (dt < 1.0) return;
    m_alifeTick = now;
    m_alife->Tick(std::min(dt, 10.0));
    WarsOnlineOffline();
    WarsChatter();
    WarsCamps();
    WarsCapture();
    WarsTeamsSend();
    if (std::chrono::duration<double>(now - m_alifeSaved).count() >= 60.0) {
        m_alife->SaveState(Wars::kStatePath);
        m_alifeSaved = now;
    }
}

void GameServer::CmdALife(std::istringstream& args) {
    if (!m_alife) { spdlog::warn("[ALife] not running (Wasteland Wars server with wars_world.json only)"); return; }
    std::string sub;
    args >> sub;
    if (sub.empty()) {
        m_alife->PrintSummary();
    } else if (sub == "near") {
        std::string place;
        std::getline(args, place);
        if (!place.empty() && place.front() == ' ') place.erase(0, 1);
        m_alife->PrintNear(place);
    } else if (sub == "events") {
        size_t n = 20;
        args >> n;
        m_alife->PrintEvents(n);
    } else if (sub == "speed") {
        double x = 1.0;
        args >> x;
        m_alife->SetSpeed(x);
        spdlog::info("[ALife] speed x{}", x);
    } else if (sub == "spawn") {
        std::string faction, place;
        args >> faction;
        std::getline(args, place);
        if (!place.empty() && place.front() == ' ') place.erase(0, 1);
        if (!m_alife->SpawnAt(faction, place))
            spdlog::warn("Usage: alife spawn <raider|gunner|mutant|feral|mirelurk|deathclaw|creature|synth|robot|trader> <place>");
    } else if (sub == "go") {
        uint32_t id = 0;
        std::string place;
        args >> id;
        std::getline(args, place);
        if (!place.empty() && place.front() == ' ') place.erase(0, 1);
        if (!m_alife->SendTo(id, place)) spdlog::warn("Usage: alife go <squad id> <place>");
    } else if (sub == "build") {
        int n = 1;
        std::string place;
        args >> n;
        std::getline(args, place);
        if (!place.empty() && place.front() == ' ') place.erase(0, 1);
        if (!m_alife->BuildAt(place, n)) spdlog::warn("Usage: alife build <things, -n wrecks> <place> (a place with camp spots)");
    } else if (sub == "unclaim") {
        std::string place;
        std::getline(args, place);
        if (!place.empty() && place.front() == ' ') place.erase(0, 1);
        if (!m_alife->Unclaim(place)) spdlog::warn("Usage: alife unclaim <place>");
    } else if (sub == "clear" || sub == "unclear" || sub == "attack") {
        std::string place;
        std::getline(args, place);
        if (!place.empty() && place.front() == ' ') place.erase(0, 1);
        const bool ok = sub == "attack" ? m_alife->AttackAt(place) : m_alife->SetCleared(place, sub == "clear");
        if (!ok) spdlog::warn("Usage: alife clear|unclear|attack <place> (attack: needs a hostile squad camping within reach)");
    } else if (sub == "save") {
        m_alife->SaveState(Wars::kStatePath);
        spdlog::info("[ALife] saved to {}", Wars::kStatePath);
    } else {
        spdlog::warn("Usage: alife [near <place> | events [n] | speed <x> | save]");
    }
}

void GameServer::HandleWarsPresence(HSteamNetConnection conn, const WarsPresenceMsg* msg) {
    if (!m_alife) return;
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) return;
    auto& p = m_warsPresence[it->second.id];
    p.outside = msg->outside != 0;
    p.x = msg->x;
    p.y = msg->y;
    p.z = msg->z;
    p.seen = std::chrono::steady_clock::now();
}

void GameServer::HandleWarsReport(HSteamNetConnection conn, const WarsSquadMsg* msg) {
    if (!m_alife) return;
    uint32_t sender = 0;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(conn);
        if (it == m_clients.end()) return;
        sender = it->second.id;
    }
    Wars::Squad* s = m_alife->Get(msg->squad);
    // Only from the game running it (a last report may come in just after the server took it back).
    if (!s || (s->owner != sender && !(msg->final && s->owner == 0))) return;
    m_alife->Report(msg->squad, std::min<int>(msg->count, Wars::kWarsSquadMaxMembers), msg->hp, msg->x, msg->y, msg->final != 0);
}

// Squads within kOnline of a player who is outdoors go to that player's game (up to kPerPlayer each); a squad
// with no outdoor player within kOffline any more goes back to the simulation. Loaded range is ~8200 units.
void GameServer::WarsOnlineOffline() {
    constexpr float kOnline = 7000.0f, kOffline = 10000.0f;
    constexpr int kPerPlayer = 6;
    const auto now = std::chrono::steady_clock::now();
    std::unordered_map<uint32_t, WarsPresence> present;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        std::unordered_map<uint32_t, bool> connected;
        for (auto& [c, cl] : m_clients) if (cl.isConnected) connected[cl.id] = true;
        for (auto it = m_warsPresence.begin(); it != m_warsPresence.end();) {
            if (!connected.count(it->first)) { it = m_warsPresence.erase(it); continue; }
            if (it->second.outside && now - it->second.seen < std::chrono::seconds(10)) present[it->first] = it->second;
            ++it;
        }
    }
    std::vector<std::pair<float, float>> players;
    for (auto& [id, p] : present) players.emplace_back(p.x, p.y);
    auto send = [&](uint32_t a_client, MessageType a_type, Wars::Squad& a_s) {
        WarsSquadMsg m{};
        m.squad = a_s.id;
        m.faction = static_cast<uint8_t>(a_s.faction);
        m.count = static_cast<uint8_t>(std::min<size_t>(a_s.members.size(), kWarsSquadMax));
        for (int i = 0; i < m.count; ++i) m.hp[i] = a_s.members[i].hp;
        const auto& place = m_alife->PlaceOf(a_s);
        const auto spot = m_alife->CampSpot(a_s, players);   // on the ground, away from players
        m.x = spot.x;
        m.y = spot.y;
        m.z = spot.z;
        m.anchorRef = place.ref;
        if (const auto* goal = m_alife->GoalOf(a_s)) m.goalRef = goal->ref;
        const auto home = WarsLiveSpot(a_s, players);   // where it lives there: its camp, if it has one
        m.gx = home.x;
        m.gy = home.y;
        m.gz = home.z;
        a_s.sentGoal = m_alife->GoalKey(a_s);
        SendMessageToClient(a_client, a_type, &m, sizeof(m));
    };
    // What a squad in a player's game does changed (camp is over: walk on; arrived: camp there).
    auto sendGoal = [&](Wars::Squad& a_s) {
        WarsSquadGoalMsg g{};
        g.squad = a_s.id;
        const auto* goal = m_alife->GoalOf(a_s);
        g.travel = goal ? 1 : 0;
        g.goalRef = goal ? goal->ref : 0;
        const auto spot = WarsLiveSpot(a_s, players);
        g.x = spot.x;
        g.y = spot.y;
        g.z = spot.z;
        a_s.sentGoal = m_alife->GoalKey(a_s);
        SendMessageToClient(a_s.owner, MessageType::WarsSquadGoal, &g, sizeof(g));
        spdlog::info("[ALife] squad #{} in player {}'s game: {}", a_s.id, a_s.owner,
            goal ? fmt::format("walk to {}", goal->name) : fmt::format("camp at {}", m_alife->PlaceOf(a_s).name));
    };
    std::unordered_map<uint32_t, int> owned;
    for (const auto& s : m_alife->Squads()) {
        if (!s.owner) continue;
        auto* sq = m_alife->Get(s.id);
        bool keep = present.count(s.owner) != 0;
        if (keep) {
            keep = false;
            for (auto& [id, p] : present)
                if (std::hypot(p.x - s.x, p.y - s.y) < kOffline) { keep = true; break; }
        }
        if (keep) {
            owned[s.owner]++;
            if (m_alife->GoalKey(s) != s.sentGoal) sendGoal(*sq);
            continue;
        }
        send(s.owner, MessageType::WarsSquadOffline, *sq);
        spdlog::info("[ALife] squad #{} back from player {}'s game", s.id, s.owner);
        m_alife->Release(*sq);
    }
    for (auto& [id, p] : present) {
        for (Wars::Squad* s : m_alife->OfflineNear(p.x, p.y, kOnline)) {
            if (owned[id] >= kPerPlayer) break;
            // Its game spawns it at its place's spot, not where it is on the road: that spot too must be well
            // inside the hand-back range, or it goes straight back.
            if (const auto spot = m_alife->CampSpot(*s, players); std::hypot(spot.x - p.x, spot.y - p.y) > kOnline + 1000.0f) continue;
            // Only its living members go online (their order is the report's order).
            s->members.erase(std::remove_if(s->members.begin(), s->members.end(), [](const Wars::Member& m) { return m.hp <= 0.0f; }), s->members.end());
            if (s->members.size() > static_cast<size_t>(kWarsSquadMax)) s->members.resize(kWarsSquadMax);
            s->owner = id;
            owned[id]++;
            send(id, MessageType::WarsSquadOnline, *s);
            spdlog::info("[ALife] squad #{} ({} {}) -> player {}'s game near {}", s->id, s->Alive(), Wars::FactionName(s->faction), id, m_alife->PlaceOf(*s).name);
        }
    }
}

// Radio chatter to the Wars players: one line per player at most every 12 s - trouble near them first, then events,
// then camp talk; anything older than a minute is dropped. Every line is also in the server log.
void GameServer::WarsChatter() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& c : m_alife->TakeChatter()) {
        spdlog::info("[Radio] {}: {}", c.who, c.text);
        for (auto& [id, p] : m_warsPresence) {
            if (auto t = m_radioTune.find(id); t != m_radioTune.end() && (!t->second.on || !t->second.chatter)) continue;   // radio off
            auto& box = m_radio[id];
            box.pending.push_back({ c, now });
            if (box.pending.size() > 12) box.pending.erase(box.pending.begin());
        }
    }
    for (auto it = m_radio.begin(); it != m_radio.end();) {
        auto pres = m_warsPresence.find(it->first);
        if (pres == m_warsPresence.end()) { it = m_radio.erase(it); continue; }
        auto& box = it->second;
        box.pending.erase(std::remove_if(box.pending.begin(), box.pending.end(), [&](const auto& e) { return now - e.second > std::chrono::seconds(60); }), box.pending.end());
        if (!box.pending.empty() && now - box.last >= std::chrono::seconds(12)) {
            const auto& p = pres->second;
            auto score = [&](const Wars::ALife::Chatter& c) {
                const float d = std::hypot(c.x - p.x, c.y - p.y);
                return c.priority * 100000.0f - d;
            };
            auto best = std::max_element(box.pending.begin(), box.pending.end(), [&](const auto& a, const auto& b) { return score(a.first) < score(b.first); });
            WarsChatterMsg m{};
            m.faction = static_cast<uint8_t>(best->first.faction);
            m.x = best->first.x;
            m.y = best->first.y;
            std::snprintf(m.who, sizeof(m.who), "%s", best->first.who.c_str());
            std::snprintf(m.text, sizeof(m.text), "%s", best->first.text.c_str());
            SendMessageToClient(it->first, MessageType::WarsChatter, &m, sizeof(m));
            box.pending.erase(best);
            box.last = now;
        }
        ++it;
    }
}

// Capturing: a player standing at a cleared place's settlement workbench for kHold seconds with no living enemy
// squad near it takes the place. Their game gets the countdown; every player gets who holds which workbench.
void GameServer::WarsCapture() {
    constexpr float kAt = 450.0f, kEnemies = 2500.0f;
    constexpr int kHold = 30;
    std::unordered_map<uint32_t, WarsPresence> present;
    std::unordered_map<uint32_t, std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& [id, p] : m_warsPresence) present[id] = p;
        for (auto& [conn, cl] : m_clients) names[cl.id] = cl.name;
    }
    const auto& nodes = m_alife->Nodes();
    auto send = [&](uint32_t a_id, int a_node, uint8_t a_state, int a_held) {
        WarsCaptureMsg m{};
        m.place = nodes[a_node].ref;
        m.state = a_state;
        m.held = static_cast<uint8_t>(a_held);
        m.need = kHold;
        std::snprintf(m.name, sizeof(m.name), "%s", nodes[a_node].name.c_str());
        SendMessageToClient(a_id, MessageType::WarsCapture, &m, sizeof(m));
    };
    for (auto& [id, p] : present) {
        auto& hold = m_warsHold[id];
        int at = -1;
        if (p.outside)
            for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
                if (nodes[i].workbench && m_alife->Cleared(i) && m_alife->ClaimOf(i).empty() &&
                    std::hypot(nodes[i].workbenchAt.x - p.x, nodes[i].workbenchAt.y - p.y) < kAt) { at = i; break; }
        bool enemies = false;
        if (at >= 0)
            for (const auto& s : m_alife->Squads())
                if (s.Alive() > 0 && s.faction != Wars::Faction::Trader &&
                    std::hypot(s.x - nodes[at].workbenchAt.x, s.y - nodes[at].workbenchAt.y) < kEnemies) { enemies = true; break; }
        if (at < 0 || enemies) {
            if (hold.node >= 0 && hold.held > 0) send(id, hold.node, 1, hold.held);
            hold = {};
            continue;
        }
        if (hold.node != at) hold = { at, 0 };
        if (++hold.held < kHold) { send(id, at, 0, hold.held); continue; }
        const auto who = names.count(id) ? names[id] : std::string("A wastelander");
        m_alife->SetClaim(at, m_teams ? m_teams->ClaimKey(who) : who);
        send(id, at, 2, kHold);
        spdlog::info("[ALife] {} captured {}", who, nodes[at].name);
        hold = {};
    }
    // Who holds which workbench, to every player (again whenever a claim changes).
    for (auto it = m_claimsSent.begin(); it != m_claimsSent.end();)
        it = present.count(it->first) ? std::next(it) : m_claimsSent.erase(it);
    for (auto& [id, p] : present) {
        auto& had = m_claimsSent[id];
        const uint32_t version = m_alife->ClaimsVersion() * 7919u + (m_teams ? m_teams->Version() : 0u);   // crews change who "we" are
        if (had == version) continue;
        had = version;
        const auto myName = names.count(id) ? names[id] : std::string();
        const auto me = m_teams ? m_teams->ClaimKey(myName) : myName;
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
            if (!nodes[i].workbench) continue;
            WarsClaimMsg c{};
            c.workbench = nodes[i].workbench;
            c.place = nodes[i].ref;
            std::snprintf(c.name, sizeof(c.name), "%s", nodes[i].name.c_str());
            const auto& owner = m_alife->ClaimOf(i);
            c.mine = !owner.empty() && (owner == me || owner == myName) ? 1 : 0;   // a claim taken before joining a crew stays ours
            std::snprintf(c.owner, sizeof(c.owner), "%s", (m_teams ? m_teams->ClaimName(owner) : owner).c_str());
            SendMessageToClient(id, MessageType::WarsClaim, &c, sizeof(c));
        }
    }
}

void GameServer::WarsNotice(uint32_t a_client, const std::string& a_text) {
    WarsNoticeMsg n{};
    std::snprintf(n.text, sizeof(n.text), "%s", a_text.c_str());
    SendMessageToClient(a_client, MessageType::WarsNotice, &n, sizeof(n));
}

// A player picks a side. The name is the server's own record of them, never what the message says.
void GameServer::HandleWarsTeam(HSteamNetConnection conn, const WarsTeamActionMsg* msg) {
    std::string who;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(conn);
        if (it == m_clients.end() || !it->second.isConnected) return;
        who = it->second.name;
        id = it->second.id;
    }
    auto text = [](const char* a_s, size_t a_n) { return std::string(a_s, strnlen(a_s, a_n)); };
    std::string why;
    bool ok = false;
    switch (msg->action) {
    case 1: ok = m_teams->PickSolo(who, why); break;
    case 2: ok = m_teams->PickFaction(who, static_cast<Wars::Allegiance>(msg->arg), why); break;
    case 3: ok = m_teams->Create(who, text(msg->name, sizeof(msg->name)), text(msg->tag, sizeof(msg->tag)), msg->arg, msg->open != 0, why); break;
    case 4: ok = m_teams->Join(who, msg->crew, why); break;
    case 5: ok = m_teams->Leave(who, why); break;
    case 6: ok = m_teams->Answer(who, text(msg->who, sizeof(msg->who)), true, why); break;
    case 7: ok = m_teams->Answer(who, text(msg->who, sizeof(msg->who)), false, why); break;
    case 8: ok = m_teams->SetOpen(who, msg->open != 0, why); break;
    default: return;
    }
    if (!why.empty()) WarsNotice(id, why);
    spdlog::info("[Teams] {}: action {} -> {}{}", who, static_cast<int>(msg->action), ok ? "done" : "refused", why.empty() ? "" : ": " + why);
}

// Everyone's side and every crew, to each player (again whenever anything changes).
void GameServer::WarsTeamsSend() {
    if (!m_teams) return;
    std::vector<std::pair<uint32_t, std::string>> players;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& [conn, cl] : m_clients) if (cl.isConnected && !cl.name.empty()) players.emplace_back(cl.id, cl.name);
    }
    std::unordered_map<uint32_t, bool> here;
    for (auto& [id, n] : players) here[id] = true;
    for (auto it = m_teamsSent.begin(); it != m_teamsSent.end();) it = here.count(it->first) ? std::next(it) : m_teamsSent.erase(it);
    // A player who came or went changes the list too.
    static size_t s_count = 0;
    const uint32_t version = m_teams->Version() * 131u + static_cast<uint32_t>(players.size());
    for (auto& [to, toName] : players) {
        auto& had = m_teamsSent[to];
        if (had == version) continue;
        had = version;
        for (const auto& [cid, c] : m_teams->Crews()) {
            WarsCrewMsg m{};
            m.crew = cid;
            m.colour = c.colour;
            m.open = c.open ? 1 : 0;
            m.members = static_cast<uint8_t>(std::min<size_t>(c.members.size(), 255));
            std::snprintf(m.name, sizeof(m.name), "%s", c.name.c_str());
            std::snprintf(m.tag, sizeof(m.tag), "%s", c.tag.c_str());
            std::snprintf(m.leader, sizeof(m.leader), "%s", c.leader.c_str());
            if (c.leader == toName)
                for (size_t i = 0; i < c.requests.size() && i < 4; ++i) std::snprintf(m.requests[i], sizeof(m.requests[i]), "%s", c.requests[i].c_str());
            SendMessageToClient(to, MessageType::WarsCrew, &m, sizeof(m));
        }
        for (auto& [pid, name] : players) {
            const auto& s = m_teams->SideOf(name);
            WarsTeamInfoMsg m{};
            m.player = pid;
            m.kind = static_cast<uint8_t>(s.kind);
            m.faction = static_cast<uint8_t>(s.faction);
            m.crew = s.kind == Wars::SideKind::Crew ? s.crew : 0;
            if (const auto* c = m.crew ? m_teams->CrewById(m.crew) : nullptr) {
                m.colour = c->colour;
                std::snprintf(m.tag, sizeof(m.tag), "%s", c->tag.c_str());
                std::snprintf(m.crewName, sizeof(m.crewName), "%s", c->name.c_str());
            }
            if (pid == to) m.cooldown = static_cast<uint32_t>(m_teams->CooldownLeft(name));
            SendMessageToClient(to, MessageType::WarsTeamInfo, &m, sizeof(m));
        }
    }
    (void)s_count;
}

Wars::Node::Spot GameServer::WarsLiveSpot(Wars::Squad& a_s, const std::vector<std::pair<float, float>>& a_players) {
    const int node = m_alife->NodeOf(a_s);
    Wars::Node::Spot mid;
    // Where the game's own furniture is (their sandbox uses it), else their own camp, else an NPC spot.
    if (const auto& n = m_alife->Nodes()[node]; n.hasHub) return n.hub;
    if (const auto* c = m_alife->CampAt(node); c && c->faction == a_s.faction && c->level > 0 && m_alife->CampMiddle(node, mid)) return mid;
    return m_alife->CampSpot(a_s, a_players);
}

// Camps to the players near them: within kSee of an outdoor player it's sent (again whenever it changes); past
// kForget it's taken back. Players indoors keep what they have (they'll be back out).
void GameServer::WarsCamps() {
    constexpr float kSee = 9000.0f, kForget = 11500.0f;
    std::unordered_map<uint32_t, WarsPresence> present;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto it = m_campsSent.begin(); it != m_campsSent.end();)
            it = m_warsPresence.count(it->first) ? std::next(it) : m_campsSent.erase(it);   // gone
        for (auto& [id, p] : m_warsPresence) if (p.outside) present[id] = p;
    }
    const auto& nodes = m_alife->Nodes();
    for (auto& [id, p] : present) {
        auto& sent = m_campsSent[id];
        for (const auto& c : m_alife->Camps()) {
            Wars::Node::Spot mid;
            if (!m_alife->CampMiddle(c.node, mid)) continue;
            const float d = std::hypot(mid.x - p.x, mid.y - p.y);
            auto was = sent.find(c.node);
            if (d < kSee && (was == sent.end() || was->second != c.version)) {
                WarsCampMsg m{};
                m.camp = nodes[c.node].ref;
                m.faction = static_cast<uint8_t>(c.faction);
                m.level = static_cast<uint8_t>(std::max(0, c.level));
                const auto items = m_alife->CampItems(c);
                m.count = static_cast<uint8_t>(std::min<size_t>(items.size(), kWarsCampMax));
                for (int i = 0; i < m.count; ++i) m.items[i] = { items[i].form, items[i].x, items[i].y, items[i].z, items[i].rz };
                SendMessageToClient(id, MessageType::WarsCamp, &m, sizeof(m));
                sent[c.node] = c.version;
            } else if (d > kForget && was != sent.end()) {
                WarsCampGoneMsg g{};
                g.camp = nodes[c.node].ref;
                SendMessageToClient(id, MessageType::WarsCampGone, &g, sizeof(g));
                sent.erase(was);
            }
        }
        // Camps that are no more.
        for (auto it = sent.begin(); it != sent.end();) {
            if (m_alife->CampAt(it->first)) { ++it; continue; }
            WarsCampGoneMsg g{};
            g.camp = nodes[it->first].ref;
            SendMessageToClient(id, MessageType::WarsCampGone, &g, sizeof(g));
            it = sent.erase(it);
        }
    }
}

void GameServer::HandleWarsTune(HSteamNetConnection conn, const WarsRadioTuneMsg* msg) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) return;
    auto& t = m_radioTune[it->second.id];
    t.freq = msg->freq;
    t.on = msg->on != 0;
    t.chatter = msg->chatter != 0;
    if (!t.on || !t.chatter) m_radio.erase(it->second.id);
    spdlog::info("[Radio] {} (ID {}): radio {}, {:.1f} MHz, A-Life chatter {}", it->second.name, it->second.id, t.on ? "on" : "off",
        t.freq / 10.0, t.chatter ? "on" : "off");
}

// Push-to-talk voice. On the radio: to everyone whose radio is on and on that frequency, wherever they are. Nearby: to
// everyone else, each game plays it by distance (when both are outdoors and far apart it isn't sent at all).
// Unreliable: late voice is useless voice.
void GameServer::HandleWarsVoice(HSteamNetConnection conn, const uint8_t* payload, uint32_t size) {
    if (size < sizeof(WarsVoiceMsg)) return;
    WarsVoiceMsg head;
    memcpy(&head, payload, sizeof(head));
    if (head.bytes > kWarsVoiceMaxBytes || sizeof(head) + head.bytes > size) return;
    if (!std::isfinite(head.x) || !std::isfinite(head.y) || !std::isfinite(head.z)) return;
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(conn);
    if (it == m_clients.end() || !it->second.isConnected) return;
    const uint32_t sender = it->second.id;
    const bool radio = (head.flags & kVoiceRadio) != 0;
    if (radio) {
        auto t = m_radioTune.find(sender);
        if (t != m_radioTune.end() && !t->second.on) return;   // its radio is off
    }
    const auto from = m_warsPresence.find(sender);
    MessageHeader header;
    header.type = MessageType::WarsVoice;
    header.size = static_cast<uint32_t>(sizeof(MessageHeader) + sizeof(head) + head.bytes);
    header.timestamp = 0;
    std::vector<uint8_t> buffer(header.size);
    head.sender = sender;
    memcpy(buffer.data(), &header, sizeof(header));
    memcpy(buffer.data() + sizeof(header), &head, sizeof(head));
    memcpy(buffer.data() + sizeof(header) + sizeof(head), payload + sizeof(head), head.bytes);
    for (const auto& [c, cl] : m_clients) {
        if (!cl.isConnected || cl.id == sender) continue;
        if (radio) {
            auto t = m_radioTune.find(cl.id);
            if (t == m_radioTune.end() || !t->second.on || t->second.freq != head.freq) continue;
        } else if (auto to = m_warsPresence.find(cl.id); from != m_warsPresence.end() && to != m_warsPresence.end() &&
                   from->second.outside && to->second.outside &&
                   std::hypot(from->second.x - to->second.x, from->second.y - to->second.y) > 8000.0f) {
            continue;
        }
        m_sockets->SendMessageToConnection(c, buffer.data(), (uint32_t)buffer.size(), k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
    }
}
