// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Based on F4MP by Jous99 (public domain). Licensed under the GNU
// GPL v3 with additional terms: see LICENSE and NOTICE.

#ifndef F4MPCLIENT_NETWORKCLIENT_H
#define F4MPCLIENT_NETWORKCLIENT_H

#include <cmath>
#include <cstdint>
#include <string>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <vector>

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace Network {

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
    NpcState = 50,      // the host's NPC state (by FormID)

    // Co-op sync (>= 60). The server relays these to everyone except the sender and
    // stamps the first uint32 (senderId) with the authoritative id.
    CoopFirst = 60,
    CoopEquipment = 60,             // player -> all: equipped weapon/armor base forms
    CoopPlayerDamage = 61,          // host -> all: damage a host NPC dealt to a player's body
    CoopNpcDamage = 62,             // client -> host: damage the client dealt to a host-owned NPC
    CoopContainerChange = 70,       // player -> all: items moved into/out of a world container
    CoopWorldRefTaken = 71,         // player -> all: world item picked up (disable it)
    CoopContainerSnapshotReq = 72,  // client -> host: send me this container's contents
    CoopContainerSnapshot = 73,     // host -> all: authoritative contents of a container
    CoopItemDropped = 74,           // player -> all: dropped an item on the ground (others place a copy)
    CoopDropTaken = 75,             // player -> all: picked up a dropped item (others remove their copy)
    CoopBuildPlaced = 76,           // player -> all: built a workshop object (others place a copy)
    CoopBuildMoved = 77,            // player -> all: moved a workshop object
    CoopBuildRemoved = 78,          // player -> all: scrapped/stored a workshop object
    CoopRefActivated = 79,          // player -> all: opened a door / flipped a switch
    CoopQuestStage = 80,            // host -> all: stage changed; client -> host with isRequest
    CoopQuestSnapshotReq = 81,      // client -> host: send me every visible quest's stage
    CoopHello = 90,                 // on connect: load-order fingerprint (host answers with its own)
    CoopAppearance = 91,            // variable length: the sender's own character (see sync/Appearance.cpp)
    CoopWorldTime = 92,             // host -> all: clock, date and weather
    CoopAnimEvent = 93,             // player -> all: combat animation event to replay on the sender's body
    CoopDynamicActor = 100,         // host -> all: a runtime-created actor and how to spawn a copy
    CoopPlayerVitals = 102,         // player -> all: own health (for name tags)
    CoopPropMoved = 101,            // player -> all: a loose prop they carry (hold E) or that's still settling
    CoopExplosion = 103,            // player -> all: an explosion went off near them (form + place)
    CoopWorldReq = 104,             // joiner -> host: show me your world around here (builds, scrapping)
    CoopWorldSnapshot = 105,        // host -> joiner: a snapshot follows (the joiner hides its own builds)
    CoopRefsHidden = 106,           // host -> joiner: plugin references the host scrapped (targetId 0: scrapped live, to all)
    CoopFurniture = 107,            // player -> all: the furniture they sit/lie on and its marker (CoopSeatMsg; ref 0 = got up)
    CoopNpcAnim = 108,              // NPC owner -> all: combat animation event of an NPC it runs (attacks, reloads)
    CoopLockChanged = 109,          // player -> all: a door/container they unlocked (lockpick, key) or locked
    CoopNpcEquipment = 110,         // NPC owner -> all: what an NPC it runs wears and holds (weapon mods included)
    CoopPluginList = 111,           // variable length: the sender's plugin names by load-order index (sync/LoadOrder.cpp)
    CoopNpcActivity = 112,          // NPC owner -> all: the furniture an NPC it runs uses and the idle it plays (sync/Activity)
    CoopPowerArmor = 113,           // player -> all: in power armor or not, and the frame's pieces (sync/Activity)
    CoopDoorStateReq = 114,         // joiner -> host: doors around me, how are they in your world?
    CoopDoorState = 115,            // host -> joiner: open/closed and locked for those doors (quest scripts open gates)
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
    char password[32];       // Update 23: the host's join password ("" = none). Older games send the
                             // first 68 bytes only; a server without a password still takes them.
};
constexpr size_t kConnectionRequestV1Size = 68;   // playerName + protocolVersion (Update 22 and older)

struct ConnectionAcceptedMsg {
    uint32_t playerId;
    uint32_t currentPlayers;
    uint32_t maxPlayers;
};

// (type Disconnect) sent by the server when a player leaves or is kicked, so their body goes AT ONCE
// instead of after the 5 s timeout.
struct PlayerLeftMsg {
    uint32_t playerId;
};

struct PlayerPositionMsg {
    uint32_t playerId;
    float x, y, z;
    float pitch;         // look up/down (rad, + = down, as the game's angle.x); was the unused velocityX
    uint32_t extraFlags; // kPos* bits (Update 24); was the unused velocityY, which every older game sent as 0.0f
    float velocityZ;
    float angleZ;        // view heading (rad): where the player looks
    float speed;         // movement speed (units/s)
    float moveDir;       // movement direction relative to facing (rad, 0 = forward)
    uint32_t cellId;
    bool isMoving;
    bool isRunning;
    bool isSprinting;
    bool isSneaking;
    bool isJumping;
    bool weaponDrawn;   // weapon out (combat stance)
};
// Packed: a size change would make older games drop every position (they check the size).
static_assert(sizeof(PlayerPositionMsg) == 50, "PlayerPositionMsg must keep its size: older games read it");
constexpr uint32_t kPosLightOn = 1u << 0;   // Pip-Boy light / power armor headlamp on
constexpr uint32_t kPosLightColorShift = 8; // bits 8-31: that Pip-Boy's color 0xRRGGBB (0 = not sent)

// Player appearance (old format, still relayed): sent once on connect.
struct PlayerAppearanceMsg {
    uint32_t playerId;
    uint32_t raceFormID;
    uint32_t hairColor;      // RGBA empaquetado (BGSColorForm::color)
    int32_t  sex;            // 0 male, 1 female, -1 none
    uint8_t  skinR, skinG, skinB, skinA;  // the NPC's bodyTintColor
    uint8_t  numHeadParts;
    uint8_t  _pad[3];
    uint32_t headParts[16];  // formIDs de head parts (pelo, ojos, barba, cejas...)
};

struct ChatMessageMsg {
    uint32_t senderId;
    char message[256];
    uint8_t channel;
};

// An NPC's state as the HOST broadcasts it. Placed NPCs have the same FormID on every machine
// (translated for other mod lists); the guest finds its own NPC by it and drives it to this state.
constexpr uint8_t kNpcWeaponDrawn = 1, kNpcRunning = 2, kNpcCompanion = 4;   // companion: its player's, wherever it is
struct NpcStateMsg {
    uint32_t formId;     // the NPC's FormID (same NPC on every machine)
    float x, y, z;
    float angleZ;        // heading
    float speed;         // for the animation
    bool isMoving;
    bool isSneaking;
    bool isDead;         // dead on the owner's side
    uint8_t flags;       // kNpcWeaponDrawn | kNpcRunning (was padding: same size as before)
    float health;        // current Health actor value on the owner
    uint32_t ownerId;    // player whose game runs this NPC (sync/Authority)
};

// ---- Co-op messages: every struct starts with senderId (stamped by the server) ----

struct CoopLockChangedMsg {
    uint32_t senderId;
    uint32_t refFormId;      // plugin reference (same everywhere)
    uint8_t  locked;
    uint8_t  pad[3];
};

constexpr size_t kMaxDoorsPerMsg = 48;
struct CoopDoorStateReqMsg {
    uint32_t senderId;
    uint32_t count;
    uint32_t refs[kMaxDoorsPerMsg];   // plugin door references near the joiner
};

// open/locked: 0 = unknown (unloaded, animating, no lock), 1 = no, 2 = yes
struct CoopDoorEntry {
    uint32_t refFormId;
    uint8_t  open;
    uint8_t  locked;
    uint8_t  pad[2];
};

struct CoopDoorStateMsg {
    uint32_t senderId;
    uint32_t targetId;        // the joiner who asked
    uint32_t count;
    CoopDoorEntry doors[kMaxDoorsPerMsg];
};

struct CoopPlayerDamageMsg {
    uint32_t senderId;
    uint32_t targetPlayerId;
    float    damage;
    uint32_t byNpc;          // 1 = an NPC in the sender's game hit the body, 0 = the sender's own shot (PvP)
};

struct CoopNpcDamageMsg {
    uint32_t senderId;
    uint32_t ownerId;        // only this player's game applies it (runtime IDs differ per game)
    uint32_t npcFormId;
    float    damage;
};

// One attached object mod (OMOD) of a weapon/armor instance: a pipe pistol and a pipe SMG are the
// same base weapon with different mods.
struct CoopModEntry {
    uint32_t modFormId;
    uint8_t  index;
    uint8_t  rank;
    uint8_t  pad[2];
};
constexpr size_t kMaxItemMods = 24;

struct CoopContainerChangeMsg {
    uint32_t senderId;
    uint32_t containerFormId;
    uint32_t itemFormId;
    int32_t  count;          // > 0 added to the container, < 0 taken from it
    uint32_t ownerId;        // 0: plugin reference; else the game whose runtime actor (a spawned enemy's corpse) it is
    uint8_t  numMods;        // the moved stack's attached mods (a modded gun put in a box stays modded)
    uint8_t  pad[3];
    CoopModEntry mods[kMaxItemMods];
};

struct CoopWorldRefTakenMsg {
    uint32_t senderId;
    uint32_t refFormId;
};

struct CoopEquipmentMsg {
    uint32_t senderId;
    uint8_t  count;
    uint8_t  numWeaponMods;  // mods on the equipped weapon (a pipe pistol vs the base pipe gun's SMG)
    uint8_t  _pad[2];
    uint32_t forms[24];      // base FormIDs of equipped WEAP/ARMO
    uint32_t weaponFormId;   // which of them the mods belong to (0 = none)
    CoopModEntry weaponMods[kMaxItemMods];
};

// A drop is named by who dropped it and the FormID of the item on THEIR ground.
struct CoopItemDroppedMsg {
    uint32_t senderId;
    uint32_t refFormId;      // the dropped reference in the sender's game
    uint32_t itemFormId;     // base object
    int32_t  count;
    float    x, y, z;
    uint32_t space;          // interior cell / worldspace (Authority::LocalSpace)
    uint8_t  numMods;        // the item's attached mods (none for plain items)
    uint8_t  pad[3];
    CoopModEntry mods[kMaxItemMods];
};

// The owner's NPC as it is dressed and armed in the owner's game: spawned enemies roll their leveled gear
// on every machine (a raider with a pipe pistol here swung a bat there), and story NPCs carry what each
// save gave them.
struct CoopNpcEquipmentMsg {
    uint32_t senderId;
    uint32_t npcFormId;      // the owner's FormID (runtime actors: mapped by sync/DynamicActors)
    uint8_t  count;
    uint8_t  numWeaponMods;
    uint8_t  _pad[2];
    uint32_t forms[24];      // worn armor and the equipped weapon (base FormIDs)
    uint32_t weaponFormId;   // the equipped weapon (0 = none)
    CoopModEntry weaponMods[kMaxItemMods];
};

struct CoopExplosionMsg {
    uint32_t senderId;
    uint32_t explosionFormId;   // BGSExplosion
    float    x, y, z;
    uint32_t space;             // interior cell / worldspace (Authority::LocalSpace)
};

struct CoopWorldReqMsg {
    uint32_t senderId;
    float    x, y, z;
    uint32_t space;
};

struct CoopWorldSnapshotMsg {
    uint32_t senderId;
    uint32_t targetId;         // the joiner who asked
    float    x, y, z;          // centre of the area
    float    radius;
    uint32_t builds, hidden;   // what follows (CoopBuildPlaced / CoopRefsHidden)
};

constexpr size_t kMaxRefsPerMsg = 60;
struct CoopRefsHiddenMsg {
    uint32_t senderId;
    uint32_t targetId;
    uint32_t count;
    uint32_t refs[kMaxRefsPerMsg];
};

struct CoopDropTakenMsg {
    uint32_t senderId;
    uint32_t ownerId;        // who dropped it
    uint32_t ownerRefFormId; // CoopItemDroppedMsg::refFormId
};

// Workshop objects are named (owner key, ref): the builder's PERMANENT player key (F4MP_player_key.txt,
// not the session id, which changes on every connect) and the object's FormID in the builder's game.
// Both survive a restart, so builds stay linked across sessions (the links live in each co-save).
// Angles in radians, as in the reference's own data.
struct CoopBuildPlacedMsg {
    uint32_t senderId;
    uint32_t ownerKey;       // the builder's permanent key
    uint32_t refFormId;
    uint32_t baseFormId;
    uint32_t workshopFormId; // the settlement's workbench (a plugin ref: same on every machine)
    float    x, y, z;
    float    ax, ay, az;
};

struct CoopBuildMovedMsg {
    uint32_t senderId;
    uint32_t ownerId;        // owner's permanent key
    uint32_t ownerRefFormId;
    float    x, y, z;
    float    ax, ay, az;
};

struct CoopBuildRemovedMsg {
    uint32_t senderId;
    uint32_t ownerId;        // owner's permanent key
    uint32_t ownerRefFormId;
};

// ownerId = owner's permanent key; 0 = a plugin reference (same FormID everywhere, e.g. a vanilla door).
struct CoopRefActivatedMsg {
    uint32_t senderId;
    uint32_t ownerId;
    uint32_t ownerRefFormId;
};

// A seat: the furniture (named like CoopRefActivatedMsg: owner key 0 = plugin reference) and which of its
// markers (a bench has several, a campfire has a sit and a cook spot). ref 0 = not on any furniture.
struct CoopSeatMsg {
    uint32_t senderId;
    uint32_t ownerId;
    uint32_t ownerRefFormId;
    uint32_t marker;
};

// What an NPC is busy with, sent by the game that runs it on change and every few seconds.
struct CoopNpcActivityMsg {
    uint32_t senderId;
    uint32_t npcFormId;      // the owner's FormID (runtime actors: mapped by sync/DynamicActors)
    uint32_t seatOwnerId;    // its furniture, as in CoopSeatMsg (seatRefFormId 0 = none)
    uint32_t seatRefFormId;
    uint32_t marker;
    uint32_t idleFormId;     // the idle it plays outside furniture (TESIdleForm), 0 = none
};

// Power armor: in it (frame + pieces) or out. The pieces are also in CoopEquipmentMsg, but a body wearing
// them as plain armor looked broken: it gets a frame of its own instead.
struct CoopPowerArmorMsg {
    uint32_t senderId;
    uint8_t  inArmor;
    uint8_t  count;
    uint8_t  _pad[2];
    uint32_t pieces[8];      // ARMO FormIDs of the pieces on the frame (helmet, torso, arms, legs)
};

struct CoopPlayerVitalsMsg {
    uint32_t senderId;
    float    health;
    float    maxHealth;
    uint8_t  dead;
};

// A loose physics prop (cup, can, tire...) someone is carrying, ~10/s, then while it falls/settles.
struct CoopPropMovedMsg {
    uint32_t senderId;
    uint32_t refFormId;      // plugin reference (same everywhere)
    float    x, y, z;
    float    ax, ay, az;
    uint8_t  held;           // 1 = in their hands, 0 = let go (settling or at rest)
};

struct CoopContainerSnapshotReqMsg {
    uint32_t senderId;
    uint32_t containerFormId;
    uint32_t ownerId;        // as in CoopContainerChangeMsg
};

struct CoopContainerSnapshotMsg {
    uint32_t senderId;
    uint32_t containerFormId;
    uint16_t count;
    uint16_t _pad;
    struct Entry { uint32_t itemFormId; int32_t count; } entries[96];
    uint32_t ownerId;        // as in CoopContainerChangeMsg
};

struct CoopQuestStageMsg {
    uint32_t senderId;
    uint32_t questFormId;
    uint16_t stage;
    uint8_t  isRequest;      // 1 = client asks the host to set this stage
    uint8_t  isSnapshot;     // 1 = part of a join snapshot (only apply forward progress)
};

struct CoopQuestSnapshotReqMsg {
    uint32_t senderId;
};

struct CoopDynamicActorMsg {
    uint32_t senderId;
    uint32_t hostRefId;      // the actor's runtime FormID on the host (>= 0xFF000000)
    uint32_t spawnFormId;    // master-file record placeatme can use (NPC_ or leveled list)
    float x, y, z;
    float angleZ;
};

// Every sync message identifies things by FormID, and FormIDs depend on the exact plugin list.
struct CoopWorldTimeMsg {
    uint32_t senderId;
    float    gameHour;       // 0-24
    float    gameDay;
    float    gameMonth;
    float    gameYear;
    float    timeScale;
    uint32_t weatherFormId;  // 0 = none (host indoors)
};

struct CoopAnimEventMsg {
    uint32_t senderId;
    char     name[44];       // graph event, e.g. attackStartAuto, meleeattackStart, rifleSightedStart
};

struct CoopNpcAnimMsg {
    uint32_t senderId;
    uint32_t npcFormId;      // the owner's FormID (runtime actors: mapped by sync/DynamicActors)
    char     name[44];       // graph event the owner's engine sent the NPC
};

struct CoopHelloMsg {
    uint32_t senderId;
    uint32_t loadOrderHash;  // FNV-1a over lowercase file names, full plugins then light (.esl)
    uint16_t fullCount;
    uint16_t lightCount;
    uint8_t  isHost;
    uint8_t  isReply;        // host's answer to a newcomer's hello (don't answer it again)
    uint8_t  _pad[2];
    float    loadedRadius;   // area this game always has loaded (game units), for ownership
};

#pragma pack(pop)

// Raw co-op message waiting to be applied on the game's main thread.
struct CoopInboxItem {
    MessageType type;
    std::vector<uint8_t> payload;
    std::chrono::steady_clock::time_point receivedAt{};
};

// Positions and NPC states are sent many times a second and only the newest one matters: unreliable, so a
// lost packet doesn't hold up everything behind it (over the internet / Steam relays). Everything else
// (hits, items, builds, quests) stays reliable and in order.
inline bool IsStreamMessage(MessageType t) { return t == MessageType::PlayerPosition || t == MessageType::NpcState; }
constexpr int kSendStream = 1;     // k_nSteamNetworkingSend_UnreliableNoNagle
constexpr int kSendEvent = 8;      // k_nSteamNetworkingSend_Reliable

constexpr uint32_t PROTOCOL_VERSION = 16;   // 16: companions shown to the others, join password (Update 23); 15: NPC furniture/idles, seat markers, power armor (Update 21); 14: player hits say whether an NPC did it (Update 19); 13: NPC equipment, item mods in containers (Update 18); 12: unreliable positions/NPC states, NPC gait/weapon flags (Update 17); 11: Steam join, aimed shots/spine (Update 16); 10: aim pitch, NPC idle animations; 9: NPC combat animations; 8: live scrapping of world objects; 7: host world snapshots; 6: explosions; 5: item mods on drops; 4: workshop objects named by permanent player keys

// Incoming data comes from another game (or anyone who reached the port): positions must be real
// numbers inside the world, or a body/copy placed there can break the game.
inline bool SanePosition(float x, float y, float z)
{
    constexpr float kWorldLimit = 1.0e7f;   // the Commonwealth spans about +-250000 units
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
           std::fabs(x) < kWorldLimit && std::fabs(y) < kWorldLimit && std::fabs(z) < kWorldLimit;
}
constexpr size_t kMaxCoopPayload = 16384;   // the server relays nothing bigger (appearance ~8 KB)
constexpr uint16_t DEFAULT_SERVER_PORT = 7779;

class NetworkClient {
public:
    static NetworkClient& GetInstance();

    bool Initialize();
    void Shutdown();

    bool Connect(const std::string& address, uint16_t port = DEFAULT_SERVER_PORT);
    void Disconnect();

    bool IsConnected() const { return m_connected; }
    // Transport opened but not accepted yet (no answer from the server so far).
    bool IsConnecting() const { return m_connection != k_HSteamNetConnection_Invalid && !m_connected; }
    // Why the last attempt/connection ended ("" if none), and "ip:port" of the last Connect.
    std::string GetLastError() { std::lock_guard<std::mutex> l(m_statusMutex); return m_lastError; }
    std::string GetTarget() { std::lock_guard<std::mutex> l(m_statusMutex); return m_target; }
    // True if the last close happened before the server accepted us (a failed attempt).
    bool LastCloseWasAttempt() const { return m_lastCloseWasAttempt; }

    void SetPlayerName(const std::string& name) { m_playerName = name; }
    void SetPassword(const std::string& pw) { m_password = pw; }   // sent in the handshake
    bool IsSteam() const { return m_steam; }
    ISteamNetworkingSockets* Sockets() const { return m_sockets; }

    void SendPacket(MessageType type, const void* data, uint32_t size);

    // Call regularly: runs GNS callbacks and handles incoming messages.
    void PumpMessages();

    uint32_t GetPlayerId() const { return m_playerId; }

    // Copy of the remote players' state (id -> last position).
    std::unordered_map<uint32_t, PlayerPositionMsg> GetRemotePlayers();

    // A remote player's appearance; false if it hasn't arrived yet.
    bool GetAppearance(uint32_t playerId, PlayerAppearanceMsg& out);

    // NPCs broadcast by their owners, keyed by (ownerId << 32 | formId): runtime FormIDs from
    // two different owners can be equal.
    std::unordered_map<uint64_t, NpcStateMsg> GetRemoteNpcs();

    // Takes every pending co-op message (type >= CoopFirst). Call from the main thread.
    std::vector<CoopInboxItem> DrainCoopInbox();

private:
    NetworkClient() = default;

    static void OnConnStatusChangedStatic(SteamNetConnectionStatusChangedCallback_t* info);
    void OnConnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
    void SendConnectionRequest();

    ISteamNetworkingSockets* m_sockets = nullptr;
    ISteamNetworkingUtils* m_utils = nullptr;

    HSteamNetConnection m_connection = k_HSteamNetConnection_Invalid;
    bool m_steam = false;     // m_connection is a Steam peer-to-peer connection (SteamLink)
    int m_steamState = 0;     // its last polled state
    bool m_ownSteamEvent = false;

    bool m_connected = false;
    std::mutex m_statusMutex;
    std::string m_lastError;
    std::string m_target;
    bool m_lastCloseWasAttempt = false;
    bool m_initialized = false;
    uint32_t m_playerId = 0;
    std::string m_playerName = "Wastelander";
    std::string m_password;

    std::mutex m_mutex;

    // Remote players (updated by PumpMessages).
    std::mutex m_remoteMutex;
    std::unordered_map<uint32_t, PlayerPositionMsg> m_remotePlayers;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> m_remoteLastSeen;
    std::unordered_map<uint32_t, PlayerAppearanceMsg> m_remoteAppearance;

    // NPCs the host broadcasts (formId -> state) + when each was last seen.
    std::unordered_map<uint64_t, NpcStateMsg> m_remoteNpcs;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_npcLastSeen;
    std::unordered_map<uint32_t, uint64_t> m_remoteStamp;   // newest position stamp per player (unreliable: drop older)
    std::unordered_map<uint64_t, uint64_t> m_npcStamp;      // the same per NPC

    std::mutex m_coopMutex;
    std::vector<CoopInboxItem> m_coopInbox;
};

}

#endif
