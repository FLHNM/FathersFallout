// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace F4SE { class SerializationInterface; }

// Wasteland Wars (EXPERIMENTAL): a separate game mode. The Commonwealth without its main- and side-quest
// characters, where players fight the factions and each other for outposts and settlements.
//
// It never touches the campaign:
// - It is on only when the launcher started the game in Wars mode ([Mode] Game=wars, read once and cleared,
//   so the next launch of any kind is the campaign again).
// - It changes a world only when that save is a Wars world (its co-save carries the Wars mark, written when
//   a new war starts). A campaign save loaded in Wars mode is left exactly as it is, and doesn't connect.
// - Its connection settings are [Wars] in FathersFallout.ini, never [Connect]; its server is a separate
//   FathersFalloutWarsServer.exe on its own port, and its protocol number differs, so a campaign game and a
//   Wars game can't join each other.
namespace F4MP::Wars
{
    void Init(const std::string& a_iniPath);   // plugin load: reads (and clears) the launcher's one-shot keys
    bool Mode();                               // started in Wars mode
    bool WorldIsWars();                        // the loaded save is a Wars world
    bool Live();                               // both: the only time Wars changes anything in the world
    uint32_t ProtocolOffset();                 // added to the protocol number in Wars mode
    const char* ConnectSection();              // "Wars" in Wars mode, else "Connect"

    // [Launch] NewWar=1: a fresh world instead of a save. Runs at the main menu; a_ready runs once the
    // player stands in it (connect then).
    bool TakeNewWar();
    void GiveKit();   // a fresh character's kit (new war, after dying)
    uint32_t RandomStart();   // a map marker somewhere quiet: where a new war starts, where you get up without a claim
    void StartNewWar(std::function<void()> a_ready);

    // After a save loaded. False = don't connect (a campaign save in Wars mode, or a Wars save in campaign mode).
    bool OnGameLoaded();

    // Co-save (the plugin's one F4SE record stream, shared with Workshop).
    constexpr uint32_t kRecord = 'WARS';
    void OnSave(const F4SE::SerializationInterface* a_intfc);
    void OnLoadRecord(const F4SE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);
    void OnRevert();

    // testing/test helper: console bridge "f4mp:wars <what>" (diag, unlock, workbench).
    void Bridge(const std::string& a_args);
}
