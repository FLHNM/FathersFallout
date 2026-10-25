# FAQ

## Is it safe?

- **All the code is public** in this repository: the game plugin (`client-ng/`), the co-op server
  (`server/`) and the launcher (`tools/launcher/Launcher.cs`). Anyone can read it or build it.
- **What the launcher does:** checks your Fallout 4 install, copies the co-op plugin
  (`F4MPClient.dll` + `FathersFallout.ini`) into `Data\F4SE\Plugins`, writes its own settings file next to
  itself, and starts the game through F4SE. When hosting it copies the co-op server to
  `%LOCALAPPDATA%\FathersFallout\server` (so Windows Firewall only asks once) and runs it from there.
- **Internet:** the game connects to the host (directly by IP, or through Steam). The launcher only
  goes online when you press **[ MY IP FOR FRIENDS ]** (it asks `api.ipify.org` for your internet
  IP) or **[ SHARE LOG ]** (opens the GitHub bug report page in your browser).
- **It never** asks for your Steam password, sends your files anywhere, or changes anything outside
  the Fallout 4 folder, your Fallout 4 saves/settings and the folders above.
- **Check your download:** each [release](https://github.com/FLHNM/FathersFallout/releases) lists SHA-256 checksums. In PowerShell:
  `Get-FileHash .\FathersFallout-Update17-Alpha.zip` - the result must match.

## Windows says "Windows protected your PC"

That's **SmartScreen**. It warns about any program that isn't signed with a paid code-signing
certificate and that few people have run yet - it doesn't mean anything was found in it.
Click **More info → Run anyway**. As more people use the launcher the warning goes away on its own.

## My antivirus blocked or deleted a file

Small unsigned tools that start games and copy plugins are sometimes flagged by mistake
("false positive"). Please **don't turn your antivirus off** - instead:
1. compare the zip's SHA-256 with the one on the release page,
2. [open an issue](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml) with the antivirus name and the
   exact detection name, so it can be reported to the antivirus maker as a false positive.

## Will it break my save?

As a **host**: your save works like normal. As a **guest**: while connected, your quests are moved
up to the host's progress and NPCs go where the host has them - **that is saved into your save**.
Use a copy of your character for co-op. Either way, back up `Documents\My Games\Fallout4\Saves`.

## Can I use other mods?

Since Update 20, yes, with limits. Every game sends its plugin list (`.esp/.esm/.esl`) when it
connects and translates the other players' items, NPCs and quests into its own load order, so mods
everyone has work even in a different order. Things from a mod only one player has don't show for
the others, and a message on connect names the mods that differ. Mods that change the base game
(overhauls, leveled lists) can still make the games disagree, so the cleanest co-op is everyone on
the same mods, or base game + DLC only. Texture or mesh mods without plugins are fine.

## Steam join can't find my friend

- The host must be **in game** (hosting starts when their save has loaded).
- You must be **Steam friends**: since Update 23 hosts only let in their Steam friends unless they
  turn **FRIENDS ONLY** off. Typing the host's **friend code** helps when Steam doesn't find them.
- *"Wrong password"*: ask the host for their game password and type it in **HOST'S PASSWORD**.
- *"Different FathersFallout version"*: everyone needs the same update.
- Both copies of Fallout 4 must be the Steam version with Steam running.
- Still nothing? Use **BY IP** as a backup and [report it](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml).

## How do I report a bug?

1. **Right after playing**, open the launcher and press **[ SHARE LOG ]**. Since Update 24 it also
   sends the session before, so after a crash you can restart the game and still send the crashed session.
2. It saves `FathersFallout-log-<date>.txt` to your Desktop with **personal details removed**
   (IP addresses, Steam IDs, friend codes, your Windows user name) and opens the
   [bug report form](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml).
3. Describe what happened, **drag the log file into the form**, and if you can, attach the other
   player's log too - most bugs need both sides.

Bug reports on GitHub are **public**, which is why the launcher cleans the log first. Have a quick
look at the file before posting if you want to be sure. (A free GitHub account is needed to post.)

## How many players?

Tested with 2 so far. More should work but hasn't been tested yet.

## Where's the log?

`Documents\My Games\Fallout4\F4SE\FathersFallout.log` - but use **[ SHARE LOG ]** for bug reports,
it removes your personal details.
