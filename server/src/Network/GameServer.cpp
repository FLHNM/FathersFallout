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

    if (msg.protocolVersion != PROTOCOL_VERSION) {
        spdlog::warn("[Server] Connection rejected: protocol {} (server {})", msg.protocolVersion, PROTOCOL_VERSION);
        const std::string why = "Different FathersFallout version (host protocol " + std::to_string(PROTOCOL_VERSION) +
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

    // Positions and NPC states: unreliable, newest wins (a lost one must not hold up the rest).
    const int flags = (type == MessageType::PlayerPosition || type == MessageType::NpcState)
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
