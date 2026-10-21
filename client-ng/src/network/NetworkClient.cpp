// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Based on F4MP by Jous99 (public domain). Licensed under the GNU
// GPL v3 with additional terms: see LICENSE and NOTICE.

#include "network/NetworkClient.h"
#include "network/SteamLink.h"
#include "sync/LoadOrder.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace Network {

NetworkClient& NetworkClient::GetInstance() {
    static NetworkClient instance;
    return instance;
}

bool NetworkClient::Initialize() {
    if (m_initialized) return true;

    REX::INFO("[Network] initializing GameNetworkingSockets...");

    SteamNetworkingErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        REX::ERROR("[Network] could not start GameNetworkingSockets: {}", errMsg);
        return false;
    }

    m_sockets = SteamNetworkingSockets();
    m_utils = SteamNetworkingUtils();
    if (!m_sockets || !m_utils) {
        REX::ERROR("[Network] could not get the networking interfaces");
        return false;
    }

    m_utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
    m_utils->SetGlobalCallback_SteamNetConnectionStatusChanged(&NetworkClient::OnConnStatusChangedStatic);

    m_initialized = true;
    REX::INFO("[Network] GameNetworkingSockets initialized");
    SteamLink::Init();   // Steam join, if this copy of the game runs on Steam
    return true;
}

void NetworkClient::Shutdown() {
    if (m_connection != k_HSteamNetConnection_Invalid) {
        Disconnect();
    }
    if (m_initialized) {
        GameNetworkingSockets_Kill();
        m_initialized = false;
    }
}

bool NetworkClient::Connect(const std::string& address, uint16_t port) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        REX::WARN("[Network] networking is not initialized");
        return false;
    }
    if (m_connection != k_HSteamNetConnection_Invalid) {
        REX::WARN("[Network] a connection is already in progress");
        return false;
    }

    // "steam" / "steam:<friend code or name>": the host's Steam account instead of an IP.
    if (_strnicmp(address.c_str(), "steam", 5) == 0 && (address.size() == 5 || address[5] == ':')) {
        std::string name, err;
        const uint64_t host = SteamLink::FindHost(address.size() > 6 ? address.substr(6) : "", name, err);
        if (!host) {
            std::lock_guard<std::mutex> l(m_statusMutex);
            m_lastError = err;
            m_target = "Steam";
            return false;
        }
        m_connection = SteamLink::Connect(host);
        if (m_connection == k_HSteamNetConnection_Invalid) {
            std::lock_guard<std::mutex> l(m_statusMutex);
            m_lastError = "Steam could not open a connection";
            return false;
        }
        m_steam = true;
        m_steamState = 1;
        {
            std::lock_guard<std::mutex> l(m_statusMutex);
            m_target = name + " (Steam)";
            m_lastError.clear();
        }
        REX::INFO("[Network] connecting to {} through Steam...", name);
        return true;
    }
    m_steam = false;

    char addrStr[64];
    snprintf(addrStr, sizeof(addrStr), "%s:%d", address.c_str(), port);

    SteamNetworkingIPAddr serverAddr;
    serverAddr.Clear();
    if (!serverAddr.ParseString(addrStr)) {
        REX::ERROR("[Network] invalid address: {}", addrStr);
        return false;
    }

    m_connection = m_sockets->ConnectByIPAddress(serverAddr, 0, nullptr);
    if (m_connection == k_HSteamNetConnection_Invalid) {
        REX::ERROR("[Network] could not create a connection to {}", addrStr);
        return false;
    }

    {
        std::lock_guard<std::mutex> l(m_statusMutex);
        m_target = addrStr;
        m_lastError.clear();
    }
    REX::INFO("[Network] connecting to {}...", addrStr);
    return true;
}

void NetworkClient::Disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_connection != k_HSteamNetConnection_Invalid) {
        if (m_steam) SteamLink::Close(m_connection);
        else m_sockets->CloseConnection(m_connection, 0, "Disconnecting", false);
        m_connection = k_HSteamNetConnection_Invalid;
    }
    m_connected = false;
    m_playerId = 0;
    {
        std::lock_guard<std::mutex> rlock(m_remoteMutex);
        m_remotePlayers.clear();   // forget remote players, or ghosts stay
        m_remoteLastSeen.clear();
        m_remoteAppearance.clear();
        m_remoteNpcs.clear();
        m_npcLastSeen.clear();
        m_remoteStamp.clear();
        m_npcStamp.clear();
    }
    REX::INFO("[Network] disconnected");
}

void NetworkClient::SendPacket(MessageType type, const void* data, uint32_t size) {
    if (m_connection == k_HSteamNetConnection_Invalid) return;

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

    const int flags = IsStreamMessage(type) ? kSendStream : kSendEvent;
    if (m_steam) SteamLink::Send(m_connection, buffer.data(), (uint32_t)buffer.size(), flags);
    else m_sockets->SendMessageToConnection(m_connection, buffer.data(), (uint32_t)buffer.size(), flags, nullptr);
}

void NetworkClient::SendConnectionRequest() {
    ConnectionRequestMsg req{};
    strncpy(req.playerName, m_playerName.c_str(), sizeof(req.playerName) - 1);
    req.protocolVersion = PROTOCOL_VERSION;
    strncpy(req.password, m_password.c_str(), sizeof(req.password) - 1);
    SendPacket(MessageType::ConnectionRequest, &req, sizeof(req));
    REX::INFO("[Network] handshake sent as '{}'", m_playerName);
}

void NetworkClient::OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info) {
    GetInstance().OnConnStatusChanged(info);
}

void NetworkClient::OnConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
    // Connections that aren't ours: the Steam relay's links into the server (host).
    if (!m_ownSteamEvent && (m_steam || info->m_hConn != m_connection)) {
        SteamLink::OnGnsStatus(info);
        return;
    }
    switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connected:
            REX::INFO("[Network] transport connected, sending handshake...");
            SendConnectionRequest();
            break;

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            REX::WARN("[Network] connection closed: {}", info->m_info.m_szEndDebug);
            {
                std::lock_guard<std::mutex> l(m_statusMutex);
                m_lastError = info->m_info.m_szEndDebug[0] ? info->m_info.m_szEndDebug : "connection closed";
            }
            m_lastCloseWasAttempt = !m_connected;
            if (m_connection != k_HSteamNetConnection_Invalid) {
                if (m_steam) SteamLink::Close(m_connection);
                else m_sockets->CloseConnection(m_connection, 0, nullptr, false);
                m_connection = k_HSteamNetConnection_Invalid;
            }
            m_connected = false;
            m_playerId = 0;
            {
                std::lock_guard<std::mutex> rlock(m_remoteMutex);
                m_remotePlayers.clear();   // forget remote players when the connection closes
                m_remoteLastSeen.clear();
                m_remoteAppearance.clear();
        m_remoteNpcs.clear();
        m_npcLastSeen.clear();
        m_remoteStamp.clear();
        m_npcStamp.clear();
            }
            break;

        default:
            break;
    }
}

void NetworkClient::PumpMessages() {
    if (!m_initialized) return;

    m_sockets->RunCallbacks();

    if (m_connection == k_HSteamNetConnection_Invalid) return;

    // Steam: no status callbacks here, the state is polled and handled like the GNS callback.
    if (m_steam) {
        SteamLink::RunCallbacks();
        std::string why;
        const int st = SteamLink::State(m_connection, why);
        if (st != m_steamState) {
            const int before = m_steamState;
            m_steamState = st;
            SteamNetConnectionStatusChangedCallback_t fake{};
            fake.m_hConn = m_connection;
            fake.m_info.m_eState = static_cast<ESteamNetworkingConnectionState>(st);
            fake.m_eOldState = static_cast<ESteamNetworkingConnectionState>(before);
            strncpy_s(fake.m_info.m_szEndDebug, why.empty() ? "Steam connection lost" : why.c_str(), _TRUNCATE);
            if (st == 0) fake.m_info.m_eState = k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
            m_ownSteamEvent = true;       // the regular handler takes it (it closes through SteamLink)
            OnConnStatusChanged(&fake);
            m_ownSteamEvent = false;
        }
        if (m_connection == k_HSteamNetConnection_Invalid) return;
    }

    // Everything that's waiting, not 16 per pass: a busy area (the host's 128 NPCs several times a second
    // + positions) queued faster than that and every body/NPC on screen fell further behind.
    for (int batch = 0; batch < 64; ++batch) {
    SteamNetworkingMessage_t* msgs[64];
    int msgCount = m_steam ? SteamLink::Receive(m_connection, msgs, 64)
                           : m_sockets->ReceiveMessagesOnConnection(m_connection, msgs, 64);
    if (msgCount <= 0) break;
    for (int i = 0; i < msgCount; ++i) {
        SteamNetworkingMessage_t* msg = msgs[i];
        if (msg->m_cbSize >= (int)sizeof(MessageHeader)) {
            const auto* header = reinterpret_cast<const MessageHeader*>(msg->m_pData);
            const void* payload = reinterpret_cast<const uint8_t*>(msg->m_pData) + sizeof(MessageHeader);

            if (header->type == MessageType::ConnectionAccepted) {
                if ((size_t)msg->m_cbSize < sizeof(MessageHeader) + sizeof(ConnectionAcceptedMsg)) { msg->Release(); continue; }
                const auto* accept = reinterpret_cast<const ConnectionAcceptedMsg*>(payload);
                m_playerId = accept->playerId;
                m_connected = true;
                REX::INFO("[Network] connected! ID {}, players {}/{}",
                    m_playerId, accept->currentPlayers, accept->maxPlayers);
            } else if (header->type == MessageType::ConnectionRejected) {
                REX::ERROR("[Network] connection rejected by the server");
                std::lock_guard<std::mutex> l(m_statusMutex);
                m_lastError = "rejected by the server";
            } else if (header->type == MessageType::PlayerPosition) {
                // Only a COMPLETE struct is used: an older version sends fewer bytes, which are ignored
                // (better than reading misaligned garbage).
                const size_t payloadSize = (size_t)msg->m_cbSize - sizeof(MessageHeader);
                if (payloadSize < sizeof(PlayerPositionMsg)) {
                    static bool warned = false;
                    if (!warned) { warned = true;
                        REX::WARN("[Network] old position packet ({} < {} bytes): the other player runs an older version, update it",
                            payloadSize, sizeof(PlayerPositionMsg)); }
                    msg->Release();
                    continue;
                }
                const auto* pos = reinterpret_cast<const PlayerPositionMsg*>(payload);
                if (!SanePosition(pos->x, pos->y, pos->z) || !std::isfinite(pos->angleZ) || !std::isfinite(pos->pitch) ||
                    !std::isfinite(pos->speed) || !std::isfinite(pos->moveDir)) { msg->Release(); continue; }
                if (pos->playerId != m_playerId) {  // ignorar la nuestra
                    std::lock_guard<std::mutex> lock(m_remoteMutex);
                    // Unreliable: an older packet can arrive after a newer one. The server stamps each.
                    auto& last = m_remoteStamp[pos->playerId];
                    if (header->timestamp && header->timestamp < last) { msg->Release(); continue; }
                    last = header->timestamp;
                    bool isNew = m_remotePlayers.find(pos->playerId) == m_remotePlayers.end();
                    auto& stored = m_remotePlayers[pos->playerId];
                    stored = *pos;
                    stored.cellId = F4MP::LoadOrder::ToLocal(pos->playerId, pos->cellId);   // their cell/worldspace in our numbering
                    m_remoteLastSeen[pos->playerId] = std::chrono::steady_clock::now();
                    if (isNew) {
                        REX::INFO("[Network] new player {} at x={:.0f} y={:.0f} z={:.0f}",
                            pos->playerId, pos->x, pos->y, pos->z);
                    }
                }
            } else if (header->type == MessageType::PlayerAppearance) {
                const size_t payloadSize = (size_t)msg->m_cbSize - sizeof(MessageHeader);
                if (payloadSize >= sizeof(PlayerAppearanceMsg)) {
                    const auto* ap = reinterpret_cast<const PlayerAppearanceMsg*>(payload);
                    if (ap->playerId != m_playerId) {
                        std::lock_guard<std::mutex> lock(m_remoteMutex);
                        m_remoteAppearance[ap->playerId] = *ap;
                        REX::INFO("[Network] appearance of player {}: sex={} race={:#x} hair={:#x} headparts={}",
                            ap->playerId, ap->sex, ap->raceFormID, ap->hairColor, ap->numHeadParts);
                    }
                }
            } else if (header->type == MessageType::Disconnect) {
                // The server says a player left: forget them now so their body goes at once (no 5 s timeout).
                const size_t payloadSize = (size_t)msg->m_cbSize - sizeof(MessageHeader);
                if (payloadSize >= sizeof(PlayerLeftMsg)) {
                    const auto* left = reinterpret_cast<const PlayerLeftMsg*>(payload);
                    std::lock_guard<std::mutex> lock(m_remoteMutex);
                    m_remotePlayers.erase(left->playerId);
                    m_remoteLastSeen.erase(left->playerId);
                    m_remoteAppearance.erase(left->playerId);
                    F4MP::LoadOrder::Forget(left->playerId);
                    REX::INFO("[Network] player {} disconnected: body removed", left->playerId);
                }
            } else if (header->type == MessageType::NpcState) {
                // The host's NPC state: kept by FormID.
                const size_t payloadSize = (size_t)msg->m_cbSize - sizeof(MessageHeader);
                if (payloadSize >= sizeof(NpcStateMsg)) {
                    NpcStateMsg translated = *reinterpret_cast<const NpcStateMsg*>(payload);
                    const auto* npc = &translated;
                    if (!SanePosition(npc->x, npc->y, npc->z) || !std::isfinite(npc->angleZ) || !std::isfinite(npc->speed) ||
                        !std::isfinite(npc->health)) { msg->Release(); continue; }
                    // The owner's FormID in our numbering (other mods/Creation Club content): 0 = not in our game.
                    translated.formId = F4MP::LoadOrder::ToLocal(npc->ownerId, npc->formId);
                    if (!translated.formId) { msg->Release(); continue; }
                    std::lock_guard<std::mutex> lock(m_remoteMutex);
                    const uint64_t key = (static_cast<uint64_t>(npc->ownerId) << 32) | npc->formId;
                    auto& last = m_npcStamp[key];
                    if (header->timestamp && header->timestamp < last) { msg->Release(); continue; }
                    last = header->timestamp;
                    m_remoteNpcs[key] = *npc;
                    m_npcLastSeen[key] = std::chrono::steady_clock::now();
                }
            } else if (static_cast<uint16_t>(header->type) >= static_cast<uint16_t>(MessageType::CoopFirst)) {
                // Co-op sync: queue the raw payload; CoopSync applies it on the main thread.
                const size_t payloadSize = (size_t)msg->m_cbSize - sizeof(MessageHeader);
                if (payloadSize > kMaxCoopPayload) { msg->Release(); continue; }
                CoopInboxItem item{ header->type, {}, std::chrono::steady_clock::now() };
                item.payload.assign(static_cast<const uint8_t*>(payload), static_cast<const uint8_t*>(payload) + payloadSize);
                std::lock_guard<std::mutex> lock(m_coopMutex);
                m_coopInbox.push_back(std::move(item));
            }
        }
        msg->Release();
    }
    if (msgCount < 64) break;
    }
}

bool NetworkClient::GetAppearance(uint32_t playerId, PlayerAppearanceMsg& out) {
    std::lock_guard<std::mutex> lock(m_remoteMutex);
    auto it = m_remoteAppearance.find(playerId);
    if (it == m_remoteAppearance.end()) return false;
    out = it->second;
    return true;
}

std::unordered_map<uint32_t, PlayerPositionMsg> NetworkClient::GetRemotePlayers() {
    std::lock_guard<std::mutex> lock(m_remoteMutex);
    // Forget players silent for >5 s (dropped without a goodbye).
    const auto clockNow = std::chrono::steady_clock::now();
    for (auto it = m_remotePlayers.begin(); it != m_remotePlayers.end();) {
        auto seen = m_remoteLastSeen.find(it->first);
        if (seen != m_remoteLastSeen.end() &&
            std::chrono::duration<float>(clockNow - seen->second).count() > 5.0f) {
            m_remoteLastSeen.erase(seen);
            it = m_remotePlayers.erase(it);
        } else {
            ++it;
        }
    }
    return m_remotePlayers;
}

std::unordered_map<uint64_t, NpcStateMsg> NetworkClient::GetRemoteNpcs() {
    std::lock_guard<std::mutex> lock(m_remoteMutex);
    // Forget NPCs the host hasn't sent for >3 s (out of range, or the host is gone).
    const auto clockNow = std::chrono::steady_clock::now();
    for (auto it = m_remoteNpcs.begin(); it != m_remoteNpcs.end();) {
        auto seen = m_npcLastSeen.find(it->first);
        if (seen != m_npcLastSeen.end() &&
            std::chrono::duration<float>(clockNow - seen->second).count() > 3.0f) {
            m_npcLastSeen.erase(seen);
            it = m_remoteNpcs.erase(it);
        } else {
            ++it;
        }
    }
    return m_remoteNpcs;
}

std::vector<CoopInboxItem> NetworkClient::DrainCoopInbox() {
    std::lock_guard<std::mutex> lock(m_coopMutex);
    std::vector<CoopInboxItem> out;
    out.swap(m_coopInbox);
    return out;
}

}
