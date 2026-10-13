#include "GameServer.h"
#include "../Heartbeat.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>

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

    // Permitir conexiones IP sin autenticacion de Steam.
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

    // Registrar el callback global de cambios de estado de conexion.
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

    // Hilo de heartbeat al master server (si esta configurado).
    if (!m_master.empty()) {
        std::thread([this]() {
            while (m_running) {
                uint32_t players;
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    players = GetPlayerCount();
                }
                Heartbeat::Send(m_master, m_name, m_port, players, m_maxPlayers);
                // dormir 10s comprobando m_running cada 100ms para salir rapido
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

        // Estado periodico cada 30 segundos (estilo Rust).
        if (std::chrono::duration<float>(now - m_lastStatus).count() >= 30.0f) {
            PrintStatus();
            m_lastStatus = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void GameServer::Update() {
    // Procesa estados de conexion (dispara OnConnStatusChanged) y mensajes.
    m_sockets->RunCallbacks();
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

void GameServer::HandleConnectionRequest(HSteamNetConnection conn, const ConnectionRequestMsg* msg) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    if (msg->protocolVersion != PROTOCOL_VERSION) {
        spdlog::warn("[Server] Connection rejected: protocol {} (server {})", msg->protocolVersion, PROTOCOL_VERSION);
        m_sockets->CloseConnection(conn, 0, "Protocol version mismatch", false);
        return;
    }

    if (m_clients.size() >= m_maxPlayers) {
        spdlog::warn("[Server] Connection rejected: server full");
        m_sockets->CloseConnection(conn, 0, "Server full", false);
        return;
    }

    ClientInfo client{};
    client.id = m_nextClientId++;
    client.connection = conn;
    client.name = msg->playerName;
    client.isConnected = true;
    client.lastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;

    m_clients[conn] = client;

    ConnectionAcceptedMsg acceptMsg{};
    acceptMsg.playerId = client.id;
    acceptMsg.currentPlayers = GetPlayerCount();
    acceptMsg.maxPlayers = m_maxPlayers;

    // Enviamos el ConnectionAccepted con su cabecera para que el cliente lo entienda.
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

        // Sobreescribimos el playerId con el ID autoritativo del servidor.
        // Asi no dependemos de que el cliente lo ponga bien (el simulador no lo
        // pone y saldria como "jugador 0"), y evitamos suplantaciones.
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
        BroadcastMessage(MessageType::ChatMessage, &broadcastMsg, sizeof(broadcastMsg), 0);
    }
}

void GameServer::HandleNpcState(HSteamNetConnection conn, const NpcStateMsg* msg) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(conn);
    if (it == m_clients.end()) return;
    // Reenviar tal cual al resto (lo manda el host). El servidor no interpreta el NPC.
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

    // Guardar con el id autoritativo del servidor.
    PlayerAppearanceMsg mine = *msg;
    mine.playerId = it->second.id;
    it->second.appearance = mine;
    it->second.hasAppearance = true;

    // 1. Repartir mi apariencia a los demas.
    BroadcastMessage(MessageType::PlayerAppearance, &mine, sizeof(mine), it->second.id);

    // 2. Enviar al recien llegado las apariencias ya conocidas (directo a su conn,
    //    sin SendMessageToClient para no re-bloquear el mutex).
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

    for (const auto& [conn, client] : m_clients) {
        if (client.isConnected && client.id != excludeId) {
            m_sockets->SendMessageToConnection(conn, buffer.data(), (uint32_t)buffer.size(),
                k_nSteamNetworkingSend_Reliable, nullptr);
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

void GameServer::ProcessMessages() {
    SteamNetworkingMessage_t* msgs[32];
    int msgCount = m_sockets->ReceiveMessagesOnPollGroup(m_pollGroup, msgs, 32);

    for (int i = 0; i < msgCount; ++i) {
        SteamNetworkingMessage_t* msg = msgs[i];
        if (msg->m_cbSize >= (int)sizeof(MessageHeader)) {
            const auto* header = reinterpret_cast<const MessageHeader*>(msg->m_pData);
            const void* payload = reinterpret_cast<const uint8_t*>(msg->m_pData) + sizeof(MessageHeader);

            switch (header->type) {
                case MessageType::ConnectionRequest:
                    HandleConnectionRequest(msg->m_conn, reinterpret_cast<const ConnectionRequestMsg*>(payload));
                    break;
                case MessageType::PlayerPosition:
                    HandlePlayerPosition(msg->m_conn, reinterpret_cast<const PlayerPositionMsg*>(payload));
                    break;
                case MessageType::NpcState:
                    HandleNpcState(msg->m_conn, reinterpret_cast<const NpcStateMsg*>(payload));
                    break;
                case MessageType::PlayerAppearance:
                    HandlePlayerAppearance(msg->m_conn, reinterpret_cast<const PlayerAppearanceMsg*>(payload));
                    break;
                case MessageType::ChatMessage:
                    HandleChatMessage(msg->m_conn, reinterpret_cast<const ChatMessageMsg*>(payload));
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
}

void GameServer::OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info) {
    GetInstance().OnConnStatusChanged(info);
}

void GameServer::OnConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting: {
            // Nueva conexion entrante: aceptar y meterla en el poll group.
            if (m_sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                m_sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                spdlog::warn("[Server] Failed to accept connection");
                break;
            }
            m_sockets->SetConnectionPollGroup(info->m_hConn, m_pollGroup);
            spdlog::info("[Server] Connection accepted, waiting for handshake");
            break;
        }

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
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
                // Avisar a los demas para que borren su cuerpo AL INSTANTE.
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

// ===================== Consola de administracion =====================

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
            m_clients.erase(it);
            // Avisar a los demas para que borren su cuerpo AL INSTANTE.
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
