# Native Options rows with two binding buttons

Research date: 2026-11-01. Static research only; no build or game launch.

## Conclusion

**Two native controls on one row are feasible, but not through Entry fields alone.**
Every row already contains three alternative button parts. The constructor resolves
all three; initialization enables one and hides the others. Recommended prototype:
use the existing choice control and ordinary button together, remove the choice
arrows from view, position the controls in separate columns, and extend the screen's
button-to-row routing. This retains the game's layout renderer, fonts, button
animations and cursor machinery. Navigation and sound behavior still need live tests.

**The toggle is one CheckButton, not two independently selectable buttons.** Its ON
and OFF text panes can display separate bindings, but that is a custom two-half
interaction, with one shared native hit area/cursor. Also, Entry.button_off/on label
the ordinary button's two states, not the toggle's two labels.

## Evidence scope and reproduction

- Checkout inspected: `feature/action-binding` at `1c79552`.
- Read first: `runtime/ui/game_options.mm` header and Entry/Item definitions, then
  its installation, message, value, decide and refresh hooks; `tools/patches.py`.
- Binary: the extracted 1.9.0 `exefs/main`, NSO build ID
  `cd57b23fa4bbad65803d9788c01821ee` (1.9.0), SHA-256
  `f6f0b20bb1f2b67d164c24871cd473147eef1d285f12e7bd492e3126c6022e27`.
- Addresses below are **1.9.0 runtime VAs**, main base `0x7000000000`, not the
  `0x710...` labels used by the old 1.8.2 comments. No constant-delta translation.
- Used local `swr-re/tools/nso.py` for LZ4 segments, `research.py` for Capstone and
  validated unwind bounds; consulted `RESULTS.md`/`CALLSITES.md` for conventions.
- Ran `game_sites.Main.locate()` on each existing Options hook/data site: each pattern
  had exactly one match and agreed with `KNOWN['1.9.0']`, including field `0x36b0`.
- Asset parsing used this repo's `tools/botw_save_convert.py` SARC/Yaz0 readers and
  disposable Python in `/tmp`. FLYT sections were walked using their own sizes,
  including `pas1`/`pae1` hierarchy and `prt1` references. No game assets are included.
- **Verified** means decoded binary or parsed asset/source. Proposed behavior is
  marked **inferred/proposed**. No runtime object addresses or screenshots were captured.

## 1. How the game builds a row

### Screen and row functions

| VA | Verified operation |
| --- | --- |
| `0x7000e2e960` | Options screen constructor; installs vtable `0x7001cf7988` |
| `0x7000e2f0a8` | createRows, screen vtable `+0x2f0`; existing `opt_create_rows` |
| `0x7000e30b58` | 0x98-byte row constructor, counterpart of old `FUN_7100d1d828` |
| `0x7000e2f2e8` | screen row initialization/scroll setup, vtable `+0x2f8` |
| `0x7000e2f4b4` | initializes one constructed row: title, kind, flag type, value, redraw |
| `0x7000e30d30` | selects which of the three button styles is enabled/visible |
| `0x7000e2fde4` | read value, existing `opt_read` |
| `0x7000e2ee44` | store value, existing `opt_store` |
| `0x7000e2eed8` | redraw value/check state, existing `opt_row_redraw` |
| `0x7000e30700` | row decide, existing `opt_decide`; also reached by directional actions |
| `0x7000e31118` | reset, existing `opt_reset` |
| `0x7000e2f564` | screen update, existing `opt_update`, vtable `+0x300` |
| `0x7000e30ee0` | set named layout text from an OptionWindow message ID |
| `0x7000e30f70` | message lookup call intercepted by existing `opt_msg_call` |
| `0x7000e84870` | original message lookup, existing `msg_lookup` |
| `0x7000e84940` | named layout text setter from a supplied UTF-16 buffer |

createRows reads table `0x7001e2e610`: count `+0`, entries pointer `+8`, row count
`+0x10`, order pointer `+0x18`. It finds `Pa_SlideBtn_00`, clones further row layouts
as `Pa_SlideBtn_%02d`, and constructs one logical row per chosen Entry.
At `0x7000e2f258` it loads allocation size `0x98`; `0x7000e2f28c` calls the constructor.
The original template row has the equivalent allocation/call at
`0x7000e2f2a4`/`0x7000e2f2d0`. Rows are inserted into the screen array at
`screen+0x36b8`, count `screen+0x36b0`. The reverse construction/insertion order
is part of the existing implementation; do not assume array order is visual order.

`0x7000e447d8` clones a layout, attaches its pane tree and optionally builds button
and cursor registrations (`0x7000e44934`, `0x7000e44a98`). It is **not** the row
constructor. `0x7000e30ccc` resolves a previously registered button from a part
layout via `0x7000e44d0c`, then checks its runtime type. No source-level C++ class
name for the row was recovered; it is an aggregate without a vptr at `+0`.

Entry is 0x30 bytes: SafeString vptr `+0`, name pointer `+8`, kind `+0x10`, desc
`+0x14`, title `+0x18`, first/last value `+0x1c/+0x20`, button off/on IDs
`+0x24/+0x28`, pad `+0x2c`. Kinds are 0 toggle, 1 choice, 2 ordinary button.

### Runtime object map

| Row offset | Verified contents/use |
| --- | --- |
| `+0x00` | owning Options screen |
| `+0x08` | screen layout, supplied from `screen+0x30`; description targets this |
| `+0x10` | row layout |
| `+0x18` | toggle button object resolved from `Pa_Btn_00` |
| `+0x20` | choice button object resolved from `Pa_Btn_01` |
| `+0x28` | ordinary button object resolved from `Pa_Btn_02` |
| `+0x30/+0x38` | left arrow Decide/Invalid animation objects |
| `+0x40/+0x48` | right arrow Decide/Invalid animation objects |
| `+0x50` | Entry index, not Entry pointer |
| `+0x54` | bool/check value (byte) |
| `+0x58/+0x5c/+0x60` | current/max/min integer values |
| `+0x64` | ordinary button state byte |
| `+0x68` | game flag type |
| `+0x70` | embedded scroll item, initialized by `0x7000d66ec8` |
| `+0x80` | row root pane, loaded from `[row_layout+0x18]` |

A button's `+0x20` points to its layout; that layout's `+0x18` is the root pane.
`0x7000e30d30` calls button vtable `+0x60` with 1/0 for the active/inactive style,
then sets/clears **bit 0 of root pane byte `+0x58`**. Thus setting visibility alone
is insufficient: the native control also has an enable state.
`0x7000f570ec(button, bool)` stores byte `button+0x89` and drives its Check animation
at `button+0x68`; this is check/value appearance, not horizontal focus.

`runtime/pc_cursor.c` independently uses button `+0x60` as hit pane, pane `+0x50/54`
as width/height and pane matrix `+0x70..+0x8c` for transformed bounds. Its pane walk
uses name `+0xb8`, child-list head `+0x20`, child link at pane `+8`. Binary
`0x7000e44b64..0x7000e44b70` also derives a part name from root pane `+0xb8`.
No direct TextBox string-buffer offset is established: use the game's text setter.

### Exact archive and panes

Parsed files under `extracted/romfs`:

```text
Pack/Bootup.pack
  Layout/Common.sblarc                      Yaz0 -> SARC, 33,032,328 bytes
    blyt/OptionWindow_00.bflyt                5,112 bytes
    blyt/PaOptionSlideBtn_00.bflyt              948 bytes
    blyt/PaOptionBtnL_00.bflyt                6,288 bytes (toggle)
    blyt/PaOptionBtnLevel_00.bflyt            5,132 bytes (choice)
    blyt/PaOptionBtn_00.bflyt                 4,452 bytes (ordinary button)
    blyt/PaNumberArrow_00.bflyt               referenced by choice arrows
    anim/OptionWindow_00_{In,Out}.bflan
    anim/PaOptionBtn{,L,Level}_00_*.bflan
    timg/__Combined.bntx                     29,192,328 bytes
Pack/Bootup_USen.pack
  Message/Msg_USen.product.ssarc             Yaz0 -> SARC
    LayoutMsg/OptionWindow_00.msbt           3,440 bytes
```

Common.sblarc compressed size is 6,348,559; compressed SHA-256 is
`a101cc8bced46aaa823bc3e62d5f949af432166ff3b55a0c644cd5c6aef835a5`.
The supplied top-level `Layout/` contains BootUp and Horse archives, not Options.
The screen is in **Bootup.pack's Common.sblarc**, not a loose Option pack.
All 16 supplied `Bootup_<language>.pack` files contain their Message archive and
no `Layout/` member. Geometry is shared; localized messages are separate.

Screen hierarchy, omitting material/user-data sections:

```text
RootPane (1280x720)
  N_Capture_00 / P_Capture_00
  N_Option_00 (y=10)
    N_SlideArea_00 (640x360)
      Pa_SlideBtn_00 -> PaOptionSlideBtn_00
  P_CaptureMask_00
  N_Option_01
    T_OptionTitle_00 (x=-4,y=270; 320x34; Normal_00)
    T_Caption_00 (y=-280; 598.5x22; Caption_00)
    N_Align_00
      Pa_GuideY_00, Pa_GuideB_00 -> PaGuideKeyNormal_00
```

The complete pane list of the **row part** `PaOptionSlideBtn_00` is:

| Section offset in BFLYT | Type/name | Geometry / reference |
| --- | --- | --- |
| `0xa4` | pan1 RootPane | 1280x720 |
| `0x100` | txt1 T_Option_00 | x=-14, 246x24, Caption_00 |
| `0x1d8` | prt1 Pa_Btn_00 | x=130, PaOptionBtnL_00 |
| `0x248` | prt1 Pa_Btn_01 | x=130, PaOptionBtnLevel_00 |
| `0x2e4` | prt1 Pa_Btn_02 | x=130, PaOptionBtn_00 |

There is **no description pane inside the row**. Selection writes Entry.desc to
screen `T_Caption_00` in `0x7000e30550`, call at `0x7000e305b0`.
All three button parts overlap at the same x/y; showing two without moving them
will superimpose their text and backgrounds.

Nested button pane inventory (shared names occur once per listed branch):

- All three: RootPane, N_Cursor_00, W_Base_02; N_Disable_00 containing B_Hit_00,
  W_Base_00, W_BaseLine_02, W_Decide_00, N_Loop_01/Window_00, N_Content_00;
  N_Disable_01 containing W_Base_01 and W_BaseLine_01.
- Ordinary: N_Content_00/T_Text_00; N_Disable_01/T_Text_01.
- Choice: N_Content_00/T_Text_00, Pa_ArrowR_00 and Pa_ArrowL_00 (each a
  PaNumberArrow_00 part); N_Disable_01/T_Text_01, P_Arrow_00, P_Arrow_01.
- Toggle: N_Content_00/N_Content_01/{W_CheckL_00,W_CheckR_00}, T_BtnON_00,
  T_BtnOFF_00; N_Disable_01/N_Content_02/{W_CheckL_01,W_CheckR_01},
  T_BtnON_01, T_BtnOFF_01.

These `_01` text copies are disabled-state counterparts, not spare binding controls.
`Disable.bflan` animates N_Disable_00/01; toggle `Check.bflan` targets ON/OFF text
and check-window panes. Each button layout declares **one** `UniteButton` controller
with **one** `B_Hit_00` and one `N_Cursor_00`, not a controller per label.
Button text font is `NormalS_00.bffnt`; toggle text boxes are only 120 and 80 units
wide (x=-48 and x=84); ordinary text is 240, choice text 200 units wide.
Native button hit regions are 276x40. Two full-width buttons need more horizontal
space than the current 540-unit row background; widening/repositioning is real work.

Byte checks: every parsed BFLYT begins `46 4c 59 54 ff fe 14 00 00 00 00 08`:
FLYT, little-endian BOM, 0x14 header, version word 0x08000000. Section-relative
pane name is `+0x0c`, translation `+0x2c`, size `+0x4c/+0x50`;
these are **file offsets**, not runtime pane offsets. Header `+0xc` is file size,
`+0x10` section count. A patch must also preserve parts, materials and animation groups.

## 2. Evaluate the second-button approaches

| Approach | Feasibility | Relative effort / risk |
| --- | --- | --- |
| a. Reveal/reposition two existing styles | Yes, structurally verified; full interaction proposed | Medium-large / medium-high: routing, geometry, focus |
| a. Use disabled text copies as buttons | No independent controls; requires custom interaction | Medium / visual-state breakage |
| b. Choice row alone | No simultaneous two bindings; only one current value | Small for a device selector, but misses requested layout |
| b. Toggle halves | Yes as two labels plus custom half selection; not two native controls | Medium / limited width, single cursor/hit box |
| c. Two Entries at same y | No stock support; possible with layout/scroll/navigation replacement | Large / high: duplicate titles, focus and scroll items |
| c. Per-row sub-layout / duplicate native part | Yes with native cloning/registration | Large / high: ownership, callbacks and resource capacity |
| d. In-memory BFLYT/archive patch | Yes; this repo already does it for HUD layouts | Medium for geometry/headers, large with new controls |

### a. Existing controls and direct pane access

Construction resolves Pa_Btn_00/01/02 at `0x7000e30bb0`, `0x7000e30bc4`,
`0x7000e30bd8`; stores objects at row `+0x18/+0x20/+0x28`. No allocation of a
new button is needed to experiment with the choice and ordinary styles together.
After row init's redraw call `0x7000e2f550`, `x19` still identifies the row;
`0x7000e2f554` is a concrete post-init hook location. Subsequent redraw entry
`0x7000e2eed8` also needs custom binding handling, otherwise it updates only one style.

Set texts with `0x7000e30ee0(layout, SafeString* pane_name, message_id)` so existing
`rt_opt_msg` supplies stable UTF-16 strings, or use `0x7000e84940` with a supplied
buffer. Target each button's own layout and T_Text_00/01 explicitly. Do not assume
changing one disabled/normal copy changes the other. SafeString needs the game's
valid vptr, as existing Entry/message hooks already use.

Enable both controls through the existing button operation and root visibility;
hide the choice arrow parts and disabled-state arrow pictures, including animation
side effects. Their row offsets `+0x30..+0x48` are animation handles, not hit panes.
**Inferred:** two registered controls can retain native cursor/Decide animations,
provided screen callbacks and geometry are fixed. Simply making them visible is not enough.

### b. What toggle/choice input actually does

Redraw's toggle branch `0x7000e2ef30..0x7000e2ef4c` calls
`0x7000f570ec(row.toggle, row.bool)`; it never reads Entry.button_off/on.
Only the ordinary-button branch fetches Entry `+0x24` or `+0x28` and writes
T_Text_00 (`0x7000e2f064` and `0x7000e2efb0`). Thus a distinct `rt_opt_msg` ID
per field still updates one ordinary button, depending on row+0x64.
Toggle relabeling must explicitly write T_BtnON_00/01 and T_BtnOFF_00/01.

Screen `+0x36c0` is an action code, **not a persistent selected column**:
0 confirm, 1 left, 2 right, 3 idle. Screen `+0x36c8` is current row. Update resets
the code to 3 at `0x7000e2f628`; confirm writes 0 at `0x7000e2f9c8`.
Left writes 1 at `0x7000e2fa50`; right writes 2 at `0x7000e2fae4`.
Input tests use screen-input bytes `+0xa/+0x12`, masks `0x44/0x88`; these cover the
shared horizontal repeat paths. Exact stick-versus-d-pad bit provenance was not
traced through the input producer; no separate persistent half-focus was found here.

Row decide `0x7000e30700` handles toggle as follows:

- Confirm: load bool at `0x7000e30768`, XOR 1 at `0x7000e30770`, update current value.
- Left/code 1: increment/clamp value at `0x7000e30a34`; right/code 2:
  decrement/clamp at `0x7000e30a14`; write bool = value != 0 at `0x7000e30a5c`.
- ON is the left label; OFF is right. This changes the option immediately; there
  is no stock “move between two bind buttons, then A activates that half” contract.

A toggle prototype could intercept this before mutation: directions select a
side without saving settings; confirm captures that side. It must preserve that
side across `rt_opt_read`, drive check appearance separately and handle mouse halves.
Current `rt_opt_decide` rejects every kind except KIND_BUTTON, and current capture
uses BIND_DEV_ANY. Both must change; left/right must never start capture.
`rt_opt_store`/`rt_opt_reset` must not call ordinary toggle get/set callbacks on a
binding pseudo-toggle. `pc_cursor.c:options_click` currently suppresses clicking the
already-checked half, so relabeling alone cannot make it a binding editor.

Choice has one value T_Text_00 between arrows; Entry.first_value + row.value selects
its message (`0x7000e2effc..0x7000e2f00c`). Arrows change that value; they are not two
text-bearing binding buttons. Reusing the whole choice control as one of two controls
is different from using its arrows as the two bindings.

### c/d. Layouts, Entries and load-time patching

Two Entries normally mean two cloned row layouts and two scroll items. Merely
assigning matching y coordinates does not merge focus, description, scrolling or
Entry identity. A new sub-layout can contain two native button parts, but a new pane
alone does not add the Options callbacks needed to route it.

The in-memory path is already implemented in `runtime/services/fs.c`:
`open_image` -> `rom_index` -> `hud_letters_patch` -> `rstb_patch`; reads pass
through `romfs_read`/`storage`, including memory-backed `Overlay.mem` ranges.
`hud_letters_patch` reads Bootup.pack, decompresses Common.sblarc, edits BFLYT files,
appends rebuilt layouts to the SARC, rewrites nodes, recompresses, shifts pack members
by 0x2000-aligned deltas and updates virtual RomFS metadata. It grows the resource
entry **`Layout/Common.blarc`**, then `rstb_patch` adjusts ResourceSizeTable sizes.
An Options patch can compose into this same pack buffer before it is published.
Do not independently replace the pack again and lose the HUD patch or user overlays.

This establishes a hookable path without shipping edited Nintendo assets. Generate
the modified data from the user's archive in memory; keep names, alignment, section
counts, file sizes, node ranges and resource allocation accounting valid. Byte-size
checks and fail-closed handling of unknown archives are needed. Existing code's
user-overlay precedence must be respected. Asset edits alone do not fix input routing.

## 3. Column headers

- T_OptionTitle_00 can carry a page title, but is a single centered 320-unit text
  box at y=270. Spaces or embedded text in it are not two independently aligned small
  column headers. No existing `Item` page-title field addresses this pane.
- T_Caption_00 is the live description; using it loses the selected action's help.
  Pa_GuideY/B are reset/back hints, not unused header panes.
- A non-selectable first row is possible with a special row-init hook: leave its
  text visible, disable all three controls, and supply two labels in separate text
  panes. No stock non-selectable Entry kind was verified. It consumes a scroll item
  and scrolls away; hiding controls must not strand initial focus on it.
- **Recommended:** add two text panes under screen N_Option_01 through the in-memory
  BFLYT transform, using its existing fonts/materials, above the scrolling region.
  Give them unique names, smaller font size, independent column positions, and no
  `cnt1` button controller. Hide outside P_BINDS. Move/shrink the list viewport if
  necessary; do not overlap title or first row. Exact final dimensions need a live
  layout check. This is proposed, not a tested resource patch.

## 4. Implementation plan and remaining limits

1. Add versioned/signature-checked sites for the new functions below to
   `tools/game_sites.py`; expose needed sites through `runtime/game_sites.h`.
   Keep additions guarded to SWR binding rows on P_BINDS. Do not enlarge the native
   0x98 row or use padding as unowned storage; keep row/column state in a sidecar.
2. At post-init `0x7000e2f554` configure row `+0x20` as keyboard/mouse control and
   `+0x28` as controller control. Keep Entry.kind=2 for ordinary read/store behavior.
   Enable both, hide choice arrows, reposition both and the action label. Verify
   runtime transform setters/dirty propagation before writing position fields;
   file offsets listed above are not runtime coordinate addresses. Alternatively
   derive geometry changes in memory at layout load while preserving native pages.
3. Intercept redraw entry `0x7000e2eed8` for these rows: write separate synthetic
   message IDs for key and pad to each T_Text_00/01 using `0x7000e30ee0` and extend
   `rt_opt_msg`. Refresh both when capture ends. Leave ordinary game rows on the
   original path. Handle keyboard-only slots with an inactive controller cell.
4. Extend native callback routing to match either control and remember the column:
   select/help `0x7000e3047c` (vtable +0x340), state callback `0x7000e305d0`
   (+0x350), decide dispatcher `0x7000e30668` (+0x360), and callback
   `0x7000e30a90` (+0x368). Each currently compares only the control selected by
   Entry.kind. Keep help through `0x7000e30550`; adapt `0x7000e302e4`'s per-kind
   state handling to both cells. Exact names of the non-decide callbacks remain inferred.
5. Fix update's active-control comparison `0x7000e2f648..0x7000e2f7d0` and its
   horizontal branches: left/right move native cursor between cells without changing
   bindings; up/down preserve the column. Initial focus `0x7000e2ffec` also chooses
   one control by kind (`0x7000e30130..0x7000e301b4`). Do not expect geometric
   placement alone to fix these hard-coded one-control assumptions.
6. Intercept `0x7000e30700` before its original body: confirm uses remembered column
   and calls `bind_capture_begin(BIND_DEV_KEY/PAD, slot)`, not BIND_DEV_ANY. Preserve
   `rt_opt_tick` refresh/capture cancellation and reset semantics. Update
   `pc_cursor.c:options_row_at/options_focus/options_click` to use both native hit
   regions; it currently selects one by kind and extends its hit area over the title.
7. Compose the header/necessary geometry transform into `hud_letters_patch`'s
   Common archive rebuild in `fs.c`, preserving RSTB accounting and overlay behavior.
   Scope visible changes to P_BINDS; shared layouts also serve original Options.
8. On a machine with dependencies, verify native cursor, hover/Decide sounds, both
   device captures, held/repeated directions, disabled cells, long/localized labels,
   scrolling, reset, cancel, reopening and both pause/title Options entry points.
   Check that inactive/disabled text copies agree and no focus reaches hidden panes.

Static feasibility is strong; the complete navigation contract, geometry setters,
text growth limits, and visual/aural acceptance are not verified. This pass cannot
supply a ready-to-apply UI patch or claim live success. No ImGui overlay, binary,
asset replacement, runtime change or build was produced.

Selected raw instruction checks (little-endian bytes):

| VA | Bytes | Meaning |
| --- | --- | --- |
| `0x7000e2f258` | `00 13 80 52` | mov w0, #0x98 |
| `0x7000e2f28c` | `33 06 00 94` | bl 0x7000e30b58 |
| `0x7000e30b6c` | `04 50 00 b9` | str w4, [x0,#0x50] |
| `0x7000e30c0c` / `0x7000e30c20` / `0x7000e30cc4` | `60 0e 00 f9` / `60 12 00 f9` / `60 16 00 f9` | store toggle/choice/button pointers |
| `0x7000e30d80` | `09 61 01 39` | strb w9, [x8,#0x58], root visibility |
| `0x7000e2efb0` / `0x7000e2f064` | `02 29 40 b9` / `02 25 40 b9` | load Entry on/off ID at +0x28/+0x24 |
| `0x7000e30770` | `2a 01 00 52` | eor w10, w9, #1, confirm toggles value |
| `0x7000e306fc` | `01 00 00 14` | b 0x7000e30700, native decide dispatch |
