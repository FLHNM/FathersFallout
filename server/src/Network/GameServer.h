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
    NpcState = 50,      // an NPC's state from its owner; the server relays it
    CoopFirst = 60,     // >= 60: co-op sync messages, relayed generically (see RelayCoop)
    CoopVoice = 119,    // push-to-talk voice: relayed like the rest, but unreliable (late voice is useless voice)
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
    char password[32];   // Update 23 (must match the client); older games send only the first 68 bytes
};
constexpr size_t kConnectionRequestV1Size = 68;

struct ConnectionAcceptedMsg {
    uint32_t playerId;
    uint32_t currentPlayers;
    uint32_t maxPlayers;
};

// Sent (type Disconnect) when a player leaves or is kicked, so the others remove their body AT ONCE
// (no waiting for the timeout).
struct PlayerLeftMsg {
    uint32_t playerId;
};

struct PlayerPositionMsg {
    uint32_t playerId;
    float x, y, z;
    float pitch;         // look up/down (client's velocityX slot, relayed as is)
    uint32_t extraFlags; // client's kPos* bits (Pip-Boy light...), relayed as is; was the unused velocityY
    float velocityZ;
    float angleZ;        // view heading
    float speed;         // movement speed
    float moveDir;       // movement direction relative to facing
    uint32_t cellId;
    bool isMoving;
    bool isRunning;
    bool isSprinting;
    bool isSneaking;
    bool isJumping;
    bool weaponDrawn;   // weapon out (must match the client)
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

// An NPC's state from the host (must match the client byte for byte). The server only RELAYS it.
constexpr uint8_t kNpcWeaponDrawn = 1, kNpcRunning = 2, kNpcCompanion = 4;   // companion: its player's, wherever it is
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

constexpr uint32_t PROTOCOL_VERSION = 17;   // must match client-ng/src/network/NetworkClient.h
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
    void SetPassword(const std::string& pw) { m_password = pw.substr(0, 31); }   // "" = anyone can join
    void Stop() { m_running = false; }

    // Queues a console command for the main thread.
    void QueueCommand(const std::string& cmd);

private:
    GameServer() = default;

    // --- Admin console ---
    void ProcessConsoleCommands();
    void HandleCommand(const std::string& line);
    void CmdList();
    void CmdKick(uint32_t id);
    void CmdSay(const std::string& msg);
    void PrintStatus();
    double UptimeSeconds() const;

    void HandleConnectionRequest(HSteamNetConnection conn, const ConnectionRequestMsg& msg, bool hasPassword);
    void Refuse(HSteamNetConnection conn, int reason, const char* why);
    bool OverRateLimit(HSteamNetConnection conn);
    void DropStaleHandshakes();
    void HandlePlayerPosition(HSteamNetConnection conn, const PlayerPositionMsg* msg);
    void HandleNpcState(HSteamNetConnection conn, const NpcStateMsg* msg);
    void HandlePlayerAppearance(HSteamNetConnection conn, const PlayerAppearanceMsg* msg);
    void HandleChatMessage(HSteamNetConnection conn, const ChatMessageMsg* msg);
    void RelayCoop(HSteamNetConnection conn, MessageType type, const void* payload, uint32_t size);

    void BroadcastMessage(MessageType type, const void* data, uint32_t size, uint32_t excludeId = 0);
    void SendMessageToClient(uint32_t clientId, MessageType type, const void* data, uint32_t size);

    void ProcessMessages();

    // GNS's global callback -> the singleton.
    static void OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info);
    void OnConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

    ISteamNetworkingSockets* m_sockets = nullptr;
    HSteamListenSocket m_listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_pollGroup = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, ClientInfo> m_clients;
    // Connections that haven't sent their handshake yet (closed after kHandshakeSeconds).
    std::unordered_map<HSteamNetConnection, std::chrono::steady_clock::time_point> m_pending;
    // Wrong passwords per remote address: after kMaxBadPasswords in a minute, that address waits a minute.
    struct BadTries { int count = 0; std::chrono::steady_clock::time_point first, lockedUntil; };
    std::unordered_map<std::string, BadTries> m_badTries;
    // Messages per connection in the current second (a flood is dropped, not relayed to everyone).
    struct Rate { std::chrono::steady_clock::time_point windowStart; uint32_t count = 0; uint32_t dropped = 0; };
    std::unordered_map<HSteamNetConnection, Rate> m_rates;
    std::mutex m_clientsMutex;

    uint32_t m_nextClientId = 1;
    uint32_t m_maxPlayers = MAX_PLAYERS;
    uint16_t m_port = DEFAULT_SERVER_PORT;
    int m_tickRate = 60;   // ticks/s (config "tick-rate")
    std::string m_name = "FathersFallout Server";
    std::string m_master;  // master server URL (empty = off)
    std::string m_password;   // join password ("" = none)

    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastStatus;

    std::queue<std::string> m_commands;
    std::mutex m_cmdMutex;

    bool m_running = false;
    bool m_initialized = false;
};

#endif
