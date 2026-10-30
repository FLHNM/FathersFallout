// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.
//
// A-Life radio chatter, in the spirit of S.T.A.L.K.E.R.'s PDA chatter: squads talk on the radio about what
// happens to them (contact, losses, raids, victories, retreats, moving in), and while they camp they chat and
// pass on what they heard about real recent events elsewhere in the Commonwealth. Ferals and animals don't talk.

#include "Wars/ALife.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

namespace Wars {

namespace {

// {thing} what they just built, {place} where it happens, {foe} the other side ("raiders"), {home} the squad's home, {heard} a recent event.
struct Voice {
    std::vector<const char*> contact, win, flee, raid, settle, losses, travel, day, night, gossip;
    std::vector<const char*> build, takeover;   // {thing}: what they built
};

const Voice kRaider{
    { "Contact! {foe} near {place}!", "We got {foe} at {place}, light 'em up!", "Hey boys, {foe} comin' through {place}. Let's have some fun." },
    { "Ha! {foe} at {place} won't be bothering anyone no more.", "Strip the bodies, boys. {place} is ours.", "That's what you get, {foe}!" },
    { "Fall back! Back to {home}!", "Screw this, I'm out! Run!", "Too many of 'em! Back to {home}!" },
    { "Heard there's {foe} at {place}. Easy pickings. Grab your guns.", "We're hitting {place} tonight. {foe} got stuff we want.", "Time to pay {place} a visit." },
    { "Nice little spot here at {place}. We're staying.", "{place} belongs to us now. Spread the word.", "Set up camp at {place}, keep your eyes open." },
    { "Someone's picking us off near {place}! Find 'em!", "We lost people at {place}! Who the hell is out there?", "Sniper! Near {place}! Get down!" },
    { "Movin' out to {place}. Keep up.", "Road to {place}. Anybody slows down gets left behind.", "{place}. Let's see what's lying around." },
    { "Anybody got some Jet? I'm going outta my skull.", "Quiet day at {place}. Too quiet.", "Who ate the last of the iguana on a stick?", "Sun's killing me. Gimme a beer.", "If I hear one more stupid story about the Glowing Sea..." },
    { "Cold night at {place}. Keep the fire going.", "Heard something out there... probably a mole rat.", "Your watch. I'm sleepin'.", "Night's when the good stuff happens." },
    { "Word is {heard}", "You hear? {heard}", "Some trader said {heard} Wasteland's going crazy." },
    { "Finally got {thing} set up at {place}.", "Who helped with {thing}? Nobody? Typical.", "Built {thing}. {place}'s starting to feel like home.", "Look at this, {thing}! We're practically civilized." },
    { "{place} is ours now. Whoever built this junk, thanks!", "Found a camp at {place}, {foe} ain't home. Finders keepers.", "{foe} left their stuff at {place}. Our stuff now." },
};

const Voice kGunner{
    { "Contact, {foe}, vicinity {place}. Engaging.", "Hostiles at {place}. Weapons free.", "Eyes on {foe}, {place}. Move in." },
    { "Area clear at {place}. Collect anything of value.", "{foe} neutralized. Good work.", "Target down. {place} secured." },
    { "We're getting chewed up! Fall back to {home}!", "Pull back, pull back! Regroup at {home}!", "Abort! Abort!" },
    { "New contract: {foe} at {place}. We move in thirty.", "Recon says {foe} are holed up at {place}. Let's earn our pay.", "Squad, form up. Target is {place}." },
    { "Establishing a position at {place}.", "{place} is a good defensive position. Dig in.", "Command, we've set up at {place}." },
    { "Taking fire at {place}! Unknown shooter!", "Man down at {place}! Find that shooter!", "We're losing people near {place}!" },
    { "Moving to {place}. Keep your spacing.", "Patrol route: {place}. Stay sharp.", "En route to {place}." },
    { "Clean your weapon, private.", "Command says hold {place}. So we hold.", "Pay's late again.", "Anyone seen the lieutenant? No? Good." },
    { "Night watch at {place}. Stay off the radio unless it's important.", "Keep the lights low. Something's out there.", "Rotate the watch every two hours." },
    { "Intel says {heard}", "Command reports {heard}", "Heads up: {heard}" },
    { "{thing} is up at {place}. Position improved.", "Engineering reports {thing} operational at {place}.", "Got {thing} in. {place} is starting to look like a real outpost." },
    { "{foe} position at {place} captured. Holding it.", "{place} taken. {foe} left their gear behind.", "Command, we've seized the camp at {place}." },
};

const Voice kMutant{
    { "HUMANS! At {place}! KILL THEM!", "Smell {foe} at {place}! SMASH!", "More meat at {place}!" },
    { "Puny {foe}. Dead now.", "{place} is quiet. Good. Eat now.", "Ha! Weak {foe}!" },
    { "Too many! Go back to {home}!", "Run! Come back later! Kill them later!", "Back! Back to {home}!" },
    { "{foe} at {place}. We go. We smash.", "Hungry. {place} has meat.", "Go to {place}. Bring big gun." },
    { "{place} is ours now. Make bag of meat.", "Good place. {place}. We stay.", "Build wall at {place}. Hang meat." },
    { "Something hurting us at {place}! FIND IT!", "Brother dead at {place}! Who did it?!", "Tiny human hiding at {place}! SMASH!" },
    { "Walk to {place}. Find things to smash.", "We go to {place} now.", "Walk. Walk. Walk to {place}." },
    { "Hungry.", "Where is Fist? Fist is stupid.", "Want to smash something.", "Humans weak. We strong." },
    { "Dark now. Good for hunting.", "Quiet. Too quiet. Want to smash.", "Sleep. Then smash." },
    { "Heard {heard}", "Brother says {heard}", "Strong ones say {heard}" },
    { "Made {thing}. Good.", "Look! {thing}! Mutants build too!", "{thing} at {place}. Now we stay." },
    { "Weak {foe} camp at {place}. Now OUR camp.", "{foe} run away. We take {place}.", "This place smell like {foe}. Mine now." },
};

const Voice kSynth{
    { "Hostiles detected near {place}. Engaging.", "Threat identified: {foe}. Location: {place}.", "Engaging targets at {place}." },
    { "Threat eliminated at {place}.", "{foe} neutralized. Resuming patrol.", "Area secure." },
    { "Unit integrity compromised. Retreating to {home}.", "Withdrawing. Requesting reinforcement.", "Retreat protocol initiated." },
    { "Objective: eliminate {foe} at {place}.", "Proceeding to {place}. Hostiles expected.", "Directive received. Target: {place}." },
    { "Establishing perimeter at {place}.", "Position acquired: {place}.", "Holding at {place}." },
    { "Unit lost at {place}. Unknown hostile.", "Taking damage. Source unidentified. {place}.", "Anomalous threat near {place}." },
    { "Proceeding to {place}.", "Relocating to {place}.", "Patrol vector: {place}." },
    { "Patrol status: nominal.", "Environmental scan complete. No anomalies.", "Awaiting directive." },
    { "Night cycle. Switching to thermal scan.", "Low light. Increasing patrol frequency.", "All quiet at {place}." },
    { "Data received: {heard}", "Field report: {heard}", "Update: {heard}" },
    { "Construction complete: {thing}. Location: {place}.", "{thing} assembled. Efficiency increased.", "Facility upgrade at {place}: {thing}." },
    { "Abandoned {foe} facility at {place} acquired.", "Position at {place} secured. Previous occupants: {foe}.", "Claiming structures at {place}." },
};

const Voice kRobot{
    { "HOSTILE DETECTED. {place}. ENGAGING.", "INTRUDER ALERT AT {place}.", "YOU ARE TRESPASSING. LETHAL FORCE AUTHORIZED." },
    { "THREAT NEUTRALIZED.", "AREA SECURE. RESUMING PATROL.", "HAVE A NICE DAY." },
    { "CRITICAL DAMAGE. RETREATING.", "SYSTEMS FAILING. RETURNING TO {home}.", "ERROR. ERROR." },
    {}, { "SECURITY PERIMETER ESTABLISHED AT {place}." },
    { "UNIT DESTROYED AT {place}. CAUSE UNKNOWN.", "UNDER ATTACK AT {place}." }, {},
    { "PATROL STATUS: NOMINAL.", "ALL SYSTEMS FUNCTIONAL.", "PLEASE REMAIN CALM." },
    { "NIGHT CYCLE ACTIVE.", "LOW LIGHT MODE ENGAGED." },
    { "BROADCAST RECEIVED: {heard}" },
};

const Voice kTrader{
    { "Bandits near {place}! Protect the brahmin!", "We got trouble at {place}! Guards!", "{foe} on the road at {place}! Circle up!" },
    { "That's handled. Let's keep moving, folks.", "Good work, guards. Drinks are on me at the next stop.", "Road's clear again." },
    { "Run! Leave the cargo if you have to!", "Back to {home}! Go, go!", "Not worth dying for! Run!" },
    {}, { "Made it to {place}. Open for business!", "{place}! Finally. Let's trade.", "Caravan's in at {place}. Come and see." },
    { "Someone's shooting at us near {place}!", "We lost a guard at {place}!", "Ambush at {place}!" },
    { "Next stop, {place}. Keep an eye on the brahmin.", "Road to {place}. Stay close.", "On to {place}." },
    { "Caps are tight this week.", "Stimpaks, ammo, purified water. Best prices in the Commonwealth!", "The brahmin's acting funny again." },
    { "Bad time to be on the road. Let's camp.", "Keep the fire small. Raiders see fires.", "Someone take first watch." },
    { "Travelers say {heard}", "Heard on the road: {heard}", "Rumor is {heard}" },
};

const Voice kBrotherhood{
    { "Contact: {foe}, {place}. Engaging.", "Hostiles sighted at {place}. Ad Victoriam!", "{foe} at {place}. Light 'em up, Knights!" },
    { "{place} is clear. The Brotherhood prevails.", "{foe} eliminated. Recover any tech.", "Area secure. Ad Victoriam." },
    { "We're outnumbered! Fall back to {home}!", "Pull back! Regroup at {home}!", "Retreat, Knights! This isn't over." },
    { "Elder's orders: purge the {foe} at {place}.", "Recon confirms {foe} at {place}. Moving to engage.", "{place} is crawling with {foe}. Not for long." },
    { "Establishing a forward position at {place}.", "{place} is under Brotherhood protection now.", "Set a perimeter at {place}." },
    { "Taking fire near {place}! Locate the shooter!", "Knight down at {place}!", "We've lost a brother at {place}!" },
    { "Patrol moving to {place}. Stay in formation.", "En route to {place}. Keep your eyes on the sky.", "Proceeding to {place}." },
    { "Scribes want every scrap of tech we find out here.", "Power armor's holding up. Barely.", "Ad Victoriam.", "Another day cleaning up the Commonwealth." },
    { "Night patrol at {place}. Helmet lights low.", "Something's moving out there. Probably a feral.", "Rotate the watch. Keep your armor on." },
    { "Field report: {heard}", "Command relays: {heard}", "Overheard on the net: {heard}" },
    { "{thing} is set up at {place}. Position fortified.", "Forward base at {place}: {thing} operational.", "{thing} in place. {place} holds." },
    { "{place} captured. The {foe} won't be back.", "{foe} position at {place} is ours now.", "Securing {place} for the Brotherhood." },
};

const Voice kMinutemen{
    { "Minutemen, {foe} near {place}! Protect the settlers!", "Contact, {place}! Form up!", "{foe} at {place}. Not on our watch." },
    { "{place} is safe again.", "That's how the Minutemen do it.", "Settlers can rest easy. {foe} are gone." },
    { "Too many of them! Back to {home}!", "Fall back, Minutemen! Regroup at {home}!", "We can't hold! Run!" },
    { "A settlement needs our help: {foe} at {place}. Move out!", "Word came in: {foe} at {place}. Minutemen, at a moment's notice!", "Let's go clear out {place}." },
    { "Setting up camp at {place}. People can count on us here.", "{place} is a Minutemen post now.", "Dig in at {place}." },
    { "We're taking casualties at {place}!", "Lost a good man near {place}!", "Sniper near {place}!" },
    { "On the road to {place}.", "Patrol heading to {place}. Keep your musket handy.", "Moving out to {place}." },
    { "Another settlement needs our help.", "Wish we had more laser muskets.", "Remember the Castle!", "Crank it twice, then fire." },
    { "Quiet night at {place}.", "Keep the fire going, folks get scared in the dark.", "Your watch, I'll take the next one." },
    { "Settlers say {heard}", "Radio Freedom reports {heard}", "Heard from a caravan: {heard}" },
    { "Got {thing} built at {place}. Coming along.", "{thing} is up. {place} is looking like a real outpost.", "Built {thing} at {place}." },
    { "{place} belongs to the people again.", "Took {place} back from the {foe}.", "{foe} cleared out of {place}. It's ours." },
};

const Voice kRailroad{
    { "Heads up, {foe} at {place}.", "Contact near {place}. Quiet as you can.", "Trouble at {place}: {foe}." },
    { "{place}'s clear. Move on.", "Done. Leave no trace.", "That's that. Keep moving." },
    { "We're blown! Scatter, meet at {home}!", "Abort! Back to {home}!", "Too hot. Fall back." },
    { "Got word of {foe} at {place}. We handle it.", "Moving on {place}. Quick and quiet.", "{place}. Let's make it quick." },
    { "Safehouse at {place}. Keep it low key.", "We'll lay low at {place} for a bit.", "{place} will do for now." },
    { "Agent down near {place}!", "We're losing people at {place}!", "Someone's onto us near {place}!" },
    { "Heading to {place}. Follow the Freedom Trail.", "Moving to {place}. Stay off the main roads.", "Next stop: {place}." },
    { "Any word from the Institute?", "Remember your cover.", "Another package to move.", "Desdemona won't like this." },
    { "Lights out at {place}.", "Night's good for us. Nobody sees much.", "Stay sharp. The Institute doesn't sleep." },
    { "Dead drop says {heard}", "Tinker Tom heard {heard}", "Word from Deacon: {heard}" },
    {}, {},
};

const Voice* VoiceOf(Faction a_f)
{
    switch (a_f) {
    case Faction::Raider: return &kRaider;
    case Faction::Gunner: return &kGunner;
    case Faction::Mutant: return &kMutant;
    case Faction::Synth: return &kSynth;
    case Faction::Robot: return &kRobot;
    case Faction::Trader: return &kTrader;
    case Faction::Brotherhood: return &kBrotherhood;
    case Faction::Minutemen: return &kMinutemen;
    case Faction::Railroad: return &kRailroad;
    default: return nullptr;   // ferals and animals don't talk
    }
}

const char* Plural(Faction a_f)
{
    switch (a_f) {
    case Faction::Raider: return "raiders";
    case Faction::Gunner: return "Gunners";
    case Faction::Mutant: return "super mutants";
    case Faction::Feral: return "ferals";
    case Faction::Mirelurk: return "mirelurks";
    case Faction::Deathclaw: return "a deathclaw";
    case Faction::Creature: return "critters";
    case Faction::Synth: return "synths";
    case Faction::Robot: return "robots";
    case Faction::Trader: return "a caravan";
    case Faction::Brotherhood: return "the Brotherhood";
    case Faction::Minutemen: return "Minutemen";
    case Faction::Railroad: return "Railroad agents";
    default: return "someone";
    }
}

const char* Speaker(Faction a_f)
{
    switch (a_f) {
    case Faction::Raider: return "Raider";
    case Faction::Gunner: return "Gunner";
    case Faction::Mutant: return "Super Mutant";
    case Faction::Synth: return "Synth";
    case Faction::Robot: return "Robot";
    case Faction::Trader: return "Caravan";
    case Faction::Brotherhood: return "Brotherhood";
    case Faction::Minutemen: return "Minuteman";
    case Faction::Railroad: return "Railroad";
    default: return "Radio";
    }
}

void Replace(std::string& a_s, const char* a_key, const std::string& a_with)
{
    for (size_t at = a_s.find(a_key); at != std::string::npos; at = a_s.find(a_key, at + a_with.size())) a_s.replace(at, std::strlen(a_key), a_with);
}

}  // namespace

void ALife::Say(const Squad& a_s, Line a_what, int a_place, Faction a_foe, int a_priority, const char* a_thing)
{
    const Voice* v = VoiceOf(a_s.faction);
    if (!v) return;
    const std::vector<const char*>* pool = nullptr;
    switch (a_what) {
    case Line::Contact: pool = &v->contact; break;
    case Line::Win: pool = &v->win; break;
    case Line::Flee: pool = &v->flee; break;
    case Line::Raid: pool = &v->raid; break;
    case Line::Settle: pool = &v->settle; break;
    case Line::Losses: pool = &v->losses; break;
    case Line::Travel: pool = &v->travel; break;
    case Line::Idle: pool = Night() ? &v->night : &v->day; break;
    case Line::Gossip: pool = &v->gossip; break;
    case Line::Build: pool = &v->build; break;
    case Line::Takeover: pool = &v->takeover; break;
    }
    if (!pool || pool->empty()) return;
    if (a_what == Line::Gossip && m_news.empty()) return;
    std::string text = (*pool)[std::uniform_int_distribution<size_t>(0, pool->size() - 1)(m_rng)];
    const int place = a_place >= 0 ? a_place : Nearest(a_s.x, a_s.y);
    Replace(text, "{place}", m_nodes[place].name);
    Replace(text, "{home}", m_nodes[a_s.home].name);
    std::string foe = Plural(a_foe);
    if (!foe.empty() && std::islower(static_cast<unsigned char>(foe[0])) && text.find("{foe}") == 0) foe[0] = static_cast<char>(std::toupper(foe[0]));
    Replace(text, "{foe}", foe);
    Replace(text, "{thing}", a_thing ? a_thing : "something");
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));   // "{thing} is up" -> "A cot is up"
    if (text.find("{heard}") != std::string::npos)
        Replace(text, "{heard}", m_news[std::uniform_int_distribution<size_t>(0, m_news.size() - 1)(m_rng)]);
    Chatter c;
    c.faction = a_s.faction;
    c.who = fmt::format("{}, {}", Speaker(a_s.faction), m_nodes[a_s.home].name);
    c.text = std::move(text);
    c.x = a_s.x;
    c.y = a_s.y;
    c.priority = a_priority;
    c.hours = m_hours;
    m_chatter.push_back(std::move(c));
    if (m_chatter.size() > 64) m_chatter.erase(m_chatter.begin());
}

// Something worth retelling: what happened, where (gossip).
void ALife::News(Faction a_winner, Faction a_loser, int a_place, bool a_wiped)
{
    std::string line = fmt::format("{} {} {} near {}.", Plural(a_winner), a_wiped ? "wiped out" : "ran off", Plural(a_loser), m_nodes[a_place].name);
    line[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(line[0])));
    m_news.push_back(std::move(line));
    if (m_news.size() > 12) m_news.erase(m_news.begin());
}

// Now and then a camping squad talks (or passes on news).
void ALife::Ambient()
{
    if (m_squads.empty()) return;
    for (int tries = 0; tries < 8; ++tries) {
        const Squad& s = m_squads[std::uniform_int_distribution<size_t>(0, m_squads.size() - 1)(m_rng)];
        if (!VoiceOf(s.faction) || s.task != Task::Camp) continue;
        Say(s, std::uniform_int_distribution<int>(0, 2)(m_rng) == 0 ? Line::Gossip : Line::Idle, s.at, Faction::Raider, 0);
        return;
    }
}

std::vector<ALife::Chatter> ALife::TakeChatter()
{
    std::vector<Chatter> out;
    out.swap(m_chatter);
    return out;
}

}  // namespace Wars
