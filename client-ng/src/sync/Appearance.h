// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Player appearance for remote bodies. Each game captures its own player character (made in its
// own single-player save) and broadcasts it; receivers build a per-player NPC base record from a
// copy of the player template (base 0x7: its head is generated live from appearance data, unlike
// NPCs from the master files that use pre-baked faces) and spawn that player's body from it.
// A copy of the player template also inherits the player's factions, so NPCs react to remote
// bodies as they would to a player. All functions run on the main thread.
namespace F4MP::Appearance
{
    // Serialized appearance of the local player, ready to send as CoopAppearance (first 4 bytes =
    // senderId, stamped by the server).
    std::vector<uint8_t> CaptureLocal();

    // Store a received appearance. Returns true if it differs from what we had for that player.
    bool OnReceived(uint32_t playerId, const uint8_t* data, size_t size);

    // FormID of the body base for this player (created on first use), or 0 if we have no
    // appearance for them yet or creating it failed.
    uint32_t GetBodyBase(uint32_t playerId);

    // True once per change: the player's appearance changed after their body was spawned.
    bool TakeRespawnRequest(uint32_t playerId);
    void RequestRespawn(uint32_t playerId);      // main.cpp replaces that player's body

    // Co-op characters: looks saved from the player's own saves (or made in the character editor),
    // kept in Documents\My Games\Fallout4\F4SE\F4MP_characters. The selected one is what other
    // players see, whatever character the shared save puts this player in.
    struct Character
    {
        std::string file;    // file name in the characters folder (the id)
        std::string name;    // character name
        bool female = false;
        bool selected = false;
    };
    std::vector<Character> ListCharacters();
    // Capture the loaded character (under a_name if given); returns its file, "" on failure.
    std::string SaveCurrentAsCharacter(const std::string& a_name = {});
    bool SelectCharacter(const std::string& a_file);    // "" = play as whatever character is loaded
    bool DeleteCharacter(const std::string& a_file);

    bool SaveMyLook();                         // save the loaded character and select it
    std::vector<uint8_t> LookToSend();         // selected character if any, else the loaded one
    std::string MyLookName();
    std::string NameOf(uint32_t playerId);     // remote player's character name, "" if unknown                  // selected character's name, or "" if none

    void Forget(uint32_t playerId);
    void Reset();
}
