// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.

#include "Wars/Teams.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "ThirdParty/nlohmann/json.hpp"
#include <spdlog/spdlog.h>

namespace Wars {

namespace {
constexpr int64_t kCooldownSeconds = 3600;   // a real hour between side changes
constexpr int64_t kGraceSeconds = 120;       // changing again this soon after a pick is free (a misclick)
constexpr size_t kMaxName = 31, kMaxTag = 5;

int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Clean(std::string a_s, size_t a_max)
{
    a_s.erase(std::remove_if(a_s.begin(), a_s.end(), [](unsigned char c) { return c < 32 || c == 127; }), a_s.end());
    while (!a_s.empty() && a_s.front() == ' ') a_s.erase(a_s.begin());
    while (!a_s.empty() && a_s.back() == ' ') a_s.pop_back();
    return a_s.substr(0, a_max);
}

bool SameText(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}
}  // namespace

const char* AllegianceName(Allegiance a_f)
{
    switch (a_f) {
    case Allegiance::Brotherhood: return "Brotherhood of Steel";
    case Allegiance::Minutemen: return "Minutemen";
    case Allegiance::Railroad: return "Railroad";
    case Allegiance::Institute: return "Institute";
    default: return "?";
    }
}

void Teams::Load(const std::string& a_path)
{
    std::ifstream in(a_path);
    if (!in) return;
    nlohmann::json j;
    try { in >> j; } catch (...) { spdlog::warn("[Teams] {} unreadable: starting empty", a_path); return; }
    m_nextCrew = j.value("nextCrew", 1u);
    for (const auto& c : j.value("crews", nlohmann::json::array())) {
        Crew k;
        k.id = c.value("id", 0u);
        k.name = c.value("name", std::string());
        k.tag = c.value("tag", std::string());
        k.leader = c.value("leader", std::string());
        k.colour = static_cast<uint8_t>(c.value("colour", 0));
        k.open = c.value("open", true);
        k.members = c.value("members", std::vector<std::string>());
        k.requests = c.value("requests", std::vector<std::string>());
        if (k.id && !k.members.empty()) m_crews[k.id] = std::move(k);
    }
    for (const auto& s : j.value("sides", nlohmann::json::array())) {
        Side d;
        d.kind = static_cast<SideKind>(s.value("kind", 0));
        d.faction = static_cast<Allegiance>(s.value("faction", 0));
        d.crew = s.value("crew", 0u);
        d.changedAt = s.value("changed", int64_t{ 0 });
        d.pickedAt = s.value("picked", int64_t{ 0 });
        if (d.kind == SideKind::Crew && !m_crews.count(d.crew)) d.kind = SideKind::Solo;
        m_sides[s.value("who", std::string())] = d;
    }
    spdlog::info("[Teams] {} crews, {} players with a side", m_crews.size(), m_sides.size());
}

void Teams::Save(const std::string& a_path) const
{
    nlohmann::json j;
    j["nextCrew"] = m_nextCrew;
    auto& crews = j["crews"] = nlohmann::json::array();
    for (const auto& [id, c] : m_crews)
        crews.push_back({ { "id", c.id }, { "name", c.name }, { "tag", c.tag }, { "leader", c.leader }, { "colour", c.colour },
            { "open", c.open }, { "members", c.members }, { "requests", c.requests } });
    auto& sides = j["sides"] = nlohmann::json::array();
    for (const auto& [who, s] : m_sides)
        sides.push_back({ { "who", who }, { "kind", static_cast<int>(s.kind) }, { "faction", static_cast<int>(s.faction) },
            { "crew", s.crew }, { "changed", s.changedAt }, { "picked", s.pickedAt } });
    const std::string tmp = a_path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return;
        out << j.dump(1);
    }
    std::remove(a_path.c_str());
    std::rename(tmp.c_str(), a_path.c_str());
}

const Side& Teams::SideOf(const std::string& a_who) const
{
    static const Side kNone;
    const auto it = m_sides.find(a_who);
    return it == m_sides.end() ? kNone : it->second;
}

const Crew* Teams::CrewById(uint32_t a_id) const
{
    const auto it = m_crews.find(a_id);
    return it == m_crews.end() ? nullptr : &it->second;
}

int Teams::CooldownLeft(const std::string& a_who) const
{
    const auto& s = SideOf(a_who);
    if (s.kind == SideKind::None || s.changedAt == 0) return 0;
    return static_cast<int>(std::max<int64_t>(0, s.changedAt + kCooldownSeconds - Now()));
}

bool Teams::CanChange(const std::string& a_who, std::string& a_why) const
{
    if (Now() - SideOf(a_who).pickedAt < kGraceSeconds) return true;   // just picked: a misclick is put right for free
    if (const int left = CooldownLeft(a_who); left > 0) {
        a_why = "You changed sides recently: you can change again in " + std::to_string((left + 59) / 60) + " min.";
        return false;
    }
    return true;
}

void Teams::Changed()
{
    m_version++;
    Save(kTeamsPath);
}

// The first pick is free, and so is a change right after a pick (a misclick); any other change starts the cooldown.
void Teams::Set(const std::string& a_who, Side a_side)
{
    const auto& was = SideOf(a_who);
    const int64_t now = Now();
    const bool grace = was.kind == SideKind::None || now - was.pickedAt < kGraceSeconds;
    a_side.changedAt = grace ? was.changedAt : now;
    a_side.pickedAt = now;
    if (was.kind == SideKind::Crew && !(a_side.kind == SideKind::Crew && a_side.crew == was.crew)) DropFromCrew(a_who);
    m_sides[a_who] = a_side;
}

void Teams::DropFromCrew(const std::string& a_who)
{
    const auto& s = SideOf(a_who);
    auto it = m_crews.find(s.crew);
    if (it == m_crews.end()) return;
    auto& c = it->second;
    c.members.erase(std::remove(c.members.begin(), c.members.end(), a_who), c.members.end());
    if (c.members.empty()) {
        spdlog::info("[Teams] crew '{}' disbanded", c.name);
        m_crews.erase(it);
        return;
    }
    if (c.leader == a_who) c.leader = c.members.front();   // the longest-standing member leads on
}

bool Teams::ResetCooldown(const std::string& a_who)
{
    auto it = m_sides.find(a_who);
    if (it == m_sides.end()) return false;
    it->second.changedAt = 0;
    Changed();
    return true;
}

bool Teams::PickSolo(const std::string& a_who, std::string& a_why)
{
    if (SideOf(a_who).kind == SideKind::Solo) return true;
    if (!CanChange(a_who, a_why)) return false;
    Set(a_who, { SideKind::Solo });
    spdlog::info("[Teams] {} plays solo", a_who);
    Changed();
    return true;
}

bool Teams::PickFaction(const std::string& a_who, Allegiance a_f, std::string& a_why)
{
    if (a_f >= Allegiance::Count) { a_why = "Unknown faction."; return false; }
    const auto& s = SideOf(a_who);
    if (s.kind == SideKind::Faction && s.faction == a_f) return true;
    if (!CanChange(a_who, a_why)) return false;
    Side d{ SideKind::Faction };
    d.faction = a_f;
    Set(a_who, d);
    spdlog::info("[Teams] {} sides with the {}", a_who, AllegianceName(a_f));
    Changed();
    return true;
}

bool Teams::Create(const std::string& a_who, const std::string& a_name, const std::string& a_tag, uint8_t a_colour, bool a_open,
    std::string& a_why)
{
    const std::string name = Clean(a_name, kMaxName), tag = Clean(a_tag, kMaxTag);
    if (name.size() < 2) { a_why = "A crew needs a name (2 letters or more)."; return false; }
    if (tag.empty()) { a_why = "A crew needs a tag (1-5 letters)."; return false; }
    if (a_colour >= kCrewColours) { a_why = "Unknown colour."; return false; }
    for (const auto& [id, c] : m_crews)
        if (SameText(c.name, name) || SameText(c.tag, tag)) { a_why = "There's a crew with that name or tag already."; return false; }
    if (!CanChange(a_who, a_why)) return false;
    Crew c;
    c.id = m_nextCrew++;
    c.name = name;
    c.tag = tag;
    c.colour = a_colour;
    c.open = a_open;
    c.leader = a_who;
    c.members = { a_who };
    const uint32_t id = c.id;
    Side d{ SideKind::Crew };
    d.crew = id;
    Set(a_who, d);      // leaves an old crew first
    m_crews[id] = std::move(c);
    spdlog::info("[Teams] {} made the crew [{}] {}", a_who, tag, name);
    Changed();
    return true;
}

bool Teams::Join(const std::string& a_who, uint32_t a_crew, std::string& a_why)
{
    auto it = m_crews.find(a_crew);
    if (it == m_crews.end()) { a_why = "That crew is gone."; return false; }
    auto& c = it->second;
    if (std::find(c.members.begin(), c.members.end(), a_who) != c.members.end()) return true;
    if (!CanChange(a_who, a_why)) return false;
    if (!c.open) {
        if (std::find(c.requests.begin(), c.requests.end(), a_who) == c.requests.end()) c.requests.push_back(a_who);
        a_why = "Asked to join " + c.name + ": its leader " + c.leader + " decides.";
        Changed();
        return false;
    }
    c.members.push_back(a_who);
    Side d{ SideKind::Crew };
    d.crew = a_crew;
    Set(a_who, d);
    spdlog::info("[Teams] {} joined [{}] {}", a_who, c.tag, c.name);
    Changed();
    return true;
}

bool Teams::Leave(const std::string& a_who, std::string& a_why)
{
    if (SideOf(a_who).kind != SideKind::Crew) { a_why = "You're not in a crew."; return false; }
    return PickSolo(a_who, a_why);
}

bool Teams::Answer(const std::string& a_leader, const std::string& a_who, bool a_accept, std::string& a_why)
{
    const auto& s = SideOf(a_leader);
    auto it = m_crews.find(s.crew);
    if (s.kind != SideKind::Crew || it == m_crews.end() || it->second.leader != a_leader) { a_why = "Only a crew's leader can do that."; return false; }
    auto& c = it->second;
    auto r = std::find(c.requests.begin(), c.requests.end(), a_who);
    if (r == c.requests.end()) { a_why = a_who + " isn't asking to join."; return false; }
    c.requests.erase(r);
    if (a_accept) {
        std::string why;
        if (!CanChange(a_who, why)) { a_why = a_who + " can't change sides yet."; Changed(); return false; }
        c.members.push_back(a_who);
        Side d{ SideKind::Crew };
        d.crew = c.id;
        Set(a_who, d);
        spdlog::info("[Teams] {} let {} into [{}] {}", a_leader, a_who, c.tag, c.name);
    }
    Changed();
    return true;
}

bool Teams::SetOpen(const std::string& a_leader, bool a_open, std::string& a_why)
{
    const auto& s = SideOf(a_leader);
    auto it = m_crews.find(s.crew);
    if (s.kind != SideKind::Crew || it == m_crews.end() || it->second.leader != a_leader) { a_why = "Only a crew's leader can do that."; return false; }
    it->second.open = a_open;
    Changed();
    return true;
}

std::string Teams::ClaimKey(const std::string& a_who) const
{
    const auto& s = SideOf(a_who);
    return s.kind == SideKind::Crew && m_crews.count(s.crew) ? "crew:" + std::to_string(s.crew) : a_who;
}

std::string Teams::ClaimName(const std::string& a_key) const
{
    if (a_key.rfind("crew:", 0) == 0) {
        const auto* c = CrewById(static_cast<uint32_t>(std::strtoul(a_key.c_str() + 5, nullptr, 10)));
        return c ? "[" + c->tag + "] " + c->name : "a crew that's gone";
    }
    return a_key;
}

}  // namespace Wars
