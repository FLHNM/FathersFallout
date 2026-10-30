// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.
//
// Wasteland Wars A-Life: the Commonwealth's NPCs live on the server, whether or not anyone plays.
// "Offline" simulation, in the spirit of S.T.A.L.K.E.R.'s A-Life: the world is a graph of the game's map markers;
// squads (a raider gang, a Gunner patrol, a feral horde, a caravan...) have a faction, a home, members with health,
// and a routine (camp, patrol, roam, raid, trade). They travel, meet, fight, die and come back on timers. The state
// is saved to disk, so the world carries on across restarts and sessions. A later step hands squads near players
// to their games ("online"), where the real NPCs act it out.

#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace Wars {

// Next to the server exe (the Wars server runs from its own folder): the graph from tools/wars/make_world.py,
// and the world's saved state. Deleting the state = a wipe of the NPC world (the server owner's call).
inline constexpr const char* kWorldPath = "./wars_world.json";
inline constexpr const char* kStatePath = "./wars_alife.json";
inline constexpr int kWarsSquadMaxMembers = 8;   // = kWarsSquadMax in the network messages

// New factions go at the end: saved worlds keep the numbers.
enum class Faction : uint8_t { Raider, Gunner, Mutant, Feral, Mirelurk, Deathclaw, Creature, Synth, Robot, Trader,
    Brotherhood, Minutemen, Railroad, Count };

const char* FactionName(Faction a_f);
bool Hostile(Faction a_a, Faction a_b);

struct Node {
    uint32_t ref = 0;           // the map marker's reference in Fallout4.esm
    std::string name;
    float x = 0, y = 0, z = 0;
    int type = 0;               // map marker icon type
    std::vector<std::pair<Faction, int>> residents;   // NPCs the game placed here, by faction
    struct Spot { float x, y, z; };
    std::vector<Spot> spawns;   // where the game placed them: on the ground (the marker can be on a roof)
    bool hasCamp = false;       // make_world.py found open, flat, dry ground for a camp here
    Spot camp{};                // its middle (the ground's height there)
    std::vector<Spot> campItems;   // where its things stand, in build order, each at the ground's height
    // The game's own furniture there (make_world.py): its middle (their sandbox uses it all) and what it has.
    uint32_t workbench = 0;     // its settlement workbench (WorkshopWorkbench reference), 0 = none
    Spot workbenchAt{};
    bool hasHub = false;
    Spot hub{};
    int hubBeds = 0, hubSeats = 0, hubBenches = 0, hubCook = 0;
    struct Facing { float x, y, z, rz; };   // rz: the game's heading (0 = north, clockwise)
    std::vector<Facing> campWalls;   // against the buildings' walls around it, backs to the wall
    std::vector<Facing> campSeats;   // around its middle (the fire), facing it
    std::vector<Facing> campEdge;    // on its edge, facing out
    std::vector<int> links;     // neighbouring nodes
};

struct Member {
    float hp = 1.0f;            // 0..1 of full health
};

enum class Task : uint8_t { Camp, Travel, Fight, Flee };

struct Squad {
    uint32_t id = 0;
    Faction faction = Faction::Raider;
    int home = 0;               // node
    int at = 0;                 // last node reached
    std::vector<int> path;      // nodes still to walk (front = next)
    float x = 0, y = 0;         // world position
    Task task = Task::Camp;
    double until = 0;           // game hours: when the current camp ends
    uint32_t foe = 0;           // squad fought
    std::vector<Member> members;
    uint32_t owner = 0;         // the player whose game runs it right now (0 = simulated here); not saved
    uint32_t sentGoal = ~0u;    // what its game was last told to do (see GoalKey); not saved
    int spot = -1;              // its camp spot at its place (index in Node::spawns); not saved
    int raid = -1;              // the cleared place it was sent to attack (-1 = none)
    double siege = 0;           // game hours it has held that place with nobody there to stop it

    int Alive() const;
    float Strength() const;
};

// What a squad's people have built where they live, Rust-style: a camp grows while its people are at home (cooking
// fire first, then bedrolls, a tent, a weapons workbench...), falls apart when nobody tends it, and is taken over by
// another faction that camps there while its owners are away. The games near it place its things; the NPCs' sandbox
// package then uses them (cook, sleep, work at the benches) like the game's own settlers do.
struct Camp {
    int node = 0;
    Faction faction = Faction::Raider;
    int level = 0;              // things built (the first `level` of the faction's build list)
    double progress = 0;        // toward the next one
    double tended = 0;          // game hours: when its people were last there
    uint32_t version = 1;       // changes with every change (players get it again); not saved
    struct Item { uint32_t form; float x, y, z, rz; const char* name; };
};

struct HomeSlot {               // a node's own population, refilled on a timer
    int node = 0;
    Faction faction = Faction::Raider;
    int quota = 1;              // squads living here
    double nextRefill = 0;      // game hours
};

class ALife {
public:
    // The world graph from tools/wars/make_world.py. False if it can't be read.
    bool LoadWorld(const std::string& a_path);
    // The saved state (wars_alife.json); a fresh population when missing.
    void LoadState(const std::string& a_path);
    void SaveState(const std::string& a_path) const;

    // Advances the simulation by a_realSeconds (times the speed setting).
    void Tick(double a_realSeconds);

    // Online: squads near players are run by one player's game; the simulation leaves them alone meanwhile and
    // takes their state from that game's reports.
    std::vector<Squad*> OfflineNear(float a_x, float a_y, float a_radius);
    Squad* Get(uint32_t a_id) { return ById(a_id); }
    void Report(uint32_t a_id, int a_count, const float* a_hp, float a_x, float a_y, bool a_final);
    void Release(Squad& a_s);
    const Node& PlaceOf(const Squad& a_s) const { return m_nodes[Nearest(a_s.x, a_s.y)]; }
    // The next place on its way (nullptr = it stays where it is).
    const Node* GoalOf(const Squad& a_s) const
    {
        return (a_s.task == Task::Travel || a_s.task == Task::Flee) && !a_s.path.empty() ? &m_nodes[a_s.path.front()] : nullptr;
    }
    // Where it stands/camps: a spawn spot of its place away from a_avoid (players), else the marker.
    Node::Spot CampSpot(Squad& a_s, const std::vector<std::pair<float, float>>& a_avoid);
    // Changes when what its game should do changes: travel to the next place, or camp at a place.
    uint32_t GoalKey(const Squad& a_s) const;

    // Camps: the one at a place (nullptr = none), its things where they stand, the middle of it (its people live
    // around there; false = that place has no camp spot).
    const std::vector<Camp>& Camps() const { return m_camps; }
    const Camp* CampAt(int a_node) const;
    std::vector<Camp::Item> CampItems(const Camp& a_c) const;
    bool CampMiddle(int a_node, Node::Spot& a_out) const;
    int NodeOf(const Squad& a_s) const { return a_s.task == Task::Camp ? a_s.at : Nearest(a_s.x, a_s.y); }
    // Admin: build a camp up now (a_levels things), at a place.
    bool BuildAt(const std::string& a_place, int a_levels);

    // Cleared places: its own people were wiped out at home in a player's game, so nobody moves back in (no refill,
    // no camp) - it's for the players to take. The wasteland still comes for it: every few game days a hostile
    // squad from nearby is sent to attack it (like the game's settlement attacks).
    bool Cleared(int a_node) const { return m_cleared.count(a_node) != 0; }
    // Admin: mark a place cleared (true) or give it back to its people (false); send an attack at it now.
    bool SetCleared(const std::string& a_place, bool a_cleared);
    bool AttackAt(const std::string& a_place);

    // Claims: a cleared place with a settlement workbench, captured by a player (holding its workbench with no
    // enemies near). Saved. Who holds it ("" = nobody); the claim changes with every change (players get it again).
    const std::string& ClaimOf(int a_node) const;
    void SetClaim(int a_node, const std::string& a_owner);
    uint32_t ClaimsVersion() const { return m_claimsVersion; }
    // Admin: free a place's claim.
    bool Unclaim(const std::string& a_place);
    int Find(const std::string& a_place) const { return FindNode(a_place); }

    void SetSpeed(double a_s) { m_speed = a_s > 0 ? a_s : 1.0; }
    void SetRespawnHours(double a_h) { m_respawnHours = a_h > 0 ? a_h : 36.0; }
    double GameHours() const { return m_hours; }

    // Console: a summary, the squads near a place, the latest events.
    void PrintSummary() const;
    void PrintNear(const std::string& a_place) const;
    void PrintEvents(size_t a_n) const;
    // Admin: a new squad of a faction at a place (its home), e.g. "raider" "Sanctuary". False if unknown.
    bool SpawnAt(const std::string& a_faction, const std::string& a_place);
    // Admin: send a squad to a place now. False if either is unknown.
    bool SendTo(uint32_t a_id, const std::string& a_place);

    // Radio chatter (Wars/ALifeChatter.cpp): what squads say, taken by the server to send to players.
    enum class Line : uint8_t { Contact, Win, Flee, Raid, Settle, Losses, Travel, Idle, Gossip, Build, Takeover };
    struct Chatter {
        Faction faction;
        std::string who;      // "Raider, Corvega Assembly Plant"
        std::string text;
        float x, y;           // where it was said
        int priority;         // 0 chat, 1 events, 2 player-made trouble
        double hours;
    };
    std::vector<Chatter> TakeChatter();

    const std::vector<Node>& Nodes() const { return m_nodes; }
    const std::vector<Squad>& Squads() const { return m_squads; }

private:
    void LinkGraph();
    void Populate();
    Squad MakeSquad(Faction a_f, int a_home);
    void Think(Squad& a_s);
    void ThinkOnline(Squad& a_s);
    void Move(Squad& a_s, double a_hours);
    void Encounters(double a_hours);
    void Refill();
    void AddFactionHomes();
    std::vector<int> Route(int a_from, int a_to) const;
    int Nearest(float a_x, float a_y) const;
    int FindNode(const std::string& a_name) const;
    Squad* ById(uint32_t a_id);
    void Event(std::string a_text);
    std::string Label(const Squad& a_s) const;
    bool Night() const;
    void Say(const Squad& a_s, Line a_what, int a_place, Faction a_foe, int a_priority, const char* a_thing = nullptr);
    void Build(double a_hours);
    void SeedCamps();
    Camp* CampAtMut(int a_node);
    void News(Faction a_winner, Faction a_loser, int a_place, bool a_wiped);
    void Clear(int a_node, const Squad& a_last);
    void Threats(double a_hours);
    bool SendAttack(int a_node);
    void Ambient();

    std::vector<Node> m_nodes;
    std::vector<Squad> m_squads;
    std::vector<HomeSlot> m_homes;
    std::vector<Camp> m_camps;
    std::vector<std::string> m_events;   // the world's chronicle (latest last)
    std::vector<Chatter> m_chatter;      // said since the last TakeChatter
    std::vector<std::string> m_news;     // recent fights worth retelling (gossip)
    std::map<int, double> m_cleared;     // cleared place -> game hour of its next attack
    std::map<int, std::string> m_claims; // captured place -> who holds it
    uint32_t m_claimsVersion = 1;
    double m_nextAmbient = 0;
    double m_hours = 24.0 * 7;           // game time, hours (day 8, 00:00)
    double m_speed = 1.0;
    double m_respawnHours = 36.0;
    uint32_t m_nextId = 1;
    std::mt19937 m_rng{ 1111 };
};

}  // namespace Wars
