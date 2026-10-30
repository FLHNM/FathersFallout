// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.
//
// Wasteland Wars sides: every player plays solo, in a crew (player-made: a name, a short tag, one of 8 colours, a
// leader who keeps it open or invite-only) or with one of the Commonwealth's big four factions. Picked when they
// first join (and from F1 after); a change after that waits out a cooldown, so nobody hops sides mid-fight. Crews
// share their claims. Kept by player name on the server, saved to wars_teams.json.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Wars {

inline constexpr const char* kTeamsPath = "./wars_teams.json";

enum class SideKind : uint8_t { None, Solo, Crew, Faction };
enum class Allegiance : uint8_t { Brotherhood, Minutemen, Railroad, Institute, Count };
inline constexpr int kCrewColours = 8;   // red, blue, white, orange, green, yellow, purple, cyan

struct Crew {
    uint32_t id = 0;
    std::string name, tag, leader;
    uint8_t colour = 0;
    bool open = true;                     // anyone may join; else the leader accepts requests
    std::vector<std::string> members;     // player names, the leader first
    std::vector<std::string> requests;    // asking to join (invite-only crews)
};

struct Side {
    SideKind kind = SideKind::None;
    Allegiance faction = Allegiance::Brotherhood;
    uint32_t crew = 0;
    int64_t changedAt = 0;                // unix seconds of the last change (the first pick doesn't count)
    int64_t pickedAt = 0;                 // unix seconds of the last pick of any kind (the misclick grace)
};

class Teams {
public:
    void Load(const std::string& a_path);
    void Save(const std::string& a_path) const;

    // What a player asks for. On refusal, a_why says why (shown to them).
    bool PickSolo(const std::string& a_who, std::string& a_why);
    bool PickFaction(const std::string& a_who, Allegiance a_f, std::string& a_why);
    bool Create(const std::string& a_who, const std::string& a_name, const std::string& a_tag, uint8_t a_colour, bool a_open,
        std::string& a_why);
    bool Join(const std::string& a_who, uint32_t a_crew, std::string& a_why);   // open: in; else a request
    bool Leave(const std::string& a_who, std::string& a_why);                  // -> solo
    bool Answer(const std::string& a_leader, const std::string& a_who, bool a_accept, std::string& a_why);
    bool SetOpen(const std::string& a_leader, bool a_open, std::string& a_why);

    const Side& SideOf(const std::string& a_who) const;
    const Crew* CrewById(uint32_t a_id) const;
    const std::map<uint32_t, Crew>& Crews() const { return m_crews; }
    // Who holds what a player captures: their crew ("crew:<id>"), else the player. And its name for people.
    std::string ClaimKey(const std::string& a_who) const;
    std::string ClaimName(const std::string& a_key) const;
    // Seconds before a_who may change side again (0 = now).
    int CooldownLeft(const std::string& a_who) const;
    uint32_t Version() const { return m_version; }
    // Admin: a player may change side again right away. False if the server doesn't know them.
    bool ResetCooldown(const std::string& a_who);

private:
    bool CanChange(const std::string& a_who, std::string& a_why) const;
    void Set(const std::string& a_who, Side a_side);
    void DropFromCrew(const std::string& a_who);
    void Changed();

    std::map<std::string, Side> m_sides;
    std::map<uint32_t, Crew> m_crews;
    uint32_t m_nextCrew = 1;
    uint32_t m_version = 1;
};

const char* AllegianceName(Allegiance a_f);

}  // namespace Wars
