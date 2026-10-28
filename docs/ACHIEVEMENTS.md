# Achievements

Savage Wind has a small achievement system that never touches the game: it reads the saves the game writes.

- **Detection** (`runtime/achievements_watch.c`): a thread looks every 2 s at every `game_data.sav` under the save folder.
  A file is read once its size and time stay the same for two looks (the game writes a save in pieces). Every slot
  counts, and what is earned in any of them is earned. Saves already on disk when the app starts are checked too.
- **Rules** (`runtime/achievements_defs.c`): one line per achievement: a flag reaching a value (`ACH_FLAG`), a number of
  flags out of a numbered family such as `Clear_Dungeon%03d` (`ACH_COUNT`), every flag in a list (`ACH_ALL`), a number of
  flags out of a long name list (`ACH_SET`: dishes, Compendium entries), the values of a name list added up (`ACH_SUM`:
  enemy kill counters) or the slots of a string array that are filled / hold a given text (`ACH_STR`: the horse names).
  The long lists are in `runtime/achievements_sets.h`. The table is in order of difficulty, easiest first (a rough guess of the
  effort in a normal playthrough), and that is the order of the lists in the menus; ids are what is saved, so moving a line
  loses nothing.
  Flag names are the game's own GameData names; the save stores their crc32 (`runtime/services/test_warp.c`).
- **Per save**: each `game_data.sav` (the numbered slots of a profile, `save/<profile>/<n>/`: autosaves and manual
  saves) has its own achievements, those its contents meet - a playthrough's progress only adds up, so its save says what
  it has done. The ones shown are those of the save being played: the one the game loaded (opened to read,
  `ach_game_loaded`) or saved (opened to write, `ach_set_active`; both from `runtime/services/fs.c`) last; before it
  opens one, the newest. A save the game writes is announced against the one that was being played: what it has that
  that one hadn't gets a notice, and the times of the rest carry over. So a new game - which the game writes over one
  slot, leaving the old playthrough's saves in the others - starts from nothing (its first save meets nothing), loading
  an older save shows its achievements again without notices, and a save copied in from elsewhere (a backup, a 100%
  save) counts as it is. `SAVAGEWIND_LOG_SAVES=1` logs which saves the game opens (to read or to write) and what it
  creates, deletes and renames in the save folder.
- **What is kept** (`achievements.txt` in the data root, next to `savagewind.ini`): a `@ <tab> save file <tab> file time`
  line per save, then `id <tab> unix time` for each achievement it has. A deleted save is forgotten. Records of whole
  profile folders, from builds that kept one list per playthrough, are dropped (the saves are read again anyway).
- **In the game's own menu** (`runtime/ui/game_options.mm`): pause menu > System > Options > **Achievements**, a page of
  the Options screen with one row per achievement: its state on the right (Unlocked, `4 / 25`, Locked) and, on the
  description line, what it takes and when it was unlocked. The first page's button says how many are unlocked. The
  page is built when it opens, so it shows what the latest save looked at. The pause menu's System page has its own
  **Achievements** button under Load, in the gap the layout leaves for the Nintendo Switch 2 Edition upgrade: the page
  adds that seventh button (0x53, `Pa_Btn_06`) itself when the upgrade is offered, and it is told it is
  (`pause_upgrade_offer` / `_owned`). It opens the Options screen straight on that page, and B goes back to the System
  page (`pause_button` in `tools/patches.py`; a game version where these sites aren't found has no such button).
  The title screen menu has one above Options: its layout (`blyt/Title_00.bflyt` in `Pack/Title.pack`) gets an
  eighth button part, `Pa_SaveFile_06`, a copy of `Pa_SaveFile_05` whose text boxes read Achievements in the menu
  language (`title_button_patch` in `runtime/services/fs.c`; `SAVAGEWIND_NO_TITLE_ACH=1`: the game's menu), and the
  game makes its button like the others' (id 0). Each menu update moves it before Options in the menu's button list
  (the cursor's order), puts it where Options is and Options and the buttons after it 40 lower, whichever buttons the
  saves and the downloadable content show (`title_ach_button` in `runtime/ui/game_options.mm`); pressed, it reads as
  Options for that press, with the Achievements page due.
- **Overlay** (`runtime/ui/achievements_ui.mm`): a toast at the top centre on each unlock, and an "Achievements" tab in
  the start screen's Settings.
- **Sound** (`runtime/achievements_sound.c`, `achievements_bars.c`): the notice comes with the game's own hidden Korok
  "Yahaha!" (`NV_Korogu_Man_Young_Normal00_HiddenKorok_Appear00` in `Voice/Common/NV_Korogu_Man_Young_Normal00.bars`),
  read from the player's RomFS and decoded (DSP-ADPCM) the first time a sound is needed: nothing of the game ships with
  Savage Wind. Without it (game files not set up), a WAV file named `achievement.wav` (or `sounds/achievement.wav`) in the
  data root, next to `savagewind.ini`; failing that, a short "ya-ha-ha!" made from sine waves (`achievements_synth.c`).
  The volume is the game's master volume. `SAVAGEWIND_ACH_SOUND=0` turns it off (so does `SAVAGEWIND_NO_AUDIO`).
  `tools/ach_tool sound <file.bars> <name> <out.wav>` writes any sound of a bank as a WAV file.
- **Pictures** (`runtime/ui/ach_icon.mm`): `achievements/<id>.png` (or `.jpg`) in the data root, named after the
  achievement's id (`paraglider.png`, `shrine_120.png`, ...), square, up to 1024 x 1024. They show on the notice and in
  the overlay's tab; an achievement without one gets the gold diamond. None ship with Savage Wind: they're the player's.
- **No cheating by accident**: while a test warp / flag edit is active (`SAVAGEWIND_TEST_*`) progress is tracked but nothing
  unlocks. Anything that later gives assists (infinite stamina, no damage, teleport) should call `ach_set_blocked(true)`.

## Adding or fixing one

1. Add a line to `g_ach_defs` (new `id`s must be unique and are saved: never rename a shipped one).
2. Check the flag names against a real save: `tools/test_achievements.sh` builds `build/ach_tool`, then
   `build/ach_tool verify <save>/game_data.sav` says which achievements have flag names that aren't in the save.
3. To find a flag you don't know (say the Korok seed counter): copy `game_data.sav` (under `save/`) as `before.sav`, do the
   one thing in the game (collect one seed), save, copy it again as `after.sav`, and run
   `build/ach_tool diff before.sav after.sav`. It lists each entry that changed as `#<crc32>  old -> new`; a counter shows
   as `<-- +1`. Other changes (play time, position, time of day) are there too. Names are filled in for flags the tool
   knows, and for any list of names you pass as a third argument (one per line). Without the name, use the hash itself as
   the flag in `achievements_defs.c`: `"#deae8325"`. When a single `diff` has several `<-- +1` (the game changes many counters at
   once), take a third save after collecting one more and run `build/ach_tool counter s0.sav s1.sav s2.sav`: it keeps
   only the entries that went up by exactly 1 at every step. `build/ach_tool guess <save>` checks a list of likely names, and
   `build/ach_tool get <save> <name|#hash>` reads one.
4. Texts go through `tr()` like the rest of the menu; Spanish is in `runtime/ui/i18n/es.inc`, the other languages show English.

The three Korok achievements read the seed counter by its hash, `#8a94e07a` (name unknown; found with `ach_tool counter` on
real saves holding 2, 3 and 4 seeds).

Only `IsGet_PlayerStole2` and `MaxHartValue` are confirmed from this repo's own notes; the rest of the table needs a
`verify` pass against a real save (the entries added last, below, were never checked).

Tests: `tools/test_achievements.sh` (no SDL or game files needed).

## Leaving the Shrine of Resurrection

`leave_shrine` ("Awakening") reads `IsPlayed_Demo103_0`: `Demo103_0` is the cutscene of leaving the Shrine of Resurrection
(ZeldaMods, WorldMgr / Time: the game itself checks it to start the clock), and `IsPlayed_Demo<N>_<k>` is the save flag of a
cutscene having played. The name comes from those notes and wasn't checked against a save here: on one taken after the
cutscene, `build/ach_tool get <save>/game_data.sav IsPlayed_Demo103_0` should print `= 1` (and `not in the save` means the flag
isn't saved under that name: `ach_tool diff` on a save before and after leaving the shrine shows what changed).

## Flag names that are known (checked against a real save)

- `Defeated_<actor>_Num`: kills of each enemy (`Defeated_Enemy_Lynel_Dark_Num`, `Defeated_Enemy_GanonBeast_Num`, ...).
- `IsRegisteredPictureBook_<actor>`: that Compendium entry is registered. `IsGet_<actor>`: you have had that item (dishes are
  `IsGet_Item_Cook_*`, runes `IsGet_Obj_Magnetglove`, `IsGet_Obj_StopTimer`, `IsGet_Obj_IceMaker`, `IsGet_Obj_Camera`,
  `IsGet_Obj_RemoteBomb`; `IsGet_Obj_RemoteBombCube` stays 0 even in a 100% save: the round and cube bombs are one rune).
- `IsGet_Weapon_Sword_070`: the Master Sword (`IsGet_Weapon_Sword_080` is something else: 0 in a 100% save).
- `MapTower_01` .. `MapTower_15`: Sheikah Towers activated.

Not in the saves (no flag found, so no achievement): the 12 memories, the Great Fairies, reflecting a Guardian beam, Flurry
Rush, bare-handed boss kills, the Eventide-style no-food climb, taming Epona / the giant horse.

## Gear, ancient parts and horses (not yet checked against a real save)

These six were added without a save at hand: their names come from BOTW's item tables (cheat-engine / trainer item
lists, the game's shield parameters) and from the zeldamods.org note on the save format. Run
`tools/ach_tool verify <save>/game_data.sav` once on a real save: any of them that says `NO FLAG IN SAVE` has a wrong
name, fix it in `achievements_defs.c`.

| id | what it reads |
| --- | --- |
| `ancient_parts` | `IsGet_Item_Enemy_26` .. `31`: Ancient Gear, Screw, Spring, Shaft, Core and Giant Ancient Core |
| `dark_armor` | `IsGet_Armor_160_Head`, `_Upper`, `_Lower`: the Dark Set (Dark Hood, Tunic, Trousers) |
| `hylian_shield` | `IsGet_Weapon_Shield_030` |
| `old_memories` | `IsGet_Weapon_Sword_070` (Master Sword) + `IsGet_Weapon_Shield_030` |
| `horse_registered` | any of the 6 horse-name slots is not empty |
| `horse_iwata` | any slot holds the name "Satoru" (or "Satoru Iwata", "Iwata Satoru"), ignoring case and blanks |
| `beedle` | `Npc_TripMaster_00_Talk` (Beedle: "Terry" in Spanish) |
| `lost_woods` | `Location_Cokiri`: the Korok Forest's map marker reached (the Lost Woods are `LostForest`) |
| `maracas` | `IsGet_Obj_Maracas` |
| `gerudo_outfit` | `IsGet_Armor_053_Head`, `_Upper`, `_Lower`: the Gerudo vai (veil, top, sirwal) |
| `arrows` | `IsGet_` of `NormalArrow`, `FireArrow`, `IceArrow`, `ElectricArrow`, `BombArrow_A`, `AncientArrow` |
| `eventide` | `NakedIsland_Finish` ("Stranded on Eventide", the island `Cohorint` on the map) |
| `stables` | `Location_<stable>Hatago` >= 1 for the 15 stables (visits; `Location_Hatago001` stays 0) |
| `great_fairy` | `ACH_ANY`: one of 19 sets with its head, body and legs at four stars (`armorStarNum` 5 in ActorInfo) |
| `all` | `ACH_META`: every other achievement; its notice plays `Sys_Challenge_Complete` (`Sound/Resource/M_UI.bars`) |

The save keeps the horse names as 6 slots of 64 characters under the hash `0x7b74e117` (96 entries, 4 characters each,
`ACH_STR`). `build/ach_tool horses <game_data.sav>` prints the slots: register a horse, save, and check that its name
shows up. A name is read as plain text (UTF-8) up to the first NUL, or as ASCII stored in UTF-16. If the slots stay
empty on a save that has a registered horse, the hash or the layout is not what the notes say: use `ach_tool diff` on a
save before and after registering a horse to see what changes.

An `IsGet_` flag stays at 1 once you have had the item, even if it breaks or you sell it.

## Waiting for its flags: "Plus Ultra"

"Plus Ultra" (*Mejora la tableta Sheika al máximo*) is the side quest "Slated for Upgrades" (*Mejora la tableta sheikah*):
Purah upgrades the Sheikah Sensor (3 Ancient Screws), the Remote Bombs (3 Ancient Shafts) and Stasis (3 Ancient Cores).
It isn't in `g_ach_defs` because the flags those upgrades set are not known, and an achievement whose flags aren't in the
save can't unlock. To find them (no need to do all three at once):

1. Copy `game_data.sav` as `before.sav` right before talking to Purah (have the materials with you), then upgrade one
   rune, save, and copy it as `after.sav`.
2. `build/ach_tool diff before.sav after.sav`: the flag of that upgrade is the entry that went `0 -> 1`; play time,
   position and the spent materials change too. Do the same for the other two runes.
3. Add it to `achievements_defs.c`, with the hashes of the three upgrades (`"#<hex crc32>"`):

```c
    {"plus_ultra", "Plus Ultra", "Upgrade the Sheikah Slate to the max.", ACH_ALL, "#aaaaaaaa,#bbbbbbbb,#cccccccc", 0, 0, 3},
```

and the Spanish text in `es.inc`:

```c
T("Plus Ultra", "Plus ultra")
T("Upgrade the Sheikah Slate to the max.", "Mejora la tableta Sheika al máximo.")
```
