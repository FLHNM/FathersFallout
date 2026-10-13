# FathersFallout

**Online co-op for Fallout 4** - by FatherLamb (YouTube: FatherLamb)

Play Fallout 4 together: each player uses their own character and save, and everyone shares the
host's world - NPCs, enemies, quests, settlements, loot, doors and builds.

> **Alpha.** Expect bugs and back up your saves (`Documents\My Games\Fallout4\Saves`) before playing.

## Download

Get the latest build from the **[Releases](https://github.com/FLHNM/FathersFallout/releases)** page. Unzip it, run
`FathersFallout.exe` and follow `README - START HERE.txt` - it covers installing, hosting,
joining through Steam (no IP needed) or by IP (GOG), known issues and how to report bugs.

**Requirements:** Fallout 4 1.11.240 with all 6 DLC, plus three free tools from their own authors:
[F4SE](https://f4se.silverlock.org/),
[Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327) and
[F4SE Menu Framework](https://www.nexusmods.com/fallout4/mods/105090)
(the launcher checks for them and links to each).

## Guides

- **[Player guide](docs/PLAYER_GUIDE.md)** - install, host, join through Steam or by IP, in-game menu
- **[Known issues & what's being worked on](docs/KNOWN_ISSUES.md)**
- **[FAQ](docs/FAQ.md)** - is it safe, Windows warnings, saves, mods, Steam join problems

## Is it safe?

The launcher isn't signed with a paid certificate, so Windows SmartScreen may say
*"Windows protected your PC"* - click **More info → Run anyway**. All the code is right here to
read, each release lists SHA-256 checksums so you can check your download, and the
[FAQ](docs/FAQ.md#is-it-safe) lists exactly what the launcher touches.

## Found a bug?

Right after playing, press **[ SHARE LOG ]** in the launcher: it saves your co-op log to the Desktop
with personal details (IP addresses, Steam IDs, your Windows user name) removed and opens the
[bug report form](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml) -
drag the file in. Logs from both players help the most.

## What's shared

Movement and animations, weapons and armor (with their mods), shooting and melee - aimed where
each player really aims - damage (including between players), enemies and NPCs, deaths,
explosions, doors and switches, dropped items, containers, building / moving / scrapping in
workshops, quest progress, time of day and weather.

## Building from source

The source is here as the GPL requires. You don't need it to play - use the Releases.

| Part | Folder | Build |
| --- | --- | --- |
| Game plugin (F4SE DLL) | `client-ng/` | [xmake](https://xmake.io): `cd client-ng && xmake` (uses [CommonLibF4](https://github.com/libxse/commonlibf4), a git submodule: clone with `--recursive`) |
| Co-op server | `server/` | `setup_deps.bat` once (builds GameNetworkingSockets, spdlog... with vcpkg), then `build_server.bat` |
| Launcher | `tools/launcher/` | `build.bat` (the .NET Framework compiler that ships with Windows) |

Requires Visual Studio 2022 with the C++ desktop workload.

## License

FathersFallout - Copyright (C) 2026 FatherLamb.

The code is free software under the **GNU GPL v3** with additional terms (GPL section 7) - see
[LICENSE](LICENSE) and [NOTICE](NOTICE). In short: keep the credits, and a modified version must be
clearly marked as modified and must not use the names "FathersFallout" or "FatherLamb".

The logo, icon and other artwork in `tools/launcher/art/` are **not** covered by the GPL and may
not be reused without permission (see [tools/launcher/art/README.txt](tools/launcher/art/README.txt)).

## Credits

- F4MP - Copyright (C) 2020 Alin Octavian, Benjamin Kyd (GPL v3): the original server and design.
- [Jous99's F4MP rework](https://github.com/Jous99/F4MP) (public domain).
- [CommonLibF4](https://github.com/libxse/commonlibf4) (GPL v3), Valve's GameNetworkingSockets
  (BSD 3-Clause), Protocol Buffers, Abseil, OpenSSL, spdlog, fmt, nlohmann/json, Dear ImGui (via F4SE Menu Framework).
- Steam joining uses Valve's Steam networking through the game's own Steam client.

Fallout 4 is a trademark of Bethesda Softworks LLC. FathersFallout is a free fan-made mod, not
affiliated with or endorsed by Bethesda Softworks or ZeniMax Media.
