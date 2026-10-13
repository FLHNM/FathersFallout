// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <steam/steamnetworkingsockets.h>

#include <cstdint>
#include <string>

// Join through Steam instead of an IP address. Uses the Steam client the game itself already runs
// (its steam_api64.dll): peer-to-peer connections by Steam account over Valve's relays - no IPs,
// no port forwarding, no firewall prompt. Not there on GOG copies: those keep the IP join.
//   Host: Steam friends' connections are relayed into the normal co-op server on this PC, and the
//         host is shown to friends (rich presence) so their game can find it.
//   Guest: the co-op connection itself goes to the host's Steam account.
namespace Network::SteamLink
{
    bool Init();                          // once; false = no Steam (GOG) - IP join only
    bool Available();
    uint64_t MySteamId();
    std::string MyName();

    // ---- host ----
    // Relays Steam friends into the server at 127.0.0.1:a_serverPort (through a_gns, the plugin's own
    // GameNetworkingSockets). Safe to call again (no-op while hosting).
    void StartHosting(ISteamNetworkingSockets* a_gns, uint16_t a_serverPort, const std::string& a_hostName);
    void StopHosting();
    bool IsHosting();
    int  RelayedPlayers();
    // GameNetworkingSockets status callback for a relay connection: true = handled here.
    bool OnGnsStatus(SteamNetConnectionStatusChangedCallback_t* a_info);

    // ---- guest ----
    // a_target: "" / "auto" = the Steam friend hosting FathersFallout right now; otherwise a friend
    // code (Steam > Add a Friend), a SteamID64 or (part of) a friend's Steam name.
    uint64_t FindHost(const std::string& a_target, std::string& a_outName, std::string& a_outError);
    HSteamNetConnection Connect(uint64_t a_hostSteamId);
    bool Send(HSteamNetConnection a_conn, const void* a_data, uint32_t a_size);
    int  Receive(HSteamNetConnection a_conn, SteamNetworkingMessage_t** a_out, int a_max);
    // 0 none, 1 connecting, 2 finding route, 3 connected, 4 closed by peer, 5 problem (k_ESteamNetworkingConnectionState_*)
    int  State(HSteamNetConnection a_conn, std::string& a_endDebug);
    void Close(HSteamNetConnection a_conn);
    void RunCallbacks();

    // [Debug] SteamSelfTest=1: connect to our own hosting through Steam and log the server's answer.
    void SelfTest();
}
