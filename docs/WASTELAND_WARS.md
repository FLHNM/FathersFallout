# Wasteland Wars (experimental) - design

A separate game mode next to campaign co-op. Rust, in Fallout 4's Commonwealth. It must never touch the
campaign: own launcher mode, own saves (Wars mark in the co-save), own server exe/port, own protocol range.

## The loop

1. **Fresh world.** Everyone starts weak and alone. No main or side-quest characters; raiders, gunners,
   super mutants, creatures and random encounters stay, and every faction is hostile to every player.
2. **Take outposts from the wasteland.** Kill everything at a clearable location, then hold its flag spot
   for about 30 s. A workbench appears; the capturing team can build there and the area is claimed
   (shown on everyone's map in the team colour).
3. **Fight each other for them.** Another team takes an outpost the same way: hold the flag with no
   defender nearby. Server owners can set raid hours / offline protection.
4. **Then it's the players' world.** Trade routes, shops, alliances, wars, bounties: player-made.

## Players

- **Joining:** make a character at the mirror (the real new game, intro skipped), then spawn **alone** at
  a random spawn point on the edge of the map, never next to another player's base. Starter kit is small.
- **Teams:** formed in game (F1 menu: invite / join / leave). A name and one of four colours: red, blue,
  white, orange. Teammates see each other's name tags and map markers.
- **Death:** **you keep your level and perks, you lose your gear**: it drops in a loot bag where you died.
  Respawn at a random spawn point, or at a bed / sleeping bag your team built (Rust-style home spawn).

## Persistence: the server owns the war

The server, not anyone's save, holds the war: characters (level, perks, last position, inventory),
teams, claims, built structures, loot bags, outpost state (cleared, owner, flag), NPC state (alive/dead,
health, position, looted) and respawn timers. It writes them to disk and survives restarts. A player's
local save only caches their own game; on joining, the server's state is applied.

**Player-hosted and dedicated are the same server program**, run on a friend's PC or on a rented box
(the Linux build / Dockerfile already exist).

### A-Life: the world lives on the server (server/src/Wars/ALife)

In the spirit of S.T.A.L.K.E.R.'s A-Life (and Project Zomboid's world that goes on without you), the
Commonwealth's NPCs are simulated **on the server**, all the time, the same for everyone:

- **The world graph** is the game's own 310 Commonwealth map markers, built from the owner's
  `Fallout4.esm` by `tools/wars/make_world.py` (-> `wars_world.json` next to the Wars server). For every
  place it counts the NPCs the game itself put there by faction: that place is that faction's home
  (Corvega = raiders, Quincy = Gunners, Lexington = ferals, Concord = raiders + its deathclaw...).
- **Squads** (raider gangs, Gunner squads, super mutant war parties, feral hordes, mirelurk nests,
  creature packs, synth patrols, robot posts, caravans, deathclaws) have a home, members with health and
  a routine: camp, patrol within reach, raid a weaker hostile camp nearby, return home; ferals come out at
  night; caravans run between settlements. Hostile squads that meet on the move fight; a squad that loses
  40% of its people breaks and runs home; wiped-out homes are refilled every 36 game hours.
- **Offline-tested** (`FathersFalloutServer.exe --alife-test 168`): 351 squads in 230 homes settle at
  ~245 and every faction survives a week. The chronicle reads like a living world ("A super mutant war
  party heads out from South Boston High School to attack a raider gang at Jamaica Plain").
- **Saved** to `wars_alife.json` every minute and on shutdown; a restart carries on where it stopped.
  Deleting that file = wiping the NPC world (the server owner's choice).
- Server console: `alife`, `alife near <place>`, `alife events [n]`, `alife speed <x>`, `alife save`.

**Online (next):** when a squad comes within loading range of a player, one game there spawns its real NPCs
(faction's leveled lists, the squad's health) and walks them to the squad's goal with the game's own AI;
everyone else sees synced copies; when the players leave, the squad's state returns to the server and it
carries on offline. The game's own placed NPCs and random encounters are replaced by server squads, so all
players see the same world.

### NPCs without a host player

Fallout 4's AI only runs inside the game, so no server can run raiders by itself. So the server keeps the
*state* and players lend their game to *simulate*:

- Each area (cell / group of exterior cells) has one **simulating player**: someone in it. Their game
  runs the NPCs there and reports their state to the server (~10/s). Others see copies.
- When that player leaves or drops, the server hands the area to another player in it within a second,
  starting from the last reported state. Nobody is ever "the host".
- An area with nobody in it is **paused**; the next player to arrive loads it exactly as it was left
  (dead stay dead, cleared stays cleared, respawns on the server's timers).

## Server settings (config.json)

name, password, max players, team size, PvP on/off, raid hours / offline protection, **wipe schedule (up
to whoever runs the server: never, or every N days; bases and/or map)**, starter kit, spawn points.

## Build order

1. New-war start fixed (intro skip leaves a normal character) - done, verified 2026-10-26.
2. Server-saved characters, spawn points, death -> loot bag + respawn.
3. Teams, colours, name tags, map markers.
4. Outpost capture (clear + hold flag), workbench, claims saved on the server.
5. A-Life: offline world simulation on the server - done 2026-10-26 (squads, routines, fights,
   respawns, saved state). Next: squads "online" in players' games, hand-off between players.
6. Raids / flips, beds, trading.
7. Dedicated server packaging (Windows + Linux/Docker), server config docs.
