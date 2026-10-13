# Known issues & what's being worked on

Update 17 (alpha). Already known - no need to report these unless you have new details.

## Known issues

| Area | What you'll see |
| --- | --- |
| Steam join | Joining through Steam between two different PCs is still being tested in the wild - if it fails, use **BY IP** and please [report it](https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml). |
| Faces | Other players see a close match of your face picked from the game's people (same hair and beard), not your exact face. |
| Sitting | A player who sits shows as standing still at the chair. |
| Aiming up/down | Shown by the other player bending at the waist - shots go where they aim, but it doesn't use the game's full aim animation, so it can look stiff. |
| First shot | The first shot right after drawing a weapon can show about half a second late on the other screen. |
| Sliding | Much less since Update 17. A player's body may still slide a short distance now and then, especially right after crouching. |
| Modded items in containers | A modded weapon put into a container shows up without its mods for the other player. To trade modded gear, drop it on the ground - drops keep their mods. |
| Companions | Your companion stays with you when you join, but the host doesn't see them yet. |
| Far-away NPCs | Guests with a lower view distance (`uGridsToLoad`) see fewer of the host's NPCs far away. Close by, everyone sees the same people. |
| Random encounters | The small radiant quests that pop up on the road belong to the host's game; guests see the creatures and people, but not those quests in their log. |
| Mods | Different plugin lists = items, quests and NPCs stop syncing (red warning in the F1 menu). |
| GOG | Not tested yet. IP join only. |
| Players | Tested with 2 players. |
| Host leaving | Ends the session for everyone (guests keep playing in their own world). |

## Being worked on / planned

- Exact faces for other players (not just a close match).
- A real sitting animation.
- Smoother aiming animation (full aim pose instead of bending at the waist).
- Power wires and settler jobs in shared settlements.
- Modded items in containers, companions shared with the host, NPC gear matching on both screens.
- More than 2 players tested.
- GOG tested.

Have an idea? [Suggest it](https://github.com/FLHNM/FathersFallout/issues/new?template=feature_request.yml).
