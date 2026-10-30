// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.
//
// A-Life camps: squads live somewhere, and like players they build it up. While a squad is at home in the daytime
// its people work on their camp: a cooking fire first, then bedrolls, a tent, a weapons workbench, a chem station...
// (the faction's build list, all the game's own objects). A camp nobody tends falls apart a piece a day; a camp whose
// owners are away is taken over by whoever camps there next. Games near a camp place its things, and the NPCs' sandbox
// package uses them the way the game's own settlers do: they cook at the fire, sleep in the bedrolls, work at the
// benches. Everything is saved with the world (wars_alife.json).

#include "Wars/ALife.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <spdlog/spdlog.h>

namespace Wars {

namespace {

// Where a thing goes, the way people set up camp: the fire in the open middle, seats around it, benches, beds and
// tents with their backs to a wall, sandbags on the edge facing out.
enum class Place : uint8_t { Middle, Seat, Wall, Edge };

struct BuildStep {
    uint32_t form;      // Fallout4.esm base object
    const char* name;   // for the chronicle and the radio
    Place place;
};

// What each faction builds, in order (none = it doesn't build: ferals, animals, robots, caravans on the road).
// Furniture with NPC markers (fires, beds, benches, stools) is what their sandbox uses; the rest is the look.
const std::vector<BuildStep> kRaiderBuild{
    { 0x0010C3B6, "a cooking fire", Place::Middle },          // WorkbenchCookingFire
    { 0x000D75BC, "a bedroll", Place::Wall },               // NpcBedSleepingBagSleep01
    { 0x00238D88, "a tent", Place::Wall },                  // Workshop_RaiderTent01
    { 0x0017B3A4, "a weapons workbench", Place::Wall },     // workbenchWeaponsA
    { 0x000D75BC, "another bedroll", Place::Wall },
    { 0x000D83C4, "a stool by the fire", Place::Seat },     // NpcStoolIndustrialMetalSit01
    { 0x0012F2F5, "a chem station", Place::Wall },          // WorkbenchChemistryA
    { 0x00238D89, "a second tent", Place::Wall },           // Workshop_RaiderTent02
    { 0x0012EA9B, "an armor workbench", Place::Wall },      // WorkbenchArmorA
    { 0x000D75BC, "a third bedroll", Place::Wall },
};
const std::vector<BuildStep> kGunnerBuild{
    { 0x0010C3B6, "a cooking fire", Place::Middle },
    { 0x0014D090, "a cot", Place::Wall },                   // NpcBedMilitaryCotMattressLay01
    { 0x0002552C, "sandbags", Place::Edge },                // SandbagSingle02
    { 0x0017B3A4, "a weapons workbench", Place::Wall },
    { 0x0014D090, "another cot", Place::Wall },
    { 0x00025529, "more sandbags", Place::Edge },           // SandbagSingle01
    { 0x0012EA9B, "an armor workbench", Place::Wall },
    { 0x000D83C4, "a stool", Place::Seat },
    { 0x0002552C, "a sandbag wall", Place::Edge },
    { 0x0014D090, "a third cot", Place::Wall },
};
const std::vector<BuildStep> kMutantBuild{
    { 0x001796AE, "a cooking cauldron", Place::Middle },      // SupermutantCookingCauldron
    { 0x002039E4, "a spike pile", Place::Edge },            // SuperMutantCampSpikePile01
    { 0x000D75BC, "a bedroll", Place::Wall },
    { 0x0017B3A4, "a weapons workbench", Place::Wall },
    { 0x002039E5, "more spikes", Place::Edge },             // SuperMutantCampSpikePile02
    { 0x000D75BC, "another bedroll", Place::Wall },
};
const std::vector<BuildStep> kSynthBuild{
    { 0x0017B3A4, "a weapons workbench", Place::Wall },
    { 0x0012EA9B, "an armor workbench", Place::Wall },
    { 0x000D83C4, "a work stool", Place::Seat },
    { 0x0012F2F5, "a chemistry station", Place::Wall },
};
const std::vector<BuildStep> kBrotherhoodBuild{
    { 0x0014D090, "a cot", Place::Wall },
    { 0x0002552C, "sandbags", Place::Edge },
    { 0x0017B3A4, "a weapons workbench", Place::Wall },
    { 0x0014D090, "another cot", Place::Wall },
    { 0x00025529, "a sandbag wall", Place::Edge },
    { 0x0012EA9B, "an armor workbench", Place::Wall },
};
const std::vector<BuildStep> kMinutemenBuild{
    { 0x0010C3B6, "a cooking fire", Place::Middle },
    { 0x000D75BC, "a bedroll", Place::Wall },
    { 0x000D83C4, "a stool by the fire", Place::Seat },
    { 0x0017B3A4, "a weapons workbench", Place::Wall },
    { 0x000D75BC, "another bedroll", Place::Wall },
    { 0x0002552C, "sandbags", Place::Edge },
};
const std::vector<BuildStep> kNone;

const std::vector<BuildStep>& BuildList(Faction a_f)
{
    switch (a_f) {
    case Faction::Raider: return kRaiderBuild;
    case Faction::Gunner: return kGunnerBuild;
    case Faction::Mutant: return kMutantBuild;
    case Faction::Synth: return kSynthBuild;
    case Faction::Brotherhood: return kBrotherhoodBuild;
    case Faction::Minutemen: return kMinutemenBuild;
    default: return kNone;
    }
}

bool Builds(Faction a_f) { return !BuildList(a_f).empty(); }

// What a camp at this place builds: its faction's list minus what the game's own furniture there already gives
// (a cooking spot, workbenches, seats, some beds). The look stays: tents, sandbags, spikes.
std::vector<BuildStep> Steps(const Node& a_n, Faction a_f)
{
    std::vector<BuildStep> out;
    int beds = a_n.hubBeds;
    for (const auto& st : BuildList(a_f)) {
        const std::string name = st.name;
        auto has = [&](const char* a_w) { return name.find(a_w) != std::string::npos; };
        if ((has("fire") || has("cauldron")) && a_n.hubCook > 0) continue;
        if ((has("workbench") || has("chem")) && a_n.hubBenches > 0) continue;
        if (has("stool") && a_n.hubSeats >= 2) continue;
        if ((has("bedroll") || has("cot")) && beds > 0) { --beds; continue; }
        out.push_back(st);
    }
    return out;
}

constexpr double kWorkPerMember = 0.06;   // per game hour of daylight at home; the next thing takes 1 + level/4 of it
constexpr double kAwayHours = 6.0;        // owners gone this long: someone else camping there takes it
constexpr double kDecayHours = 24.0;      // untended: a piece falls apart every game day
constexpr float kCampRadius = 420.0f;     // everything within the sandbox's reach around the middle (512)

}  // namespace

// The middle of a place's camp: the spot where the game itself put the most NPCs close together (its own camp).
bool ALife::CampMiddle(int a_node, Node::Spot& a_out) const
{
    if (a_node < 0 || a_node >= static_cast<int>(m_nodes.size())) return false;
    if (m_nodes[a_node].hasCamp) { a_out = m_nodes[a_node].camp; return true; }   // open ground (make_world.py)
    const auto& sp = m_nodes[a_node].spawns;
    if (sp.empty()) return false;
    int best = 0, bestN = -1;
    for (int i = 0; i < static_cast<int>(sp.size()); ++i) {
        int n = 0;
        for (const auto& o : sp)
            if (std::hypot(o.x - sp[i].x, o.y - sp[i].y) < kCampRadius && std::abs(o.z - sp[i].z) < 300.0f) ++n;
        if (n > bestN) { bestN = n; best = i; }
    }
    a_out = sp[best];
    return true;
}

const Camp* ALife::CampAt(int a_node) const
{
    for (const auto& c : m_camps) if (c.node == a_node) return &c;
    return nullptr;
}

Camp* ALife::CampAtMut(int a_node)
{
    for (auto& c : m_camps) if (c.node == a_node) return &c;
    return nullptr;
}

// Where its things stand: on the other ground spots near the middle first (the game's own NPC spots: real floor),
// then around the middle in a ring. Same camp = same places every time, in every game.
std::vector<Camp::Item> ALife::CampItems(const Camp& a_c) const
{
    std::vector<Camp::Item> out;
    Node::Spot mid;
    if (!CampMiddle(a_c.node, mid)) return out;
    const auto& node = m_nodes[a_c.node];
    const auto list = Steps(node, a_c.faction);
    if (node.hasCamp) {
        // Each thing at its kind of spot (make_world.py: open ground, each at the ground's own height); a camp with
        // no walls near, or out of seats, uses the open spots around the middle.
        size_t used[4]{}, open = 0;
        const std::vector<Node::Facing>* pools[4] = { nullptr, &node.campSeats, &node.campWalls, &node.campEdge };
        const int n = std::min<int>(a_c.level, static_cast<int>(list.size()));
        for (int i = 0; i < n; ++i) {
            const auto kind = static_cast<int>(list[i].place);
            if (list[i].place == Place::Middle && used[0]++ == 0) {
                out.push_back({ list[i].form, mid.x, mid.y, mid.z, 0.0f, list[i].name });
                continue;
            }
            if (const auto* pool = pools[kind]; pool && used[kind] < pool->size()) {
                const auto& p = (*pool)[used[kind]++];
                out.push_back({ list[i].form, p.x, p.y, p.z, p.rz, list[i].name });
                continue;
            }
            if (open >= node.campItems.size()) continue;   // no room left: it isn't placed
            const auto& p = node.campItems[open++];
            out.push_back({ list[i].form, p.x, p.y, p.z, std::atan2(mid.x - p.x, mid.y - p.y), list[i].name });
        }
        return out;
    }
    // An older wars_world.json (no camps in it): near the game's own NPC spots.
    std::vector<Node::Spot> near;
    for (const auto& o : m_nodes[a_c.node].spawns) {
        const float d = std::hypot(o.x - mid.x, o.y - mid.y);
        if (d > 90.0f && d < kCampRadius && std::abs(o.z - mid.z) < 200.0f) near.push_back(o);
    }
    std::sort(near.begin(), near.end(), [&](const Node::Spot& a, const Node::Spot& b) {
        return std::hypot(a.x - mid.x, a.y - mid.y) < std::hypot(b.x - mid.x, b.y - mid.y);
    });
    const int n = std::min<int>(a_c.level, static_cast<int>(list.size()));
    for (int i = 0; i < n; ++i) {
        const float turn = static_cast<float>(i) * 2.39996f + static_cast<float>(a_c.node % 7) * 0.9f;   // golden angle
        float x, y, z;
        if (i < static_cast<int>(near.size())) {
            // Next to an NPC spot, not on it (someone spawns there).
            x = near[i].x + std::cos(turn) * 70.0f;
            y = near[i].y + std::sin(turn) * 70.0f;
            z = near[i].z;
        } else {
            const float r = 150.0f + 55.0f * static_cast<float>(i % 4);
            x = mid.x + std::cos(turn) * r;
            y = mid.y + std::sin(turn) * r;
            // The ground's height there: as at the nearest place the game put an NPC (on a slope, closer beats flat).
            z = mid.z;
            float best = std::hypot(x - mid.x, y - mid.y);
            for (const auto& o : near)
                if (const float d = std::hypot(x - o.x, y - o.y); d < best) { best = d; z = o.z; }
        }
        // Facing the middle (the game's heading: 0 = north, clockwise).
        const float rz = std::atan2(mid.x - x, mid.y - y);
        out.push_back({ list[i].form, x, y, z, rz, list[i].name });
    }
    return out;
}

// Daylight work at home, decay of untended camps, takeovers.
void ALife::Build(double a_hours)
{
    std::set<std::pair<int, Faction>> home;   // who is camping where right now
    for (const auto& s : m_squads)
        if (s.task == Task::Camp && s.Alive() > 0) home.insert({ s.at, s.faction });
    const bool day = !Night();
    for (auto& s : m_squads) {
        if (s.task != Task::Camp || s.Alive() == 0 || !Builds(s.faction)) continue;
        Camp* c = CampAtMut(s.at);
        if (!c) {
            Node::Spot mid;
            if (s.at != s.home || Cleared(s.at) || !CampMiddle(s.at, mid)) continue;   // a new camp only at home
            m_camps.push_back({ s.at, s.faction, 0, 0.0, m_hours, 1 });
            c = &m_camps.back();
        }
        if (c->faction != s.faction) {
            // Its owners aren't here: whoever camps here now makes it theirs (and wrecks half of it).
            if (home.count({ c->node, c->faction }) || m_hours - c->tended < kAwayHours) continue;
            const Faction was = c->faction;
            c->faction = s.faction;
            c->level = std::min<int>(c->level / 2, static_cast<int>(Steps(m_nodes[c->node], s.faction).size()));
            c->progress = 0;
            c->tended = m_hours;
            c->version++;
            Event(fmt::format("The {} took over the {} camp at {}", Label(s), FactionName(was), m_nodes[c->node].name));
            Say(s, Line::Takeover, c->node, was, 1);
            continue;
        }
        c->tended = m_hours;
        const auto steps = Steps(m_nodes[c->node], c->faction);
        const int max = static_cast<int>(steps.size());
        if (!day && c->faction != Faction::Synth) continue;   // they sleep at night
        if (c->level >= max) continue;
        c->progress += a_hours * s.Alive() * kWorkPerMember;
        if (c->progress < 1.0 + 0.25 * c->level) continue;
        c->progress = 0;
        const char* thing = steps[c->level].name;
        c->level++;
        c->version++;
        Event(fmt::format("The {} built {} at {}", Label(s), thing, m_nodes[c->node].name));
        Say(s, Line::Build, c->node, Faction::Raider, 1, thing);
    }
    for (auto& c : m_camps) {
        if (m_hours - c.tended < kDecayHours) continue;
        c.tended += kDecayHours;
        c.level--;   // an empty camp left alone another day (-1) is forgotten
        c.version++;
        if (c.level == 0) Event(fmt::format("The abandoned {} camp at {} has fallen apart", FactionName(c.faction), m_nodes[c.node].name));
    }
    m_camps.erase(std::remove_if(m_camps.begin(), m_camps.end(), [](const Camp& c) { return c.level < 0; }), m_camps.end());
}

// A world that's been lived in: every home of a building faction starts with part of its camp up.
void ALife::SeedCamps()
{
    for (const auto& h : m_homes) {
        Node::Spot mid;
        if (!Builds(h.faction) || CampAt(h.node) || !CampMiddle(h.node, mid)) continue;
        const int max = static_cast<int>(Steps(m_nodes[h.node], h.faction).size());
        if (max == 0) continue;
        const int level = std::uniform_int_distribution<int>(std::min(2, max), std::min(5, max))(m_rng);
        m_camps.push_back({ h.node, h.faction, level, 0.0, m_hours, 1 });
    }
    spdlog::info("[ALife] {} camps", m_camps.size());
}

namespace {
constexpr double kAttackMinHours = 48.0;   // a cleared place is attacked every 2-4 game days
constexpr double kAttackMaxHours = 96.0;
constexpr float kAttackReach = 16000.0f;   // from homes this close
constexpr double kSiegeHours = 3.0;        // attackers holding a captured place this long, undefended: it's lost
}

// Its own people are gone: nobody moves back in, their camp there goes, and the squads that lived there and were
// away find a new home.
void ALife::Clear(int a_node, const Squad& a_last)
{
    m_cleared[a_node] = m_hours + std::uniform_real_distribution<double>(kAttackMinHours, kAttackMaxHours)(m_rng);
    m_camps.erase(std::remove_if(m_camps.begin(), m_camps.end(), [&](const Camp& c) { return c.node == a_node; }), m_camps.end());
    for (auto& o : m_squads) {
        if (o.home != a_node || &o == &a_last || o.Alive() == 0) continue;
        int best = -1;
        float bd = 1e30f;
        for (const auto& h : m_homes)
            if (h.faction == o.faction && !Cleared(h.node))
                if (const float d = std::hypot(m_nodes[h.node].x - o.x, m_nodes[h.node].y - o.y); d < bd) { bd = d; best = h.node; }
        if (best >= 0) o.home = best;
    }
    Event(fmt::format("{} has been cleared out - the {}s there are gone for good", m_nodes[a_node].name, FactionName(a_last.faction)));
    spdlog::info("[ALife] {} cleared (next attack in {:.0f} game hours)", m_nodes[a_node].name, m_cleared[a_node] - m_hours);
}

// The wasteland comes for cleared places: when one's time is up, the nearest hostile squad camping at its home
// within reach is sent to attack it. Nobody there: they camp a while and move on (nothing moves back in).
// Attackers at a captured place: while they're in a player's game it's a fight there (nothing is decided here);
// holding it with nobody around for kSiegeHours, the claim falls - the place stays cleared, for anyone to retake.
void ALife::Threats(double a_hours)
{
    for (auto& [node, next] : m_cleared) {
        if (m_hours < next) continue;
        next = m_hours + std::uniform_real_distribution<double>(kAttackMinHours, kAttackMaxHours)(m_rng);
        SendAttack(node);
    }
    for (auto& s : m_squads) {
        if (s.raid < 0) continue;
        if (s.Alive() == 0) { s.raid = -1; continue; }
        const bool there = s.task == Task::Camp && s.at == s.raid;
        if (!there) {
            if (s.task != Task::Travel && s.task != Task::Fight) s.raid = -1;   // gave up / went elsewhere
            continue;
        }
        if (s.owner) continue;   // someone's there: it's being fought over in their game
        s.siege += a_hours;
        if (s.siege < kSiegeHours) continue;
        const int node = s.raid;
        s.raid = -1;
        if (const auto holder = ClaimOf(node); !holder.empty()) {
            SetClaim(node, std::string());
            Event(fmt::format("The {} overran {} - nobody was there to hold it", Label(s), m_nodes[node].name));
            Say(s, Line::Win, node, Faction::Trader, 2);
            spdlog::info("[ALife] {} lost {} to the {}", holder, m_nodes[node].name, Label(s));
        }
    }
}

bool ALife::SendAttack(int a_node)
{
    Squad* best = nullptr;
    float bd = kAttackReach;
    for (auto& o : m_squads) {
        if (o.owner || o.Alive() == 0 || o.task != Task::Camp || o.at == a_node || o.faction == Faction::Trader ||
            o.faction == Faction::Robot || Cleared(o.at))   // robots hold their posts
            continue;
        if (const float d = std::hypot(m_nodes[a_node].x - o.x, m_nodes[a_node].y - o.y); d < bd) { bd = d; best = &o; }
    }
    if (!best) return false;
    best->path = Route(best->at, a_node);
    if (best->path.empty()) return false;
    best->task = Task::Travel;
    best->raid = a_node;
    best->siege = 0;
    Event(fmt::format("A {} heads out from {} to attack {}", Label(*best), m_nodes[best->at].name, m_nodes[a_node].name));
    Say(*best, Line::Raid, a_node, Faction::Trader, 2);
    return true;
}

bool ALife::SetCleared(const std::string& a_place, bool a_cleared)
{
    const int n = FindNode(a_place);
    if (n < 0) return false;
    if (!a_cleared) {
        m_cleared.erase(n);
        Event(fmt::format("{} is open to its people again", m_nodes[n].name));
        return true;
    }
    Squad none;
    none.faction = Faction::Raider;
    for (const auto& h : m_homes) if (h.node == n) none.faction = h.faction;
    for (auto& o : m_squads)
        if (o.home == n && NodeOf(o) == n && !o.owner) for (auto& m : o.members) m.hp = 0.0f;   // gone with it
    Clear(n, none);
    return true;
}

bool ALife::AttackAt(const std::string& a_place)
{
    const int n = FindNode(a_place);
    return n >= 0 && SendAttack(n);
}

const std::string& ALife::ClaimOf(int a_node) const
{
    static const std::string kNobody;
    const auto it = m_claims.find(a_node);
    return it == m_claims.end() ? kNobody : it->second;
}

void ALife::SetClaim(int a_node, const std::string& a_owner)
{
    if (a_owner.empty()) m_claims.erase(a_node);
    else m_claims[a_node] = a_owner;
    m_claimsVersion++;
    Event(a_owner.empty() ? fmt::format("{} is nobody's now", m_nodes[a_node].name)
                          : fmt::format("{} has taken {}", a_owner, m_nodes[a_node].name));
}

bool ALife::Unclaim(const std::string& a_place)
{
    const int n = FindNode(a_place);
    if (n < 0) return false;
    SetClaim(n, std::string());
    return true;
}

bool ALife::BuildAt(const std::string& a_place, int a_levels)
{
    const int n = FindNode(a_place);
    Node::Spot mid;
    if (n < 0 || !CampMiddle(n, mid)) return false;
    Camp* c = CampAtMut(n);
    if (!c) {
        Faction f = Faction::Raider;
        for (const auto& h : m_homes) if (h.node == n && Builds(h.faction)) f = h.faction;
        m_camps.push_back({ n, f, 0, 0.0, m_hours, 1 });
        c = &m_camps.back();
    }
    const int max = static_cast<int>(Steps(m_nodes[n], c->faction).size());
    c->level = std::clamp(c->level + a_levels, 0, max);
    c->tended = m_hours;
    c->version++;
    Event(fmt::format("The {} camp at {} is now {} of {} built", FactionName(c->faction), m_nodes[n].name, c->level, max));
    return true;
}

}  // namespace Wars
