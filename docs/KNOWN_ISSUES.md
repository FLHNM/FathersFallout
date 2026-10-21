# Known issues & what's being worked on

Update 23 (alpha). Already known - no need to report these unless you have new details.

## Known issues

| Area | What you'll see |
| --- | --- |
| Steam join | Works between two PCs in our tests. If it fails for you, use **BY IP** and please [report it](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml). |
| New characters | Co-op starts only after your character has left Vault 111. Saves from the pre-war intro won't connect: leave the vault, save, and load that save. |
| Faces | Other players see a close match of your face picked from the game's people (same hair and beard), not your exact face. |
| Power armor | When a player gets out, the empty frame they leave isn't shown on the other screen. Entering power armor has been tested with a simulated second player, not yet across two PCs. |
| Aiming up/down | Shown by the other player bending at the waist - shots go where they aim, but it doesn't use the game's full aim animation, so it can look stiff. |
| First shot | The first shot right after drawing a weapon can show about half a second late on the other screen. |
| Sliding | Since Update 21 a body whose walk stops playing restarts it by itself, usually within a second. A short slide can still happen now and then. |
| Companions | Shared since Update 23: the other players see a copy of your companion. If the other player's world has that same NPC standing nearby (their Piper in Diamond City while your Piper follows you), you only see your own Piper there. |
| Far-away NPCs | Guests with a lower view distance (`uGridsToLoad`) see fewer of the host's NPCs far away. Close by, everyone sees the same people. |
| Quest catch-up | A guest's quests are moved up to the host's progress, but one-time things from steps the guest skipped (items handed out, NPCs moved) don't happen in the guest's game. Gates and doors those steps opened do open for everyone since Update 22. |
| Random encounters | The small radiant quests that pop up on the road belong to the host's game; guests see the creatures and people, but not those quests in their log. |
| Mods | Different plugin lists work since Update 20, but things from a mod only one player has don't show for the others (a message on connect names those mods). Overhaul mods can still make the games disagree. |
| GOG | Not tested yet. IP join only. |
| Players | Tested with 2 players. |
| Host leaving | Ends the session for everyone (guests keep playing in their own world). |

## Being worked on / planned

- Exact faces for other players (not just a close match).
- Smoother aiming animation (full aim pose instead of bending at the waist).
- Power wires and settler jobs in shared settlements.
- More than 2 players tested.
- GOG tested.

Have an idea? [Suggest it](https://github.com/FLHNM/FathersFallout/issues/new?template=feature_request.yml).
