# HDR output

Savage Wind presents in HDR10 when **Video > HDR** is `on`, or `auto` while the display is in HDR. The display must
offer an HDR10 swapchain format (`VK_EXT_swapchain_colorspace`, ST 2084 / Rec. 2020); otherwise the picture stays SDR.
The HDR present path is the Vulkan backend (Windows and Linux); the calibration-profile read below is Windows-only.

## How BOTW's picture reaches the display

BOTW renders the 3D scene and its bloom into two **B10G11R11 float** images at the internal resolution (1600x900
docked). Its scene tonemap (fragment shader `72cb7ff600`, the engine's `hdr_compose` pass) adds them, applies the
game's own `1 - e^-x` curve and saturation grade, and writes a 1600x900 `A2B10G10R10` image. The game's upscale/FXAA
pass (`72caebd800`) brings that to the 1920x1080 final image, the HUD is drawn into it, and the game copies it
sRGB-encoded into the 8-bit image it presents. That copy clips everything above white.

With HDR on:

- `runtime/nvn/vulkan/textures.c` creates the 1600x900 scene tonemap target **and** the 1920x1080 final image as
  **RGBA16F**, so the extended curve's highlights reach the game's upscale/FXAA pass and the HUD. The B10G11R11
  scene and bloom sources are already float; small 10-bit scene targets (effects/copies) stay as they are.
- `runtime/nvn/vulkan/exec.c` (`hdr_make_tonemap_prog`) recognises the tonemap at its first draw by its own fragment
  shader constants (`0.2989`, `-1.442695`, `10000`, `2/3`) and its two float scene/bloom textures, then replaces its
  fragment shader with the HDR tonemap: RenoDX's **extended vanilla** curve (the game's own `1 - e^-x` up to a pivot,
  continued along its tangent above it, keeping BOTW's exact luminance/per-channel mix and saturation grade), the
  MacLeod-Boynton hue/purity transfer, and then either the RenoDX BOTW mod's **PsychoV test17 + per-channel Neutwo**
  (the default) or the game's own clipped tonemap (Vanilla). The output is linear, with 1.0 at the HUD's white.
- `exec.c` (`hdr_record_draw`, `hdr_record_copy`) remembers the float image the game's final copy reads. The present
  then uses that image instead of the clipped 8-bit copy.
- `device.c` draws the game's picture into an RGBA16F window-size image, PQ-encodes it to the HDR10 swapchain
  (`hdr_pq`), draws the overlay menu into an RGBA8 image and blends it on at the paper white (`hdr_ui`), and sends
  `VkHdrMetadataEXT` with the configured peak.

The in-game HUD is part of the game's picture. The 3D scene and the HUD/menu have separate whites (`hdr_game_nits`
and `hdr_ui_nits`, both 203 by default); the peak is the final cap for both.

BOTW's shader archives (`Bootup_Graphics.pack` → `uking_sys` / AGL `hdr_compose`) and its embedded shader names
(`HDRCompose`, `hdr_compose`, `HDR_TONEMAPPING`, `HDR_BLOOM_COMPOSE`) confirm this pass; the `72cb7ff600` program
is the one the game draws into the 1600x900 scene target.

The **main-menu background** is not this 3D pass: it is the static 2D layout art in `Layout/Title.sblarc`
(`timg/__Combined.bntx`, with `blyt/Title_00.bflyt` for the logo and menu). It is drawn after the scene tonemap and
therefore does not use the extended curve. The layout texture can itself hold values slightly above 1.0 (the current
menu art measures up to ~1.7x paper white), so it is not clamped exactly to paper white; that is the 2D layout's own
range, not a 3D scene highlight. `TitleBG.pack` holds the separate 3D title scene (terrain, Link, `WorldMgr`); when that scene
is drawn, it uses the same scene path as the game.

## Settings

| INI key | Meaning | Default |
|---|---|---|
| `hdr` | `auto`, `on`, `off` | `auto` |
| `hdr_peak_nits` | the display's peak: the scene's roll-off target **and** the final cap for every element; on Windows the first start fills it from the monitor's HDR calibration profile (1000 nits if none), then it is editable; other platforms default to 1000 and it is set manually | Windows: ICC peak / 1000; elsewhere 1000 |
| `hdr_game_nits` | the 3D scene's white (paper white), capped by the peak | `203` |
| `hdr_ui_nits` | the HUD and menu white, capped by the peak | `203` |
| `hdr_tonemapper` | `0` Vanilla (the game's own tonemap, clipped at white), `1` RenoDX (PsychoV + per-channel Neutwo; the BOTW mod's "RenoDRT" option) | `1` |
| `hdr_exposure` | PsychoV exposure, percent of the scene's own | `100` |
| `hdr_highlights` | PsychoV highlight contrast (0..100; 50 neutral) | `50` |
| `hdr_shadows` | PsychoV shadow contrast (0..100; 50 neutral) | `50` |
| `hdr_contrast` | PsychoV contrast around mid-grey (0..100; 50 neutral) | `50` |
| `hdr_saturation` | PsychoV saturation (0..100; 50 neutral) | `50` |
| `hdr_highlight_saturation` | PsychoV highlight colour (0..100; 50 neutral, below desaturates toward white) | `50` |
| `hdr_flare` | PsychoV flare/glare compensation (0..100; 0 neutral; the mod's steep `0.10 * pow(value, 10)` mapping) | `0` |
| `hdr_gamma` | the mod's `ToneMapGammaCorrection`: `0` none, `1` 2.2, `2` BT.1886 (2.4); it adjusts the per-channel Neutwo peak for gamma-encoded output. `0` is correct for this linear/PQ path; the mod defaults to `1` (2.2) for its gamma-encoded output | `0` |
| `hdr_look1`..`hdr_look3` | the three saved HDR looks (`Video > HDR preset`), 18 space-separated values: tone mapper, exposure, highlights, shadows, contrast, saturation, highlight saturation, flare, hue, blowout, SDR blend, vanilla saturation, bloom, clamp, peak, game, UI, gamma | the mod's defaults; peak/game/UI start as 0 (keep the current display setup): on Windows the first start fills the ICC peak, elsewhere they stay 0 until a preset is saved (a saved preset stores the effective nits) |
| `hdr_preset` | `0` Preset Off (the mod's OnPresetOff; it leaves SDR blend, vanilla saturation and gamut clamp as they are), `1`..`3` the selected saved slot; `4` is Custom, derived by the UI when the values match no state and never stored | `1` |
| `hdr_hue` | highlight hue/purity transfer: how much of the game's SDR hue the extended colour takes, in MacLeod-Boynton space, percent (`0` off) | `70` |
| `hdr_blowout` | SDR blowout: how much of the game's SDR highlight desaturation/purity the transfer keeps, percent | `65` |
| `hdr_sdr_blend` | extended-curve pivot: where the curve leaves the game's `1 - e^-x` and continues straight, percent of scene white | `18` |
| `hdr_vanilla_saturation` | the game's own saturation grade strength, percent | `100` |
| `hdr_bloom` | the game's bloom strength in HDR, percent (`0` removes it) | `100` |
| `hdr_clamp` | the clamp after the grade: `0` BT.709 (the game's own), `1` BT.2020 | `0` |
| `hdr_clip_nits` | legacy, ignored; the final cap is always `hdr_peak_nits` | — |
| `hdr_paper_white` | old SDR fallback key, used only if the game and UI whites are both `0` | Windows' SDR white |

The peak, game and HUD brightness, the tone mapper, the PsychoV sliders and the rest of the HDR look controls (hue,
blowout, SDR blend, vanilla saturation, bloom, clamp, presets) are in the HDR menu and the game's own Options screen;
the peak slider moves in 10-nit steps, the hue in 5% and the others in 1%. The HUD and menu default to 203 nits (HGIG
reference white), and the 3D scene's paper white also defaults to 203.

`Video > HDR preset` saves and recalls three complete looks: the tone mapper, every look control, the display peak,
game and UI whites, and gamma (the mod's presets do the same). `Preset Off` is the mod's OnPresetOff: Vanilla tone
mapper, peak/game/UI 203, neutral grading, hue 100, blowout 0 and bloom 100; SDR blend, vanilla saturation and gamut
clamp stay as they are, as the mod's OnPresetOff leaves them. It starts on Preset 1, which matches the RenoDX BOTW mod's
defaults. `Save HDR preset` writes the current look into the last selected slot (Preset 1 when Off or Custom); the
values apply live, as in the mod. `Custom` is shown but not selectable: it means the current values differ from the
selected slot. Select Preset 2, adjust the sliders and Save to write that look back into Preset 2; with no slot
selected, Save uses Preset 1.

The overlay/launcher HDR group keeps the tone mapper, peak, game (paper white) and UI brightness visible and folds
the rest, from HDR exposure through Save HDR preset, under `Advanced (show)`; it starts collapsed. The game's own
Options screen keeps its flat page.

A fresh settings file has no peak value. On Windows, the first start reads the display's **ICM profile association**
(`ICMProfileAC`) for the monitor the game window is on, and uses the last valid profile's **MHC2** tag
`max_luminance` — the display's small-area peak. It writes that value into the settings and logs the ICC file and
peak. The standard `lumi` tag is the maximum full-frame luminance, not the highlight peak. The group for the window's
monitor is used, not another display's profile; if that group has no profile with an `MHC2` tag, the monitor's default
profile is tried, and the result is 0 (no profile) if that has none either. On other platforms there is no Windows
calibration profile: the peak defaults to 1000 and is set manually.

The value is then a normal editable setting; on Windows the setting's description shows the current ICC profile and
peak, or says no profile was found. If no profile is found, the first start writes **1000 nits** and the description
says so (`No Windows HDR calibration profile found; peak defaults to 1000 nits.`). There is no 0/automatic value in the
UI.

The scene's tonemap rolls off at the peak, and the present clamps the final signal — scene, flashes, particles, HUD and
menu — to that same peak. The peak also caps the game and UI whites: if either is set above it, the effective white is
the peak. The game and HUD whites are separate so the 3D scene can be brighter or dimmer against the HUD; both default
to 203.

The picture is PQ-encoded at the HUD/paper white; the opening awakening cutscene is rendered in real time; the
`Movie/*.webm` files are the memories and other demos, not that sequence.

The highlight hue correction (`hdr_hue`) is a compact port of RenoDX's MacLeod-Boynton hue/purity transfer: the
extended colour takes the hue angle and purity of the game's own SDR result while keeping its HDR luminance, so a
fire's core reads yellow-white as in SDR instead of staying deep red. `hdr_blowout` is the purity strength (the SDR
blowout); `hdr_hue` is the hue strength, weighted by the SDR donor's chroma. The defaults, 70 and 65, are RenoDX
BOTW's.

`hdr_sdr_blend` is the extended curve's pivot (RenoDX BOTW's 18%). `hdr_vanilla_saturation` scales the game's own
saturation grade (100 is the game's look); the grade is normalized by RenoDX's Neutwo max-channel scale
(`1/sqrt(max^2+1)`), as the mod does. `hdr_bloom` scales the bloom the game's tonemap adds (100 is the game's own
bloom; 0 removes it in HDR). `hdr_clamp` clamps negative channels after the grade in BT.709 (the game's own) or
BT.2020.

The default tone mapper is the RenoDX BOTW mod's **PsychoV test17** path: its `ApplyTest17BT2020` (exposure, highlights,
shadows, contrast and flare on the Yf luminance, saturation and highlight saturation in MacLeod-Boynton purity, with
the mod's post gamut compression) and then its per-channel `neutwo` fit to the display peak. `runtime/nvn/vulkan/hdr_psycho.c` is a GLSL
port of the game-local `psycho_test17_custom.hlsli`, limited to the path the mod's `BuildPsycho17Config` uses (no LMS
tonemap, bleaching, hue emulation, pre-gamut compression or dechroma). The mod's generic template labels this
`ToneMapType = 1` option "RenoDRT", but the shader runs PsychoV, not RenoDRT; this UI names it "RenoDX (PsychoV)".
Vanilla (`0`) uses the game's own tonemap and grade, clipped at white, so it has no scene detail above white.

`hdr_saturation` is the PsychoV purity scale, and `hdr_highlight_saturation` is the mod's `ColorGradeHighlightSaturation`
(the `purity_highlights` term): below 50% it desaturates highlights toward white, above 50% it adds highlight colour.
The neon-like shifts of the per-channel PsychoV path are why the mod's `hdr_blowout` transfers the SDR purity first.

The HDR look (tone mapper, sliders, hue/blowout/SDR blend/vanilla saturation/bloom/clamp, gamma and the presets,
including peak/game/UI) is read from a push-constant block on every HDR tonemap draw, and the HDR metadata is re-sent
when the peak or scene white changes, so changes apply live, as in the mod. Only `hdr` itself still needs a restart:
turning HDR on or off changes the game's target formats (RGBA16F) and the swapchain, which happen at start. `auto`
follows the display's HDR state at the next start.

## Debugging

- `SAVAGEWIND_HDR_RT8=1`: keep the final image 10-bit (no HDR float path).
- `SAVAGEWIND_HDR_TONEMAP=0`: keep the game's tonemap (the float path still presents its output, without the extended
  curve; the game's own 8-bit copy would clip it).
- `SAVAGEWIND_SAVE_MSL=1`: save translated GLSL under `build/glsl/`.
- The log says whether the tonemap was replaced and whether the present found the float picture:
  `[hdr] the game's tonemap runs as the HDR tonemap (fs 72cb7ff600)`, `[hdr] presenting the game's float picture`,
  `[hdr] HDR10 output, but the game's float picture was not found`. On Windows the first start logs the ICC profile and
  peak (`[hdr] Windows HDR calibration profile: "<profile>.icc", <peak> nits`, then
  `[hdr] peak brightness set to <peak> nits (from the ICC profile)`), or
  `[hdr] no Windows HDR calibration profile found` / `[hdr] peak brightness set to 1000 nits (no ICC profile)`; the
  first present logs the paper white and the peak/final cap:
  `[hdr] present: paper white <UI white> nits, peak <peak> nits (final cap)`.
- `SAVAGEWIND_WINDOW_SHOT=<frame>` saves the presented HDR float picture as an SDR PNG and logs its true range, e.g.
  `[hdr] float picture: max <x> (<nits> nits), mean <y> (<nits> nits)`; the raw float image can exceed the peak (the
  HUD/menu at their own white, or post-tonemap effects), and the present clamps it to the peak. (The control
  endpoint's `shot` saves the game's clipped 8-bit copy instead, so highlights there are white.)

## Limitations

- AMD FSR 3 frame generation is off while HDR is on: its intermediate copies are 8-bit and would clip the picture.
- FSR 1 on the final image is off while HDR is on (it expects a gamma-encoded signal). The scene upscaler and TAA run;
  the grade and sharpening passes preserve values above 1.0, so the extended highlights survive them.
- The PsychoV port is the subset the RenoDX BOTW mod actually runs (`ApplyTest17BT2020` with its fixed configuration);
  the mod's unused full-observer `psychotm_test17` (LMS Naka-Rushton response, bleaching, hue emulation) and its
  advanced controls (dechroma, hue shift, adaptation) are not ported. The mod's generic template labels this option
  "RenoDRT", but its shader runs PsychoV; this UI names it "RenoDX (PsychoV)".
- The Windows ICC first-start read selects the monitor's profile group by its `\\.\DISPLAYn` number; Windows exposes no
  stable mapping between display numbers and those groups, so on an unusual multi-monitor setup the description may
  name another display's profile. The log and the setting's description name the profile used. On Linux and macOS the
  peak is set manually.
- The overlay menu's translucent parts blend in PQ code values, so their edges can be slightly off; opaque menu
  pixels are exact.
- The extended curve needs the scene's 1600x900 (docked) tonemap target; at the game's 1280x720 handheld resolution
  that target stays 10-bit and highlights clip at white, as without HDR.
- The optional **Video > Colour** grade, including its LUT, still runs on the HDR scene (`exec.c` `scene_post`,
  `shaders.c` `grade_lut.frag`): the LUT is applied to the SDR part of the linear scene (0..1) and the part above 1 is
  added back, but an SDR-authored LUT can still shift HDR colours. If a scene shows an unexpected tint — the reported
  teal cliffs in Gerudo — turn the LUT off first. The HDR tonemap replacement itself samples only the game's scene and
  bloom, not a LUT.

## Credits

The approach comes from the RenoDX TotK port by ProSep
([totk-pc-recomp#281](https://github.com/valentderah/totk-pc-recomp/pull/281)), which credits the RenoDX TotK mod:

- game mod by souperman9
- tonemapper by Musa
- RenoDX by ShortFuse (Carlos Lopez Jr.)
- RenoVK fork by Ritsu

Sources: [RenoDX](https://github.com/clshortfuse/renodx), MIT; the curve follows its BOTW mod (`src/games/botw`). From
it come two ideas: replacing the tonemap and keeping the game's curve below a pivot, and the observation that the
passes after the tonemap keep values above 1. The MacLeod-Boynton matrices and the hue/purity formula are adapted
from that mod's `macleod_boynton.hlsli` (`CorrectHueAndPurityMBBT709WithBT2020`, MIT); the PsychoV test17 path and the
per-channel Neutwo fit are a GLSL port of its `psycho_test17_custom.hlsli` (`ApplyTest17BT2020`, the gamut-compression
and purity helpers, MIT); the rest of the shader and the runtime code here are new. The PQ (SMPTE ST 2084) and
Rec. 709 -> Rec. 2020 (BT.2087) constants are from the standards.
