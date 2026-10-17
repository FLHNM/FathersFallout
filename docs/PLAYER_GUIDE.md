# FathersFallout - Player Guide

Everything you need to install, host and join. The same guide ships in the download as
`README - START HERE.txt`.

> **Alpha.** Back up your saves first: copy `Documents\My Games\Fallout4\Saves` somewhere safe.

## 1. What you need

- **Fallout 4 version 1.11.240** (the current Steam version) with **all 6 DLC**.
  - **Steam:** everything works, including joining friends through Steam (no IP needed).
  - **GOG:** should work if your game is the same version, but GOG hasn't been tested yet in this
    alpha. GOG players host and join **by IP**.
  - **Game Pass / Microsoft Store:** not supported (no script extender).
- Three free tools from their own authors - the launcher checks for them and links to each:
  [F4SE](https://f4se.silverlock.org/),
  [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327),
  [F4SE Menu Framework](https://www.nexusmods.com/fallout4/mods/105090).
- **Best: the same plugins for everyone** - the base game + DLC (+ the same Creation Club items).
  Different mods no longer break sync (Update 20), but things from a mod only one player has won't
  show for the others. See `LOAD ORDER.txt` in the download.
- **Everyone on the same update.** Older versions are refused when they connect.

## 2. Install (about 2 minutes)

1. Download the zip from [Releases](https://github.com/FLHNM/FathersFallout/releases) and unzip it anywhere (Desktop is fine).
2. Open **`FathersFallout.exe`**. If Windows says *"Windows protected your PC"*, click
   **More info → Run anyway** (see the [FAQ](FAQ.md#windows-says-windows-protected-your-pc) for why).
3. **SYSTEM CHECK** shows what's installed. Anything marked `[!!]` has a download link:
   - **F4SE:** `f4se_loader.exe` + `f4se_1_11_240.dll` next to `Fallout4.exe`, and its
     `Data\Scripts` folder into your Fallout 4 `Data` folder.
   - **Address Library:** the `.bin` file into `Data\F4SE\Plugins`.
   - **Menu Framework:** its `F4SE`, `Interface` and `Scripts` folders into `Data`.
   - Game not found? Press **[ GAME FOLDER ]** and pick the folder with `Fallout4.exe`.
4. Press **[ INSTALL / UPDATE FATHERSFALLOUT ]** (PLAY also does this every time).
   *Access denied?* Right-click `FathersFallout.exe` → **Run as administrator**, once.
5. You need a save of your own. No save yet? Start Fallout 4 normally, choose New, walk out of
   Vault 111, save, quit, and come back to the launcher.

## 3. Hosting (the player whose world everyone joins)

1. Choose **[ HOST ]**, type your name, pick your save, press **PLAY**.
2. The game starts, loads your save and starts the co-op server by itself.
   If Windows Firewall asks about `F4MPServer`, click **Allow** (only needed for IP joins).
3. **Steam friends can join you now.** Your Steam **friend code** is shown under HOST - give it
   to friends if Steam can't find you automatically.
4. **Friends without Steam (GOG)** join by IP: press **[ MY IP FOR FRIENDS ]** (copies your
   internet IP) and forward **UDP port 7779** on your router to your PC. Players in the same house
   use the "SAME HOUSE" address instead - no forwarding needed.

Only **one** player hosts. Start the host's game first.

## 4. Joining a friend

### Through Steam (recommended)
1. **[ JOIN A FRIEND ]** → **[ THROUGH STEAM ]**.
2. **Friend code or name:** leave it **empty** to join whichever Steam friend is hosting, or type
   the host's friend code (shown in their launcher, or in Steam under *Friends → Add a Friend*)
   or part of their Steam name.
3. Your name, **your** save, **PLAY**.

No IPs, no port forwarding. If it says *"no Steam friend is hosting"*: make sure the host is
already in game, that you're Steam friends, or type the host's friend code. It keeps looking for
about a minute.

### By IP (GOG, or if Steam join doesn't work)
1. **[ JOIN A FRIEND ]** → **[ BY IP (GOG) ]**.
2. The host's IP (from their **[ MY IP FOR FRIENDS ]**) and port `7779`.
3. Your name, your save, **PLAY**. The host must have UDP 7779 forwarded.

## 5. In game

- **Co-op menu:** **F1 → FathersFallout → Menu** - connect/disconnect, host or join, pick which
  character others see, and **Go to host**.
- When you join, you're moved next to the host (or told how far away they are).
- **Guests:** while connected, the **host's world and story are the story**. Your quests are moved
  up to the host's progress and NPCs go where the host has them. **This changes your save** - use
  a separate save (a copy of your character) for co-op if you want your own playthrough untouched.
- **Shared:** movement and animations, weapons and armor (with mods), power armor, furniture (sitting,
  lying, cooking - players and NPCs), NPC idles, shooting and melee, damage
  (including to each other), enemies and NPCs, deaths, explosions, doors and switches, items you
  drop for each other, containers, building / moving / scrapping in workshops, quest progress,
  time of day and weather.

## 6. Something went wrong?

See [Known issues](KNOWN_ISSUES.md) and the [FAQ](FAQ.md). Found a new bug? Press
**[ SHARE LOG ]** in the launcher right after playing and attach the file to a
[bug report](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml) - details in the
[FAQ](FAQ.md#how-do-i-report-a-bug).

## 7. Uninstall

Delete `F4MPClient.dll` and `F4MP.ini` from `Fallout 4\Data\F4SE\Plugins`. Your saves stay as they are.
