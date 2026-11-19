# Colour vision options: design

2026-11-15, BotW 1.9.0. **Status:** phase 1 implemented the same day, on the user's request, as the first rows of
Options > Accessibility (runtime/colour_vision.c, the present shader variant). The game-specific help under "Later"
is not started. Player docs: [Cheats and assists](../cheats-and-assists.md#colour-vision).

## Goal

Players with a colour vision deficiency (CVD) can tell apart the game's colour cues: Chuchu and Keese elements, enemy
tiers, the stamina wheel's exhausted state and the like. The option sits at the top of Options > Accessibility, so it
is one of the first things a player finds, and it can also be offered at the first setup.

About 8% of men and 0.5% of women have red-green CVD (protan or deutan, most of them the milder "anomalous" forms);
tritan (blue-yellow) is rare. Red-green is the case that matters most.

## Colour cues in BOTW

Approximate, from knowledge of the game; the rows marked "verify" must be checked in game with the simulation mode
below before relying on them.

| Cue | Colours | Other cue the game gives | Risk |
|---|---|---|---|
| Chuchu, Keese, Wizzrobe elements; Chuchu Jelly | red fire, yellow shock, white-blue ice, green-blue normal | flames, sparks, frost close up; the jelly's name in the pouch | High (red / yellow / green) |
| Enemy tiers: Bokoblin, Moblin, Lizalfos, Lynel, Hinox | red, blue, black, silver, gold | none besides how hard they hit | High (red looks dark to protans: red vs. black) |
| Lizalfos elements | red fire, yellow shock, white ice | breath effects | Medium |
| Stamina wheel | green, red when exhausted, yellow extra | Link pants, the wheel empties | Medium |
| Hearts | red, yellow extra hearts | extra hearts come last in the row | Low |
| Temperature gauge | blue cold, red hot | needle position, flame icon | Low |
| Shrines and towers | orange glow not done, blue done | map icon (verify whether it differs) | Low |
| Guardian laser | red-pink laser and dot | sound, music | Low |
| Map pins and beacons | verify | | verify |
| Weapon comparison in the pouch | verify whether better / worse is colour only | | verify |

## What the player sees

Options > Accessibility gets a "Colour vision" group as its first rows, above God mode:

| Row | Values | Default |
|---|---|---|
| Colour vision | Off; Protanopia (red looks dark, red and green look alike); Deuteranopia (green and red look alike); Tritanopia (blue and yellow look alike) | Off |
| Correction strength | 10% to 100% in 10% steps (always shown; no effect while Colour vision is Off) | 100%, to be revisited with testers |

Changes apply at once. The game's own Options screen is drawn by the game and corrected like the rest of the
picture, so the player sees the effect while changing it.

Placement decisions for the user:

1. The two rows at the top of Accessibility (proposed).
2. Accessibility moved up in the main Options list, for example right after Gameplay (it is 8th today).
3. A "Colour vision" row in the launcher's first setup ("Set up" section, next to "Savage Wind colour look").
4. The main page's Accessibility description changes to "Colour vision and assists that make the game easier."

A "World only" choice (HUD keeps its colours) is cheap to add later, because the colour grading pass already runs on
the scene before the HUD. It is left out of the first version to keep the page simple.

## How it works

### Colour science

- Simulation: Machado, Oliveira and Fernandes (2009) matrices at severity 1.0, in linear RGB. The paper also gives
  milder severities; the implementation uses only the 1.0 matrices, and the strength slider scales the correction
  (k in runtime/colour_vision.h), which also suits the milder anomalous forms. Chromium's developer tools use the same model for their vision deficiency emulation; Unreal Engine offers a
  similar "type plus severity, simulate or correct" option, which is the usual pattern players know from games.
- Correction ("daltonize"): the colour difference the player cannot see, `c - S c`, is moved into channels they can
  see: `c' = c + E (c - S c)`. For protan and deutan, E moves the lost red-green difference into green and blue
  (`[[0,0,0],[0.7,1,0],[0.7,0,1]]`); for tritan, the lost blue-yellow difference into red and green
  (`[[1,0,0.7],[0,1,0.7],[0,0,0]]`).
- Both fold into **one 3x3 matrix** per type and strength, `M = I + E (I - S)`, computed on the CPU when the setting
  changes. The GPU cost is one matrix multiply per pixel.

Correction matrices at full strength (linear RGB, rows = output R, G, B):

```
protan  [ 1.0000  0.0000  0.0000 ] [ 0.4789  0.4769  0.0442 ] [ 0.5973 -0.6887  1.0914 ]
deutan  [ 1.0000  0.0000  0.0000 ] [ 0.1628  0.7250  0.1122 ] [ 0.4547 -0.6454  1.1907 ]
tritan  [ 0.7412 -0.4072  0.6660 ] [ 0.0751  0.5852  0.3397 ] [ 0.0000  0.0000  1.0000 ]
```

Synthetic check: colour difference (CIE76 ΔE; above ~10 is easy to tell, below ~5 hard) between approximate cue
colours, as a CVD player sees them before and after the correction. Colours picked by eye, not measured in the game.

| Pair | Normal | Protan: before, after | Deutan: before, after | Tritan: before, after |
|---|---|---|---|---|
| Fire Chuchu red / normal green | 110 | 27, 60 | 18, 52 | 119, 100 |
| Shock Chuchu yellow / normal green | 60 | 39, 43 | 47, 44 | 57, 41 |
| Heart red / extra heart yellow | 85 | 68, 66 | 38, 74 | 73, 64 |
| Stamina green / exhausted red | 121 | 43, 66 | 18, 59 | 129, 104 |
| Shrine orange / blue | 126 | 113, 75 | 126, 77 | 101, 62 |
| Red / black Bokoblin | 67 | 37, 42 | 55, 37 | 77, 56 |
| Ice Chuchu light blue / normal green | 76 | 72, 76 | 63, 73 | 29, 47 |

The red-green pairs, which are the real problem, get two to three times further apart. Pairs that were already far
apart lose some distance but stay easy to tell. At full strength the whole picture shifts (green-tinted for protan
and deutan), which is why the strength slider exists. Demo pictures of two screenshots (simulated, corrected,
corrected as seen) were made with a scratch script; they are kept outside the repository, in
`issue-logs/alpha6-colour-vision/` of the working folder.

### Where in the renderer (as implemented)

The correction runs on the final picture, after the game has drawn its HUD and menus:

- `vk_present_draw` (runtime/nvn/vulkan/exec.c) draws the game's image into the window with `present.frag`, after
  `post_process` (SMAA, FSR 1). With the option on it uses `present_cvd.frag` (runtime/nvn/vulkan/shaders.c) instead:
  the matrix rows as fragment push constants after the vertex shader's 32 bytes (the present pipeline layout gained
  that range). With the option off the pipeline is the same as before.
- **SDR:** the game's 8-bit image is read raw, so it holds sRGB-encoded values: the shader decodes them, applies M and
  encodes again.
- **HDR10:** the game's float picture (RGBA16F, linear, HUD included) also goes through `vk_present_draw` before the
  PQ pass in device.c, so the same shader applies M to it directly (a flag in the push constants says the picture is
  linear). The ImGui menu is drawn separately and keeps its colours.
- **Frame generation:** generated frames are also drawn with `vk_present_draw`, so they are corrected the same way.
  The generator's own HUD-less input is untouched.
- **Not corrected, on purpose:** the Savage Wind menu and overlays (ImGui, drawn after the present), and Album photos
  taken with the camera rune (runtime/hle/photo.c captures the game's image before the present), so photos keep the
  game's real colours. Window captures (Steam, OBS) show the corrected picture.
- **macOS / Metal:** not done; the build is unmaintained and macOS has its own system colour filters.

### Code

- `runtime/settings.h` / `settings.c`: `colour_vision` (`off`, `protanopia`, `deuteranopia`, `tritanopia` in the ini)
  and `colour_vision_strength` (percent, 10 to 100), saved in savagewind.ini like the other Accessibility rows.
- `runtime/colour_vision.c/.h`: the three Machado matrices and `colour_vision_matrix(float m[9])`, false when off.
- `runtime/ui/game_options.mm`: the two rows at the top of the `P_ACCESS` items (before God mode). The type's
  description names the chosen type; the strength row is always shown.
- Translations in all 13 `runtime/ui/i18n/*.inc` tables (`tools/i18n_check.py`: none missing).
- Player docs: the "Colour vision" section of docs/cheats-and-assists.md, a README line and a CHANGELOG entry.
- Not done: a row in the launcher's first setup, and moving Accessibility up in the main Options list.

### Testing

- Developer simulation mode: `SAVAGEWIND_CVD_SIMULATE=protan|deutan|tritan[:<percent>]` shows the picture as a
  dichromat sees it, after the correction if one is on. It is meant for auditing the cues above (the rows marked
  "verify") and for before / after screenshots.
- Done 2026-11-15 (Windows, hidden runs, Great Plateau, 60 fps cap): the setting is read and saved back; the run
  reaches the world at a steady 60 fps; window shots show the corrected picture with the HUD corrected (deuteranopia:
  the red hearts turn pink) and the F10 overlay untouched. The pictures are in `issue-logs/alpha6-colour-vision/` of
  the working folder.
- Still to do: HDR checked by eye on an HDR display, one Steam Deck run, frame time on and off at 4K, and testers
  with colour blindness on the Discord to set the default strength.

## Later: game-specific help

Not needed for the first version; considered after testers try the filter.

1. **Name on lock-on.** While Z-targeting, a short label with the enemy's name from the game's own texts (for
   example "Electric Chuchu", "Blue Bokoblin"). It covers element and tier without relying on colour at all. It needs
   reverse engineering of the lock-on target and the actor's localised name (none exists in the runtime yet), new
   1.9.0 sites in tools/game_sites.py, and a way to draw the label (overlay or the game's own layout). Medium to
   large.
2. **Targeted recolouring** of specific shaders (shrine and tower glow, guardian laser) by shader hash. Fragile and of
   little value once the full-picture correction exists.
3. **High-contrast HUD** and bigger icons: a separate accessibility topic.

## Risks

- The models are approximations, and real CVD varies from person to person; the strength slider and tester feedback
  cover this.
- At full strength the picture looks different from the game's art; some players will prefer a lower strength.
- The correction runs after the colour grading and the Savage Wind LUT, which are on by default; the two compose,
  but testers should judge them together.
- Off by default, and the Off path is unchanged, so players who do not use it are not affected.

## Later work

The game-specific help above is not Alpha 6 work. The launcher setup row and moving Accessibility up in the main
list are small and can follow if the user wants them.
