#ifndef F4MPSERVER_GAMESERVER_H
#define F4MPSERVER_GAMESERVER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <chrono>
#include <memory>
#include <sstream>

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "Wars/ALife.h"
#include "Wars/Teams.h"

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
    WarsPresence = 130,      // Wars, player -> server: where I am (not relayed)
    WarsSquadOnline = 131,   // Wars, server -> one player: run this A-Life squad in your game
    WarsSquadReport = 132,   // Wars, player -> server: that squad's state (not relayed)
    WarsSquadOffline = 133,  // Wars, server -> its player: hand it back
    WarsSquadGoal = 134,     // Wars, server -> its player: camp at a spot / walk to the next place
    WarsChatter = 135,       // Wars, server -> players: A-Life radio chatter
    WarsCamp = 136,          // Wars, server -> players near it: an A-Life camp and what its people built
    WarsCampGone = 137,      // Wars, server -> a player: that camp is out of their range now
    WarsRadioTune = 138,     // Wars, player -> server: my radio (on/off, frequency, chatter; not relayed)
    WarsVoice = 139,         // Wars, player -> server -> players: push-to-talk voice (nearby / radio frequency)
    WarsCapture = 140,       // Wars, server -> a player: capturing a cleared place (seconds held / interrupted / done)
    WarsClaim = 141,         // Wars, server -> players: a place's workbench and who holds it (yours or not)
    WarsTeamAction = 142,  // Wars, player -> server: pick a side (solo / faction / make, join, leave a crew; leader: answer, open)
    WarsTeamInfo = 143,    // Wars, server -> players: one player's side (crew, tag, colour; cooldown for themselves)
    WarsCrew = 144,         // Wars, server -> players: a crew (name, tag, colour, open, members; its leader also gets requests)
    WarsNotice = 145,       // Wars, server -> a player: a line for them (why a pick was refused, a request sent...)
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

// Wasteland Wars A-Life (server/src/Wars/ALife). Never relayed to other players: the server answers them.
constexpr int kWarsSquadMax = 8;
struct WarsPresenceMsg {
    uint32_t sender;        // filled in by the server
    uint8_t outside;        // 1 = in the Commonwealth's exterior (x/y/z valid), 0 = inside somewhere / not in a world
    float x, y, z;
};
struct WarsSquadMsg {
    uint32_t sender;
    uint32_t squad;         // A-Life squad id
    uint8_t faction;        // Wars::Faction
    uint8_t count;          // members (online: to spawn; report: still alive)
    float hp[kWarsSquadMax];   // each member's health, 0..1
    float x, y, z;          // online: where to spawn (its place's map marker); report: where they are
    uint32_t anchorRef;     // online: that map marker's reference
    uint32_t goalRef;       // online: the map marker of the place it's heading for (0 = staying)
    uint8_t final;          // report: 1 = the last one (squad handed back)
    float gx, gy, gz;       // online: where it lives while it camps (its camp's middle; 0 = x/y/z)
};
struct WarsSquadGoalMsg {
    uint32_t sender;
    uint32_t squad;
    uint8_t travel;         // 1 = walk to goalRef (a place's map marker), 0 = camp at x/y/z
    uint32_t goalRef;
    float x, y, z;          // camp: the spot (where the game put NPCs there, on the ground)
};
struct WarsChatterMsg {
    uint32_t sender;
    uint8_t faction;        // Wars::Faction of the speaker (0xFF = the server / news)
    float x, y;             // where it was said
    char who[48];           // "Raider, Corvega gang"
    char text[200];
};

// Squads build up camps where they live (server/src/Wars/ALife: Camp); the games near one place its things.
constexpr int kWarsCampMax = 16;
struct WarsCampItem {
    uint32_t form;          // a vanilla base object: cooking fire, bedroll, tent, weapons workbench...
    float x, y, z;          // z = the camp's ground height there (the game drops it onto the ground)
    float rz;               // facing, radians
};
struct WarsTeamActionMsg {
    uint32_t sender;
    uint8_t action;         // 1 solo, 2 faction (arg), 3 make a crew (name, tag, arg = colour, open), 4 join (crew),
                            // 5 leave, 6 accept (who), 7 turn away (who), 8 set open (open)
    uint8_t arg;
    uint8_t open;
    uint32_t crew;
    char name[32];
    char tag[6];
    char who[48];
};
struct WarsTeamInfoMsg {
    uint32_t sender;
    uint32_t player;        // whose side this is
    uint8_t kind;           // 0 not picked yet, 1 solo, 2 crew, 3 faction
    uint8_t faction;        // 0 Brotherhood, 1 Minutemen, 2 Railroad, 3 Institute
    uint8_t colour;         // crew colour 0-7
    uint32_t crew;
    uint32_t cooldown;      // seconds before they may change again (theirs only)
    char tag[6];
    char crewName[32];
};
struct WarsCrewMsg {
    uint32_t sender;
    uint32_t crew;
    uint8_t colour, open, members, gone;   // gone = 1: disbanded
    char name[32];
    char tag[6];
    char leader[48];
    char requests[4][48];   // asking to join: sent to its leader only
};
struct WarsNoticeMsg {
    uint32_t sender;
    char text[160];
};
struct WarsCaptureMsg {
    uint32_t sender;
    uint32_t place;         // the place's map marker
    uint8_t state;          // 0 holding, 1 interrupted (left / enemies near), 2 captured
    uint8_t held, need;     // seconds
    char name[48];          // the place's name
};
struct WarsClaimMsg {
    uint32_t sender;
    uint32_t workbench;     // the place's settlement workbench (WorkshopWorkbench reference)
    uint8_t mine;           // 1 = the receiving player holds it
    char owner[48];         // who does ("" = nobody)
    uint32_t place;         // the place's map marker (where its holders get up after dying: fast travel there)
    char name[48];          // the place's name
};
struct WarsCampMsg {
    uint32_t sender;
    uint32_t camp;          // the place's map marker reference (one camp per place)
    uint8_t faction;        // Wars::Faction of the people who live there
    uint8_t level;          // how far they've built it up (= items in use)
    uint8_t count;
    WarsCampItem items[kWarsCampMax];
};
struct WarsCampGoneMsg {
    uint32_t sender;
    uint32_t camp;
};
// The player's radio: A-Life chatter and radio voice only reach a radio that's on; voice only on the same frequency.
struct WarsRadioTuneMsg {
    uint32_t sender;
    uint16_t freq;          // tenths of MHz: 1015 = 101.5
    uint8_t on;
    uint8_t chatter;        // A-Life chatter on this radio too
};
// Variable length: this header, then `bytes` of Opus frames ([u8 length][frame] each, 20 ms of 16 kHz mono).
// Unreliable: a lost packet is a 40 ms gap, never a hold-up.
constexpr int kWarsVoiceMaxBytes = 600;
struct WarsVoiceMsg {
    uint32_t sender;
    uint8_t flags;          // kVoiceRadio: on the radio (else nearby), kVoiceEnd: the last packet of a transmission
    uint16_t freq;          // radio: the frequency it went out on
    uint16_t seq;
    uint32_t space;         // nearby: worldspace (outdoors) or interior cell FormID; voice carries only within one
    float x, y, z;          // the speaker
    uint8_t frames;
    uint16_t bytes;
};
constexpr uint8_t kVoiceRadio = 1, kVoiceEnd = 2;

#pragma pack(pop)

constexpr uint32_t PROTOCOL_VERSION = 17;   // must match client-ng/src/network/NetworkClient.h
// Wasteland Wars games (a separate mode) add this, so a campaign game and a Wars game never join each other.
constexpr uint32_t kWarsProtocolOffset = 1000;
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
    void SetWars(bool a_wars) { m_wars = a_wars; }   // a Wasteland Wars server (FathersFalloutWarsServer.exe)
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
    void StartALife();
    void TickALife();
    void CmdALife(std::istringstream& args);
    void HandleWarsPresence(HSteamNetConnection conn, const WarsPresenceMsg* msg);
    void HandleWarsReport(HSteamNetConnection conn, const WarsSquadMsg* msg);
    void WarsOnlineOffline();
    void WarsChatter();
    void WarsCamps();
    void WarsCapture();
    void WarsTeamsSend();
    void HandleWarsTeam(HSteamNetConnection conn, const WarsTeamActionMsg* msg);
    void WarsNotice(uint32_t a_client, const std::string& a_text);
    void HandleWarsTune(HSteamNetConnection conn, const WarsRadioTuneMsg* msg);
    void HandleWarsVoice(HSteamNetConnection conn, const uint8_t* payload, uint32_t size);
    // Where a squad lives while it camps: the middle of its faction's camp there, else a spawn spot away from players.
    Wars::Node::Spot WarsLiveSpot(Wars::Squad& a_s, const std::vector<std::pair<float, float>>& a_players);

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
    bool m_wars = false;      // Wasteland Wars: expects PROTOCOL_VERSION + kWarsProtocolOffset

    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastStatus;

    // Wasteland Wars: the world's NPCs, simulated here (null on campaign servers or without wars_world.json).
    std::unique_ptr<Wars::ALife> m_alife;
    std::unique_ptr<Wars::Teams> m_teams;
    std::chrono::steady_clock::time_point m_alifeTick, m_alifeSaved;
    // Where each Wars player is (by client id): squads near players who are outdoors go "online".
    struct WarsPresence { bool outside = false; float x = 0, y = 0, z = 0; std::chrono::steady_clock::time_point seen; };
    std::unordered_map<uint32_t, WarsPresence> m_warsPresence;
    struct WarsHold { int node = -1; int held = 0; };
    std::unordered_map<uint32_t, WarsHold> m_warsHold;          // player -> the place they're capturing
    std::unordered_map<uint32_t, uint32_t> m_claimsSent;        // player -> claims version they have
    std::unordered_map<uint32_t, uint32_t> m_teamsSent;         // player -> teams version they have
    // Radio lines waiting for each player (by client id), and when the last one went out.
    struct RadioBox { std::vector<std::pair<Wars::ALife::Chatter, std::chrono::steady_clock::time_point>> pending; std::chrono::steady_clock::time_point last; };
    std::unordered_map<uint32_t, RadioBox> m_radio;
    // Each Wars player's radio (by client id; none yet = on, chatter on, no frequency) and the camps they were sent
    // (place index -> camp version).
    struct RadioTune { uint16_t freq = 0; bool on = true; bool chatter = true; };
    std::unordered_map<uint32_t, RadioTune> m_radioTune;
    std::unordered_map<uint32_t, std::unordered_map<int, uint32_t>> m_campsSent;

    std::queue<std::string> m_commands;
    std::mutex m_cmdMutex;

    bool m_running = false;
    bool m_initialized = false;
};

#endif
