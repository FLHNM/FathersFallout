# Known issues & what's being worked on

Update 26 Experimental (alpha). Already known - no need to report these unless you have new details.

## Known issues

| Area | What you'll see |
| --- | --- |
| Steam join | Works between two PCs in our tests. If it fails for you, use **BY IP** and please [report it](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml). |
| New characters | Co-op starts only after your character has left Vault 111. Saves from the pre-war intro won't connect: leave the vault, save, and load that save. |
| Faces | Other players see a close match of your face picked from the game's people (same hair and beard), not your exact face. |
| Power armor | Entering power armor has been tested with a simulated second player, not yet across two PCs. |
| Aiming up/down | Shown by the other player bending at the waist - shots go where they aim, but it doesn't use the game's full aim animation, so it can look stiff. |
| Time of day | If the other player's sky or time looks different from the host's, the joining player can press **Sync time with host** in the F1 menu. |
| Sliding | Since Update 21 a body whose walk stops playing restarts it by itself, usually within a second. A short slide can still happen now and then. |
| Companions | Shared since Update 23: the other players see a copy of your companion. If the other player's world has that same NPC standing nearby (their Piper in Diamond City while your Piper follows you), you only see your own Piper there. |
| Far-away NPCs | Guests with a lower view distance (`uGridsToLoad`) see fewer of the host's NPCs far away. Close by, everyone sees the same people. |
| Quest catch-up | When a guest joins, their quests are moved up to the host's current stage of each quest, but one-time things from steps the guest skipped (items handed out, NPCs moved) don't happen in the guest's game. Gates and doors those steps opened do open for everyone since Update 22. Once you play together, every stage reaches everyone in order (Update 25). |
| New in Update 25 | Voice chat, power wires, settler jobs, guests' quest progress and quest buttons pressed by a guest have been tested with a simulated second player, not yet across two PCs. |
| New in Update 26 | Join Game from the Steam friends list and NPC dialogue for the other players are new: please report anything odd. A game Steam starts by itself has no F4SE, so start the game from the launcher first. |
| Wasteland Wars | Experimental. Survival mode, random starts and the latest squad changes have only been tested on one PC so far. Diamond City's gate stays shut and the town is empty. Your death bag is only in your own game, and what you build may not show for other players yet. Squads spread out when they spawn and set off, but still camp close together. The DLC islands aren't in it. |
| Voice chat | Uses the Windows default microphone. If nobody hears you: F1 → Voice chat → **hear myself** and the mic meter, then your default recording device in Windows Sound settings. |
| Random encounters | The small radiant quests that pop up on the road belong to the host's game; guests see the creatures and people, but not those quests in their log. |
| Mods | Different plugin lists work since Update 20, but things from a mod only one player has don't show for the others (a message on connect names those mods). Overhaul mods can still make the games disagree. |
| GOG | Not tested yet. IP join only. |
| Players | Tested with 2 players. |
| Quitting | Closing the game can write a "GAME CRASHED" entry at the end of the log even when nothing went wrong during play. |
| Host leaving | Ends the session for everyone (guests keep playing in their own world). |

## Being worked on / planned

- Exact faces for other players (not just a close match).
- Smoother aiming animation (full aim pose instead of bending at the waist).
- Wasteland Wars: Diamond City as a town, wider-roaming squads, shared death bags.
- More than 2 players tested.
- GOG tested.

Have an idea? [Suggest it](https://github.com/FLHNM/FathersFallout/issues/new?template=feature_request.yml).
