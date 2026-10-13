#ifndef F4MPSERVER_GAMESERVER_H
#define F4MPSERVER_GAMESERVER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <chrono>

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

enum class MessageType : uint16_t {
    Invalid = 0,
    ConnectionRequest = 1,
    ConnectionAccepted = 2,
    ConnectionRejected = 3,
    Disconnect = 4,
    PlayerPosition = 10,
    PlayerRotation = 11,
    PlayerAnimation = 12,
    PlayerAppearance = 13,
    ChatMessage = 20,
    DamageDealt = 30,
    EntitySpawn = 40,
    EntityState = 41,
    NpcState = 50,      // el host difunde el estado de un NPC; el servidor lo reenvia
    CoopFirst = 60,     // >= 60: co-op sync messages, relayed generically (see RelayCoop)
};

#pragma pack(push, 1)

struct MessageHeader {
    MessageType type;
    uint32_t size;
    uint64_t timestamp;
};

struct ConnectionRequestMsg {
    char playerName[64];
    uint32_t protocolVersion;
};

struct ConnectionAcceptedMsg {
    uint32_t playerId;
    uint32_t currentPlayers;
    uint32_t maxPlayers;
};

// Se envia (tipo Disconnect) cuando un jugador se va o es expulsado, para que
// los demas clientes eliminen su cuerpo AL INSTANTE (sin esperar al timeout).
struct PlayerLeftMsg {
    uint32_t playerId;
};

struct PlayerPositionMsg {
    uint32_t playerId;
    float x, y, z;
    float pitch;         // look up/down (client's velocityX slot, relayed as is)
    float velocityY, velocityZ;
    float angleZ;        // heading de vista
    float speed;         // velocidad de movimiento
    float moveDir;       // direccion del movimiento relativa al facing
    uint32_t cellId;
    bool isMoving;
    bool isRunning;
    bool isSprinting;
    bool isSneaking;
    bool isJumping;
    bool weaponDrawn;   // arma/magia en mano (debe coincidir con la del cliente)
};

struct PlayerAppearanceMsg {
    uint32_t playerId;
    uint32_t raceFormID;
    uint32_t hairColor;
    int32_t  sex;
    uint8_t  skinR, skinG, skinB, skinA;
    uint8_t  numHeadParts;
    uint8_t  _pad[3];
    uint32_t headParts[16];
};

struct ChatMessageMsg {
    uint32_t senderId;
    char message[256];
    uint8_t channel;
};

// Estado de un NPC difundido por el host (debe coincidir con la del cliente). El
// servidor solo la REENVIA a los demas; no interpreta su contenido.
constexpr uint8_t kNpcWeaponDrawn = 1, kNpcRunning = 2;
struct NpcStateMsg {
    uint32_t formId;
    float x, y, z;
    float angleZ;
    float speed;
    bool isMoving;
    bool isSneaking;
    bool isDead;
    uint8_t flags;       // kNpcWeaponDrawn | kNpcRunning (was padding: same size as before)
    float health;      // must match client-ng NpcStateMsg byte-for-byte
    uint32_t ownerId;
};

struct DamageDealtMsg {
    uint32_t attackerId;
    uint32_t targetId;
    uint64_t targetFormId;
    float damageAmount;
    uint8_t damageType;
    bool isCritical;
};

#pragma pack(pop)

constexpr uint32_t PROTOCOL_VERSION = 12;   // must match client-ng/src/network/NetworkClient.h
constexpr uint16_t DEFAULT_SERVER_PORT = 7779;
constexpr uint32_t MAX_PLAYERS = 64;

struct ClientInfo {
    uint32_t id;
    HSteamNetConnection connection;
    std::string name;
    float x, y, z;
    float yaw, pitch, roll;
    uint32_t cellId;
    bool isConnected;
    double lastUpdate;
    bool hasAppearance = false;
    PlayerAppearanceMsg appearance{};
};

class GameServer {
public:
    static GameServer& GetInstance();

    bool Initialize(uint16_t port = DEFAULT_SERVER_PORT);
    void Shutdown();

    void Run();
    void Update();

    bool IsRunning() const { return m_running; }
    uint32_t GetPlayerCount() const;

    void SetMaxPlayers(uint32_t n) { if (n > 0) m_maxPlayers = n; }
    void SetName(const std::string& name) { m_name = name; }
    void SetMaster(const std::string& url) { m_master = url; }
    void SetTickRate(int r) { if (r >= 1 && r <= 256) m_tickRate = r; }
    void Stop() { m_running = false; }

    // Encola un comando de consola para procesarlo en el hilo principal.
    void QueueCommand(const std::string& cmd);

private:
    GameServer() = default;

    // --- Consola de administracion ---
    void ProcessConsoleCommands();
    void HandleCommand(const std::string& line);
    void CmdList();
    void CmdKick(uint32_t id);
    void CmdSay(const std::string& msg);
    void PrintStatus();
    double UptimeSeconds() const;

    void HandleConnectionRequest(HSteamNetConnection conn, const ConnectionRequestMsg* msg);
    void HandlePlayerPosition(HSteamNetConnection conn, const PlayerPositionMsg* msg);
    void HandleNpcState(HSteamNetConnection conn, const NpcStateMsg* msg);
    void HandlePlayerAppearance(HSteamNetConnection conn, const PlayerAppearanceMsg* msg);
    void HandleChatMessage(HSteamNetConnection conn, const ChatMessageMsg* msg);
    void RelayCoop(HSteamNetConnection conn, MessageType type, const void* payload, uint32_t size);

    void BroadcastMessage(MessageType type, const void* data, uint32_t size, uint32_t excludeId = 0);
    void SendMessageToClient(uint32_t clientId, MessageType type, const void* data, uint32_t size);

    void ProcessMessages();

    // Callback global de GNS -> instancia singleton.
    static void OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info);
    void OnConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

    ISteamNetworkingSockets* m_sockets = nullptr;
    HSteamListenSocket m_listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_pollGroup = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, ClientInfo> m_clients;
    std::mutex m_clientsMutex;

    uint32_t m_nextClientId = 1;
    uint32_t m_maxPlayers = MAX_PLAYERS;
    uint16_t m_port = DEFAULT_SERVER_PORT;
    int m_tickRate = 60;   // ticks/s (configurable con "tick-rate")
    std::string m_name = "FathersFallout Server";
    std::string m_master;  // URL del master server (vacio = desactivado)

    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastStatus;

    std::queue<std::string> m_commands;
    std::mutex m_cmdMutex;

    bool m_running = false;
    bool m_initialized = false;
};

#endif
