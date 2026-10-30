// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.

#include "Wars/ALife.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <queue>
#include <spdlog/spdlog.h>

#include "ThirdParty/nlohmann/json.hpp"

namespace Wars {

namespace {

// Game time runs 20x real time (the game's TimeScale): a real second is 1/180 of a game hour.
constexpr double kRealSecondsPerGameHour = 180.0;
// Squads closer than this (game units; a cell is 4096) are in each other's sight.
constexpr float kSight = 1200.0f;

struct Def {
    const char* name;       // "raider"
    const char* group;      // how a squad is called in the chronicle
    int minSize, maxSize;
    float memberStrength;   // fighting power of one healthy member
    float speed;            // walking speed, game units per real second (an NPC walks ~150)
    int roam;               // how many links away it patrols
    float campMin, campMax; // game hours it stays somewhere
    float raidChance;       // chance a new plan is an attack on a hostile camp nearby
};

// Faction table: who they are and how they live.
const Def kDefs[] = {
    { "raider", "raider gang", 3, 6, 1.0f, 150.0f, 2, 6.0f, 16.0f, 0.12f },
    { "gunner", "Gunner squad", 4, 6, 1.3f, 160.0f, 3, 5.0f, 12.0f, 0.15f },
    { "mutant", "super mutant war party", 3, 5, 1.6f, 140.0f, 3, 6.0f, 18.0f, 0.18f },
    { "feral", "feral horde", 4, 8, 0.6f, 170.0f, 1, 8.0f, 20.0f, 0.05f },
    { "mirelurk", "mirelurk nest", 2, 4, 1.2f, 100.0f, 1, 12.0f, 30.0f, 0.0f },
    { "deathclaw", "deathclaw", 1, 1, 6.0f, 180.0f, 2, 14.0f, 36.0f, 0.05f },
    { "creature", "pack", 2, 5, 0.4f, 130.0f, 1, 8.0f, 24.0f, 0.0f },
    { "synth", "synth patrol", 3, 5, 1.4f, 150.0f, 3, 8.0f, 16.0f, 0.10f },
    { "robot", "robot guard post", 2, 4, 1.5f, 0.0f, 0, 999.0f, 999.0f, 0.0f },
    { "trader", "caravan", 3, 4, 1.5f, 120.0f, 0, 6.0f, 12.0f, 0.0f },
    { "brotherhood", "Brotherhood patrol", 3, 5, 1.8f, 150.0f, 3, 6.0f, 14.0f, 0.15f },
    { "minutemen", "Minutemen squad", 3, 6, 1.0f, 150.0f, 2, 6.0f, 14.0f, 0.10f },
    { "railroad", "Railroad cell", 2, 3, 1.1f, 160.0f, 3, 6.0f, 12.0f, 0.05f },
};
static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == static_cast<size_t>(Faction::Count));

const Def& D(Faction a_f) { return kDefs[static_cast<int>(a_f)]; }

// Where caravans run between (the ones this world has).
const char* kTradeHubs[] = { "Bunker Hill", "Covenant", "Abernathy Farm", "Starlight Drive In", "Tenpines Bluff",
    "Sanctuary", "Greentop Nursery", "Finch Farm", "Oberland Station", "County Crossing", "Taffington Boathouse",
    "Somerville Place", "Sunshine Tidings Co-op", "The Slog", "Coastal Cottage", "Kingsport Lighthouse" };

bool IsAnimal(Faction a_f) { return a_f == Faction::Mirelurk || a_f == Faction::Deathclaw || a_f == Faction::Creature; }

}  // namespace

const char* FactionName(Faction a_f) { return D(a_f).name; }

bool Hostile(Faction a_a, Faction a_b)
{
    if (a_a == a_b) return false;
    if (IsAnimal(a_a) && IsAnimal(a_b)) return false;   // animals leave each other alone
    auto pair = [&](Faction x, Faction y) { return (a_a == x && a_b == y) || (a_a == y && a_b == x); };
    if (pair(Faction::Feral, Faction::Mutant)) return false;   // ferals don't go for super mutants
    if (pair(Faction::Feral, Faction::Robot)) return false;
    // The big factions: the Minutemen get on with the Brotherhood and the Railroad (who hate each other), and
    // nobody of them robs caravans. (Synths are the Institute: everyone's enemy but their own.)
    if (pair(Faction::Minutemen, Faction::Brotherhood) || pair(Faction::Minutemen, Faction::Railroad)) return false;
    for (Faction f : { Faction::Brotherhood, Faction::Minutemen, Faction::Railroad })
        if (pair(f, Faction::Trader)) return false;
    return true;   // the Commonwealth: everyone else is at war
}

int Squad::Alive() const
{
    return static_cast<int>(std::count_if(members.begin(), members.end(), [](const Member& m) { return m.hp > 0.0f; }));
}

float Squad::Strength() const
{
    float hp = 0;
    for (const auto& m : members) hp += std::max(0.0f, m.hp);
    return hp * D(faction).memberStrength;
}

bool ALife::LoadWorld(const std::string& a_path)
{
    std::ifstream in(a_path);
    if (!in) return false;
    nlohmann::json j;
    try { in >> j; } catch (...) { return false; }
    m_nodes.clear();
    for (const auto& n : j.value("nodes", nlohmann::json::array())) {
        Node node;
        node.ref = n.value("ref", 0u);
        node.name = n.value("name", std::string());
        node.x = n.value("x", 0.0f);
        node.y = n.value("y", 0.0f);
        node.z = n.value("z", 0.0f);
        node.type = n.value("type", 0);
        const nlohmann::json residents = n.value("residents", nlohmann::json::object());
        for (auto& [f, count] : residents.items())
            for (int i = 0; i < static_cast<int>(Faction::Count); ++i)
                if (f == kDefs[i].name) node.residents.emplace_back(static_cast<Faction>(i), count.get<int>());
        for (const auto& sp : n.value("spawns", nlohmann::json::array()))
            if (sp.size() >= 4) node.spawns.push_back({ sp[1].get<float>(), sp[2].get<float>(), sp[3].get<float>() });
        if (n.contains("workshop") && n["workshop"].size() >= 4) {
            const nlohmann::json w = n["workshop"];
            node.workbench = w[0].get<uint32_t>();
            node.workbenchAt = { w[1].get<float>(), w[2].get<float>(), w[3].get<float>() };
        }
        if (n.contains("hub")) {
            const nlohmann::json h = n["hub"];
            node.hasHub = true;
            node.hub = { h.value("x", node.x), h.value("y", node.y), h.value("z", node.z) };
            node.hubBeds = h.value("beds", 0);
            node.hubSeats = h.value("seats", 0);
            node.hubBenches = h.value("benches", 0);
            node.hubCook = h.value("cook", 0);
        }
        if (n.contains("camp")) {
            const nlohmann::json c = n["camp"];
            node.hasCamp = true;
            node.camp = { c.value("x", node.x), c.value("y", node.y), c.value("z", node.z) };
            for (const auto& it : c.value("items", nlohmann::json::array()))
                if (it.size() >= 3) node.campItems.push_back({ it[0].get<float>(), it[1].get<float>(), it[2].get<float>() });
            auto facing = [&](const char* a_key, std::vector<Node::Facing>& a_out) {
                for (const auto& it : c.value(a_key, nlohmann::json::array()))
                    if (it.size() >= 4) a_out.push_back({ it[0].get<float>(), it[1].get<float>(), it[2].get<float>(), it[3].get<float>() });
            };
            facing("walls", node.campWalls);
            facing("seats", node.campSeats);
            facing("edge", node.campEdge);
        }
        m_nodes.push_back(std::move(node));
    }
    if (m_nodes.size() < 2) return false;
    LinkGraph();
    spdlog::info("[ALife] world: {} places", m_nodes.size());
    return true;
}

// Each place links to its nearest neighbours; separate clusters are then joined at their closest places, so
// every place can be reached.
void ALife::LinkGraph()
{
    const int n = static_cast<int>(m_nodes.size());
    auto dist = [&](int a, int b) { return std::hypot(m_nodes[a].x - m_nodes[b].x, m_nodes[a].y - m_nodes[b].y); };
    auto link = [&](int a, int b) {
        auto& la = m_nodes[a].links;
        if (a == b || std::find(la.begin(), la.end(), b) != la.end()) return;
        la.push_back(b);
        m_nodes[b].links.push_back(a);
    };
    for (int a = 0; a < n; ++a) {
        std::vector<int> order(n);
        std::iota(order.begin(), order.end(), 0);
        std::partial_sort(order.begin(), order.begin() + std::min(n, 6), order.end(), [&](int x, int y) { return dist(a, x) < dist(a, y); });
        for (int k = 1; k < std::min(n, 6); ++k)
            if (dist(a, order[k]) < 18000.0f) link(a, order[k]);
    }
    std::vector<int> comp(n, -1);
    for (;;) {
        std::fill(comp.begin(), comp.end(), -1);
        int comps = 0;
        for (int s = 0; s < n; ++s) {
            if (comp[s] >= 0) continue;
            std::queue<int> q;
            q.push(s);
            comp[s] = comps;
            while (!q.empty()) {
                int c = q.front();
                q.pop();
                for (int l : m_nodes[c].links)
                    if (comp[l] < 0) { comp[l] = comps; q.push(l); }
            }
            ++comps;
        }
        if (comps <= 1) break;
        int ba = -1, bb = -1;
        float bd = 1e30f;
        for (int a = 0; a < n; ++a)
            if (comp[a] == 0)
                for (int b = 0; b < n; ++b)
                    if (comp[b] != 0 && dist(a, b) < bd) { bd = dist(a, b); ba = a; bb = b; }
        link(ba, bb);
    }
}

Squad ALife::MakeSquad(Faction a_f, int a_home)
{
    const Def& d = D(a_f);
    Squad s;
    s.id = m_nextId++;
    s.faction = a_f;
    s.home = s.at = a_home;
    s.x = m_nodes[a_home].x + std::uniform_real_distribution<float>(-600, 600)(m_rng);
    s.y = m_nodes[a_home].y + std::uniform_real_distribution<float>(-600, 600)(m_rng);
    s.members.resize(std::uniform_int_distribution<int>(d.minSize, d.maxSize)(m_rng));
    s.task = Task::Camp;
    s.until = m_hours + std::uniform_real_distribution<double>(0.0, d.campMax)(m_rng);
    return s;
}

// The world's own population: every place the game filled with NPCs gets that many squads of them (about one
// per five NPCs, up to three), plus caravans between the settlements.
void ALife::Populate()
{
    m_squads.clear();
    m_homes.clear();
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
        // One faction holds a place: the one the game put most of there (Lexington's ferals, not its 5 raiders;
        // two hostile homes on one spot only ever fought).
        const auto& res = m_nodes[i].residents;
        auto top = std::max_element(res.begin(), res.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        if (top == res.end()) continue;
        // Up in the sky: the Prydwen (not there in a Wars world) and the Mass Fusion penthouse.
        if (m_nodes[i].name.find("Prydwen") != std::string::npos || m_nodes[i].name.find("Executive Suite") != std::string::npos) continue;
        {
            auto [f, count] = *top;
            if (count < 2 && f != Faction::Deathclaw) continue;
            HomeSlot h;
            h.node = i;
            h.faction = f;
            h.quota = std::clamp((count + 2) / 5, 1, 3);
            m_homes.push_back(h);
            for (int k = 0; k < h.quota; ++k) m_squads.push_back(MakeSquad(f, i));
        }
    }
    std::vector<int> hubs;
    for (const char* name : kTradeHubs)
        if (int n = FindNode(name); n >= 0) hubs.push_back(n);
    for (size_t k = 0; hubs.size() >= 2 && k < 4; ++k) {
        HomeSlot h;
        h.node = hubs[k % hubs.size()];
        h.faction = Faction::Trader;
        h.quota = 1;
        m_homes.push_back(h);
        m_squads.push_back(MakeSquad(Faction::Trader, h.node));
    }
    AddFactionHomes();
    m_camps.clear();
    SeedCamps();
    Event(fmt::format("The Commonwealth wakes up: {} squads in {} homes", m_squads.size(), m_homes.size()));
}

// The big factions (Brotherhood, Minutemen, Railroad). The Brotherhood lives where the game put its soldiers out in
// the open (the airport, Cambridge PD); the Minutemen hold the Castle and the Railroad's cells hide in its safehouses
// (the game has almost none of them outside). Also run on a world saved before they existed: their homes are added.
void ALife::AddFactionHomes()
{
    auto has = [&](int a_node) { return std::any_of(m_homes.begin(), m_homes.end(), [&](const HomeSlot& h) { return h.node == a_node; }); };
    auto add = [&](Faction a_f, int a_node, int a_quota) {
        if (a_node < 0 || has(a_node) || Cleared(a_node)) return;
        HomeSlot h;
        h.node = a_node;
        h.faction = a_f;
        h.quota = a_quota;
        m_homes.push_back(h);
        for (int k = 0; k < a_quota; ++k) m_squads.push_back(MakeSquad(a_f, a_node));
        spdlog::info("[ALife] the {} live at {} now", FactionName(a_f), m_nodes[a_node].name);
    };
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
        const auto& n = m_nodes[i];
        if (n.name.find("Prydwen") != std::string::npos || n.name.find("Executive Suite") != std::string::npos) continue;
        for (auto [f, count] : n.residents)
            if (f == Faction::Brotherhood && count >= 5) add(f, i, std::clamp((count + 2) / 8, 1, 2));
    }
    // The first free ones (the Castle is the mirelurks' until someone clears it).
    auto firstFree = [&](Faction a_f, std::initializer_list<const char*> a_places, int a_homes) {
        if (std::count_if(m_homes.begin(), m_homes.end(), [&](const HomeSlot& h) { return h.faction == a_f; }) >= a_homes) return;
        for (const char* p : a_places) {
            const int n = FindNode(p);
            if (n < 0 || has(n) || Cleared(n)) continue;
            add(a_f, n, a_f == Faction::Minutemen ? 2 : 1);
            if (std::count_if(m_homes.begin(), m_homes.end(), [&](const HomeSlot& h) { return h.faction == a_f; }) >= a_homes) return;
        }
    };
    firstFree(Faction::Minutemen, { "The Castle", "Starlight Drive In", "Tenpines Bluff", "Oberland Station", "County Crossing", "Greentop Nursery" }, 2);
    firstFree(Faction::Railroad, { "Old North Church", "Mercer Safehouse", "Ticonderoga", "Bunker Hill" }, 2);
}

void ALife::LoadState(const std::string& a_path)
{
    std::ifstream in(a_path);
    nlohmann::json j;
    bool ok = false;
    if (in) {
        try { in >> j; ok = j.value("places", 0) == static_cast<int>(m_nodes.size()); } catch (...) { ok = false; }
    }
    if (!ok) {
        spdlog::info("[ALife] no saved world state (or a different world): a fresh population");
        Populate();
        return;
    }
    m_hours = j.value("hours", m_hours);
    m_nextId = j.value("nextId", 1u);
    m_squads.clear();
    for (const auto& s : j["squads"]) {
        Squad q;
        q.id = s["id"];
        q.faction = static_cast<Faction>(s["f"].get<int>());
        q.home = s["home"];
        q.at = s["at"];
        q.path = s["path"].get<std::vector<int>>();
        q.x = s["x"];
        q.y = s["y"];
        q.task = static_cast<Task>(s["task"].get<int>());
        q.until = s["until"];
        q.foe = s["foe"];
        for (float hp : s["hp"]) q.members.push_back({ hp });
        m_squads.push_back(std::move(q));
    }
    m_homes.clear();
    for (const auto& h : j["homes"])
        m_homes.push_back({ h["node"], static_cast<Faction>(h["f"].get<int>()), h["quota"], h["next"] });
    m_camps.clear();
    if (j.contains("camps")) {
        for (const auto& c : j["camps"])
            m_camps.push_back({ c["node"], static_cast<Faction>(c["f"].get<int>()), c["level"], c["progress"], c["tended"], 1 });
    } else {
        SeedCamps();   // a world saved before camps existed
    }
    m_cleared.clear();
    for (const auto& c : j.value("cleared", nlohmann::json::array()))
        if (c.size() >= 2) m_cleared[c[0].get<int>()] = c[1].get<double>();
    m_claims.clear();
    for (const auto& c : j.value("claims", nlohmann::json::array()))
        if (c.size() >= 2) m_claims[c[0].get<int>()] = c[1].get<std::string>();
    m_events = j.value("events", std::vector<std::string>());
    AddFactionHomes();   // a world saved before the big factions were in it
    spdlog::info("[ALife] world state loaded: day {}, {} squads", static_cast<int>(m_hours / 24) + 1, m_squads.size());
}

void ALife::SaveState(const std::string& a_path) const
{
    nlohmann::json j;
    j["places"] = m_nodes.size();
    j["hours"] = m_hours;
    j["nextId"] = m_nextId;
    auto& squads = j["squads"] = nlohmann::json::array();
    for (const auto& s : m_squads) {
        std::vector<float> hp;
        for (const auto& m : s.members) hp.push_back(m.hp);
        squads.push_back({ { "id", s.id }, { "f", static_cast<int>(s.faction) }, { "home", s.home }, { "at", s.at },
            { "path", s.path }, { "x", s.x }, { "y", s.y }, { "task", static_cast<int>(s.task) }, { "until", s.until },
            { "foe", s.foe }, { "hp", hp } });
    }
    auto& homes = j["homes"] = nlohmann::json::array();
    for (const auto& h : m_homes)
        homes.push_back({ { "node", h.node }, { "f", static_cast<int>(h.faction) }, { "quota", h.quota }, { "next", h.nextRefill } });
    auto& camps = j["camps"] = nlohmann::json::array();
    for (const auto& c : m_camps)
        camps.push_back({ { "node", c.node }, { "f", static_cast<int>(c.faction) }, { "level", c.level }, { "progress", c.progress }, { "tended", c.tended } });
    auto& cleared = j["cleared"] = nlohmann::json::array();
    for (const auto& [node, next] : m_cleared) cleared.push_back({ node, next });
    auto& claims = j["claims"] = nlohmann::json::array();
    for (const auto& [node, owner] : m_claims) claims.push_back({ node, owner });
    const size_t keep = std::min<size_t>(m_events.size(), 100);
    j["events"] = std::vector<std::string>(m_events.end() - keep, m_events.end());
    const std::string tmp = a_path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return;
        out << j.dump();
    }
    std::remove(a_path.c_str());
    std::rename(tmp.c_str(), a_path.c_str());
}

void ALife::Tick(double a_realSeconds)
{
    if (m_nodes.empty()) return;
    double hours = a_realSeconds * m_speed / kRealSecondsPerGameHour;
    // Small steps, so fights and travel stay fine-grained even fast-forwarded.
    while (hours > 0) {
        const double step = std::min(hours, 0.05);
        hours -= step;
        m_hours += step;
        for (auto& s : m_squads) {
            if (s.owner) { ThinkOnline(s); continue; }   // a player's game runs its body; the plans stay here
            if (s.task == Task::Fight) {
                // Its enemy is gone (dead, or fled from someone else): back to camp.
                const Squad* f = ById(s.foe);
                if (!f || f->Alive() == 0 || f->task != Task::Fight || f->foe != s.id) {
                    s.task = Task::Camp;
                    s.until = m_hours + 1.0;
                    s.at = Nearest(s.x, s.y);
                    s.path.clear();
                }
                continue;
            }
            if (s.task == Task::Camp) {
                for (auto& m : s.members)
                    if (m.hp > 0) m.hp = std::min(1.0f, m.hp + static_cast<float>(0.05 * step));
                if (m_hours >= s.until) Think(s);
            } else {
                Move(s, step);
            }
        }
        Encounters(step);
        Build(step);
        if (m_hours >= m_nextAmbient) {   // someone on the radio every ~45 real seconds
            m_nextAmbient = m_hours + 0.25;
            Ambient();
        }
        m_squads.erase(std::remove_if(m_squads.begin(), m_squads.end(), [](const Squad& s) { return s.Alive() == 0 && !s.owner; }), m_squads.end());
        Refill();
        Threats(step);
    }
}

// A new plan once the squad is done camping: raid a hostile camp nearby, patrol, go home, or (caravans) the next
// settlement.
void ALife::Think(Squad& a_s)
{
    const Def& d = D(a_s.faction);
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);
    auto camp = [&](float a_scale) {
        a_s.task = Task::Camp;
        a_s.until = m_hours + a_scale * std::uniform_real_distribution<float>(d.campMin, d.campMax)(m_rng);
    };
    if (d.speed <= 0) { camp(1.0f); return; }
    // Ferals keep to their holes by day.
    if (a_s.faction == Faction::Feral && !Night()) { camp(0.5f); return; }

    int target = -1;
    if (a_s.faction == Faction::Trader) {
        std::vector<int> hubs;
        for (const char* name : kTradeHubs)
            if (int n = FindNode(name); n >= 0 && n != a_s.at) hubs.push_back(n);
        if (!hubs.empty()) target = hubs[std::uniform_int_distribution<size_t>(0, hubs.size() - 1)(m_rng)];
    } else {
        // Places within reach (breadth-first, d.roam links; raids look a little further).
        const int reach = roll(m_rng) < d.raidChance ? d.roam + 1 : d.roam;
        std::vector<int> near;
        std::vector<int> depth(m_nodes.size(), -1);
        std::queue<int> q;
        q.push(a_s.at);
        depth[a_s.at] = 0;
        while (!q.empty()) {
            int c = q.front();
            q.pop();
            if (c != a_s.at) near.push_back(c);
            if (depth[c] >= reach) continue;
            for (int l : m_nodes[c].links)
                if (depth[l] < 0) { depth[l] = depth[c] + 1; q.push(l); }
        }
        if (reach > d.roam) {
            // A raid: the weakest hostile squad camping in reach.
            const Squad* prey = nullptr;
            for (const auto& o : m_squads)
                if (o.task == Task::Camp && Hostile(a_s.faction, o.faction) && depth[o.at] > 0 &&
                    o.Strength() < a_s.Strength() * 1.2f && (!prey || o.Strength() < prey->Strength()))
                    prey = &o;
            if (prey) {
                target = prey->at;
                Event(fmt::format("A {} heads out from {} to attack a {} at {}", Label(a_s), m_nodes[a_s.at].name, Label(*prey), m_nodes[target].name));
                Say(a_s, Line::Raid, target, prey->faction, 1);
            }
        }
        if (target < 0) {
            if (a_s.at != a_s.home && roll(m_rng) < 0.5f) target = a_s.home;
            else if (!near.empty()) target = near[std::uniform_int_distribution<size_t>(0, near.size() - 1)(m_rng)];
        }
    }
    if (target < 0 || target == a_s.at) { camp(1.0f); return; }
    a_s.path = Route(a_s.at, target);
    a_s.task = a_s.path.empty() ? Task::Camp : Task::Travel;
    if (a_s.task == Task::Camp) camp(1.0f);
    else if (roll(m_rng) < 0.15f) Say(a_s, Line::Travel, target, Faction::Raider, 0);
}

void ALife::Move(Squad& a_s, double a_hours)
{
    if (a_s.path.empty()) {
        a_s.task = Task::Camp;
        a_s.until = m_hours + std::uniform_real_distribution<float>(D(a_s.faction).campMin, D(a_s.faction).campMax)(m_rng);
        return;
    }
    // Fleeing squads run.
    const float speed = D(a_s.faction).speed * (a_s.task == Task::Flee ? 1.6f : 1.0f);
    float budget = static_cast<float>(speed * a_hours * kRealSecondsPerGameHour);
    while (budget > 0 && !a_s.path.empty()) {
        const Node& n = m_nodes[a_s.path.front()];
        const float dx = n.x - a_s.x, dy = n.y - a_s.y;
        const float left = std::hypot(dx, dy);
        if (left <= budget) {
            a_s.x = n.x;
            a_s.y = n.y;
            a_s.at = a_s.path.front();
            a_s.path.erase(a_s.path.begin());
            budget -= left;
        } else {
            a_s.x += dx / left * budget;
            a_s.y += dy / left * budget;
            budget = 0;
        }
    }
    if (a_s.path.empty()) {
        if (a_s.task == Task::Flee) Event(fmt::format("The {} made it back to {}", Label(a_s), m_nodes[a_s.at].name));
        a_s.task = Task::Camp;
        a_s.until = m_hours + std::uniform_real_distribution<float>(D(a_s.faction).campMin, D(a_s.faction).campMax)(m_rng);
    }
}

// Hostile squads in sight of each other fight (one of them on the move); each side hurts the other by its strength. A side down to under
// 40% of its people breaks and runs home; a side with nobody left is gone.
void ALife::Encounters(double a_hours)
{
    for (size_t i = 0; i < m_squads.size(); ++i)
        for (size_t k = i + 1; k < m_squads.size(); ++k) {
            Squad& a = m_squads[i];
            Squad& b = m_squads[k];
            if (a.owner || b.owner) continue;   // in a player's game: fights happen there, for real
            if (a.Alive() == 0 || b.Alive() == 0 || a.task == Task::Flee || b.task == Task::Flee) continue;
            if (a.task == Task::Fight && a.foe != b.id) continue;
            if (b.task == Task::Fight && b.foe != a.id) continue;
            if (!Hostile(a.faction, b.faction)) continue;
            // Fights start on the move (a patrol, a raid, a caravan): two camps don't go at each other.
            if (a.task == Task::Camp && b.task == Task::Camp) continue;
            if (std::abs(a.x - b.x) > kSight || std::abs(a.y - b.y) > kSight || std::hypot(a.x - b.x, a.y - b.y) > kSight) continue;
            if (a.task != Task::Fight) {
                Event(fmt::format("A {} and a {} clash near {}", Label(a), Label(b), m_nodes[Nearest(a.x, a.y)].name));
                Say(a, Line::Contact, -1, b.faction, 1);
                a.task = b.task = Task::Fight;
                a.foe = b.id;
                b.foe = a.id;
            }
            const float sa = a.Strength(), sb = b.Strength();
            auto hurt = [&](Squad& v, float power) {
                float dmg = static_cast<float>(power * a_hours * 2.0);
                std::vector<Member*> up;
                for (auto& m : v.members) if (m.hp > 0) up.push_back(&m);
                while (dmg > 0 && !up.empty()) {
                    const size_t pick = std::uniform_int_distribution<size_t>(0, up.size() - 1)(m_rng);
                    const float hit = std::min(dmg, 0.35f);
                    up[pick]->hp -= hit * std::uniform_real_distribution<float>(0.5f, 1.5f)(m_rng);
                    dmg -= hit;
                    if (up[pick]->hp <= 0) up.erase(up.begin() + pick);
                }
            };
            hurt(a, sb);
            hurt(b, sa);
            auto settle = [&](Squad& loser, Squad& winner) {
                if (loser.Alive() == 0) {
                    Event(fmt::format("The {} wiped out a {} near {}", Label(winner), Label(loser), m_nodes[Nearest(loser.x, loser.y)].name));
                    Say(winner, Line::Win, -1, loser.faction, 1);
                    News(winner.faction, loser.faction, Nearest(loser.x, loser.y), true);
                    for (auto& h : m_homes)
                        if (h.node == loser.home && h.faction == loser.faction) h.nextRefill = std::max(h.nextRefill, m_hours + m_respawnHours);
                } else if (loser.Alive() * 10 < static_cast<int>(loser.members.size()) * 6 ||
                           (loser.faction == Faction::Trader && loser.Alive() < static_cast<int>(loser.members.size()))) {
                    // Losing 40% of its people breaks a squad (a caravan runs at its first loss).
                    Event(fmt::format("The {} breaks and runs from a {}", Label(loser), Label(winner)));
                    Say(loser, Line::Flee, -1, winner.faction, 1);
                    News(winner.faction, loser.faction, Nearest(loser.x, loser.y), false);
                    loser.task = Task::Flee;
                    loser.path = Route(Nearest(loser.x, loser.y), loser.home);
                    if (loser.path.empty()) loser.task = Task::Camp, loser.until = m_hours + 4.0;
                } else {
                    return false;
                }
                winner.task = Task::Camp;
                winner.until = m_hours + 2.0;   // licks its wounds, loots, then carries on
                winner.at = Nearest(winner.x, winner.y);
                winner.path.clear();
                return true;
            };
            if (!settle(a, b)) settle(b, a);
        }
}

// Emptied homes fill up again, one squad at a time, every m_respawnHours game hours.
void ALife::Refill()
{
    for (auto& h : m_homes) {
        if (m_hours < h.nextRefill || Cleared(h.node)) continue;
        const int have = static_cast<int>(std::count_if(m_squads.begin(), m_squads.end(), [&](const Squad& s) { return s.home == h.node && s.faction == h.faction; }));
        if (have >= h.quota) continue;
        Squad s = MakeSquad(h.faction, h.node);
        Event(fmt::format("A new {} moves into {}", Label(s), m_nodes[h.node].name));
        Say(s, Line::Settle, h.node, Faction::Raider, 0);
        m_squads.push_back(std::move(s));
        h.nextRefill = m_hours + m_respawnHours;
    }
}

std::vector<Squad*> ALife::OfflineNear(float a_x, float a_y, float a_radius)
{
    std::vector<Squad*> out;
    for (auto& s : m_squads)
        if (!s.owner && s.Alive() > 0 && std::abs(s.x - a_x) < a_radius && std::abs(s.y - a_y) < a_radius &&
            std::hypot(s.x - a_x, s.y - a_y) < a_radius)
            out.push_back(&s);
    std::sort(out.begin(), out.end(), [&](const Squad* a, const Squad* b) {
        return std::hypot(a->x - a_x, a->y - a_y) < std::hypot(b->x - a_x, b->y - a_y);
    });
    return out;
}

// A squad in a player's game: the game walks and fights it; its plans (when to move on, where to) stay here.
// It reached the next place when its people stand within kArrive of that place.
void ALife::ThinkOnline(Squad& a_s)
{
    constexpr float kArrive = 1500.0f;
    if (a_s.task == Task::Camp) {
        if (m_hours >= a_s.until) Think(a_s);
        return;
    }
    if (a_s.task == Task::Fight) { a_s.task = Task::Camp; a_s.until = m_hours + 1.0; return; }
    while (!a_s.path.empty()) {
        const Node& n = m_nodes[a_s.path.front()];
        if (std::hypot(n.x - a_s.x, n.y - a_s.y) > kArrive) break;
        a_s.at = a_s.path.front();
        a_s.path.erase(a_s.path.begin());
        a_s.spot = -1;
    }
    if (a_s.path.empty()) {
        a_s.task = Task::Camp;
        a_s.until = m_hours + std::uniform_real_distribution<float>(D(a_s.faction).campMin, D(a_s.faction).campMax)(m_rng);
    }
}

Node::Spot ALife::CampSpot(Squad& a_s, const std::vector<std::pair<float, float>>& a_avoid)
{
    const Node& n = m_nodes[a_s.task == Task::Camp ? a_s.at : Nearest(a_s.x, a_s.y)];
    if (n.spawns.empty()) return { n.x, n.y, n.z };
    if (a_s.spot < 0 || a_s.spot >= static_cast<int>(n.spawns.size())) {
        // The spot farthest from every player (a squad shouldn't pop up in front of anyone).
        float best = -1;
        for (int i = 0; i < static_cast<int>(n.spawns.size()); ++i) {
            float nearest = 1e30f;
            for (auto [px, py] : a_avoid) nearest = std::min(nearest, std::hypot(n.spawns[i].x - px, n.spawns[i].y - py));
            if (nearest > best) { best = nearest; a_s.spot = i; }
        }
    }
    return n.spawns[a_s.spot];
}

uint32_t ALife::GoalKey(const Squad& a_s) const
{
    const bool moving = (a_s.task == Task::Travel || a_s.task == Task::Flee) && !a_s.path.empty();
    // Camping where their own camp now stands: a new goal (its middle instead of an NPC spot).
    const Camp* c = moving ? nullptr : CampAt(a_s.at);
    const bool ownCamp = c && c->faction == a_s.faction && c->level > 0;
    return (moving ? 0x80000000u : 0u) | (ownCamp ? 0x40000000u : 0u) | static_cast<uint32_t>(moving ? a_s.path.front() : a_s.at);
}

// A player's game tells how its squad is doing: who's still up, where they are. Deaths there are deaths here.
void ALife::Report(uint32_t a_id, int a_count, const float* a_hp, float a_x, float a_y, bool a_final)
{
    Squad* s = ById(a_id);
    if (!s) return;
    const int before = s->Alive();
    for (size_t i = 0; i < s->members.size(); ++i)
        s->members[i].hp = static_cast<int>(i) < a_count ? std::clamp(a_hp[i], 0.0f, 1.0f) : 0.0f;
    if (a_x != 0.0f || a_y != 0.0f) { s->x = a_x; s->y = a_y; }
    const int after = s->Alive();
    if (after < before)
        Event(fmt::format("{} of the {} fell near {}", before - after, Label(*s), m_nodes[Nearest(s->x, s->y)].name));
    if (after < before && after > 0) Say(*s, Line::Losses, -1, Faction::Raider, 2);
    if (after == 0) {
        for (auto& h : m_homes)
            if (h.node == s->home && h.faction == s->faction) h.nextRefill = std::max(h.nextRefill, m_hours + m_respawnHours);
        // Wiped out where they camped, in a player's game, with nobody else living there: if that place is
        // someone's home or has a camp, it's cleared for good.
        const int at = s->task == Task::Camp ? s->at : Nearest(s->x, s->y);
        const bool lived = CampAt(at) || std::any_of(m_homes.begin(), m_homes.end(), [&](const HomeSlot& h) { return h.node == at; });
        if (lived && !Cleared(at) && Nearest(s->x, s->y) == at &&
            std::none_of(m_squads.begin(), m_squads.end(), [&](const Squad& o) {
                return &o != s && o.Alive() > 0 && o.task == Task::Camp && o.at == at; }))
            Clear(at, *s);
        s->owner = 0;   // removed on the next tick
    } else if (a_final) {
        Release(*s);
    }
}

// Back to the simulation, from where its people actually are.
void ALife::Release(Squad& a_s)
{
    a_s.owner = 0;
    a_s.at = Nearest(a_s.x, a_s.y);
    if (a_s.task == Task::Fight) a_s.task = Task::Camp, a_s.until = m_hours + 1.0;
    if (a_s.task == Task::Travel || a_s.task == Task::Flee) {
        const int goal = a_s.path.empty() ? a_s.home : a_s.path.back();
        a_s.path = Route(a_s.at, goal);
    }
}

std::vector<int> ALife::Route(int a_from, int a_to) const
{
    const int n = static_cast<int>(m_nodes.size());
    std::vector<float> best(n, 1e30f);
    std::vector<int> prev(n, -1);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    best[a_from] = 0;
    q.push({ 0.0f, a_from });
    while (!q.empty()) {
        auto [c, u] = q.top();
        q.pop();
        if (u == a_to) break;
        if (c > best[u]) continue;
        for (int v : m_nodes[u].links) {
            const float w = c + std::hypot(m_nodes[u].x - m_nodes[v].x, m_nodes[u].y - m_nodes[v].y);
            if (w < best[v]) { best[v] = w; prev[v] = u; q.push({ w, v }); }
        }
    }
    std::vector<int> path;
    if (prev[a_to] < 0 && a_to != a_from) return path;
    for (int v = a_to; v != a_from; v = prev[v]) path.push_back(v);
    std::reverse(path.begin(), path.end());
    return path;
}

int ALife::Nearest(float a_x, float a_y) const
{
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
        const float d = std::hypot(m_nodes[i].x - a_x, m_nodes[i].y - a_y);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int ALife::FindNode(const std::string& a_name) const
{
    auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    const std::string want = lower(a_name);
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
        if (lower(m_nodes[i].name) == want) return i;
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
        if (lower(m_nodes[i].name).find(want) != std::string::npos) return i;
    return -1;
}

Squad* ALife::ById(uint32_t a_id)
{
    for (auto& s : m_squads) if (s.id == a_id) return &s;
    return nullptr;
}

std::string ALife::Label(const Squad& a_s) const
{
    return fmt::format("{} #{} ({})", D(a_s.faction).group, a_s.id, a_s.Alive());
}

bool ALife::Night() const
{
    const double h = std::fmod(m_hours, 24.0);
    return h < 6.0 || h >= 20.0;
}

void ALife::Event(std::string a_text)
{
    const int day = static_cast<int>(m_hours / 24) + 1;
    const int hh = static_cast<int>(std::fmod(m_hours, 24.0));
    const int mm = static_cast<int>(std::fmod(m_hours * 60.0, 60.0));
    a_text = fmt::format("day {} {:02}:{:02} - {}", day, hh, mm, a_text);
    spdlog::info("[ALife] {}", a_text);
    m_events.push_back(std::move(a_text));
    if (m_events.size() > 400) m_events.erase(m_events.begin(), m_events.begin() + 200);
}

void ALife::PrintSummary() const
{
    int per[static_cast<int>(Faction::Count)]{}, people[static_cast<int>(Faction::Count)]{};
    int fighting = 0, travelling = 0;
    for (const auto& s : m_squads) {
        per[static_cast<int>(s.faction)]++;
        people[static_cast<int>(s.faction)] += s.Alive();
        fighting += s.task == Task::Fight;
        travelling += s.task == Task::Travel || s.task == Task::Flee;
    }
    spdlog::info("[ALife] day {} {:02}:00, speed x{}: {} squads ({} on the move, {} fighting)", static_cast<int>(m_hours / 24) + 1,
        static_cast<int>(std::fmod(m_hours, 24.0)), m_speed, m_squads.size(), travelling, fighting);
    for (int i = 0; i < static_cast<int>(Faction::Count); ++i)
        if (per[i]) spdlog::info("[ALife]   {:<10} {:>3} squads, {:>4} alive", kDefs[i].name, per[i], people[i]);
    int built = 0;
    for (const auto& c : m_camps) built += std::max(0, c.level);
    spdlog::info("[ALife]   {} camps, {} things built", m_camps.size(), built);
}

void ALife::PrintNear(const std::string& a_place) const
{
    const int n = FindNode(a_place);
    if (n < 0) { spdlog::warn("[ALife] no place called '{}'", a_place); return; }
    spdlog::info("[ALife] around {}:", m_nodes[n].name);
    if (const Camp* c = CampAt(n)) {
        std::string things;
        for (const auto& it : CampItems(*c)) things += std::string(things.empty() ? "" : ", ") + it.name;
        spdlog::info("[ALife]   {} camp, {} built{}{}", FactionName(c->faction), c->level, things.empty() ? "" : ": ", things);
    }
    static const char* kTask[] = { "camping", "travelling", "fighting", "fleeing" };
    for (const auto& s : m_squads) {
        const float d = std::hypot(s.x - m_nodes[n].x, s.y - m_nodes[n].y);
        if (d > 15000.0f) continue;
        spdlog::info("[ALife]   {} {} {:.0f} units away (home {}{})", Label(s), kTask[static_cast<int>(s.task)], d,
            m_nodes[s.home].name, s.path.empty() ? "" : fmt::format(", heading for {}", m_nodes[s.path.back()].name));
    }
}

bool ALife::SpawnAt(const std::string& a_faction, const std::string& a_place)
{
    const int n = FindNode(a_place);
    if (n < 0) return false;
    for (int i = 0; i < static_cast<int>(Faction::Count); ++i)
        if (a_faction == kDefs[i].name) {
            Squad s = MakeSquad(static_cast<Faction>(i), n);
            s.until = m_hours + 2.0;
            Event(fmt::format("A {} turns up at {}", Label(s), m_nodes[n].name));
            m_squads.push_back(std::move(s));
            return true;
        }
    return false;
}

bool ALife::SendTo(uint32_t a_id, const std::string& a_place)
{
    Squad* s = ById(a_id);
    const int n = FindNode(a_place);
    if (!s || n < 0) return false;
    s->at = Nearest(s->x, s->y);
    s->path = Route(s->at, n);
    s->task = s->path.empty() ? Task::Camp : Task::Travel;
    Event(fmt::format("The {} sets off for {}", Label(*s), m_nodes[n].name));
    Say(*s, Line::Travel, n, Faction::Raider, 1);
    return true;
}

void ALife::PrintEvents(size_t a_n) const
{
    const size_t from = m_events.size() > a_n ? m_events.size() - a_n : 0;
    for (size_t i = from; i < m_events.size(); ++i) spdlog::info("[ALife] {}", m_events[i]);
}

}  // namespace Wars
