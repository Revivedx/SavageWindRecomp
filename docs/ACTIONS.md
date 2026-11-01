# Per-action bindings

Each game action owns one keyboard/mouse binding and one controller binding. Its factory defaults are copied from
its Switch button's original layout using the fixed 1.9.0 mask table below: Jump starts on Space / North, Interact
on E / East, and so on. If either device differs from its factory default, the action reads its own host bindings on
both devices, independent of raw Switch rebinds. With both devices at factory defaults, the game answers through the
Switch layer as before, preserving its Jump button change option and raw Switch injection. Multiple actions may share
an input. Reset restores each action's factory defaults. `ACT_FOLLOW` remains a reset command accepted by `act_set`;
it is no longer stored in `g_act_key` or `g_act_pad`. `ACT_NONE` unbinds an action side.

Movement and camera directions keep their keyboard-only slots and defaults (W S A D / I K J L). Walk, Tilt and all
seven gyro slots accept both devices; their new controller sides default to unbound (`-1`). Walk scales its device's
left stick to 40%. Tilt holds the mouse clutch and enables the existing sensorless controller stick tilt path. Gyro
buttons and keys combine per direction, only while `gyro_keys_live()`; simultaneous opposite directions cancel.
Walk, Tilt, gyro and game action bindings allow shared inputs on both devices. Assigning one never changes another
slot, including raw Switch menu buttons; rebinding a raw slot also leaves these shared bindings intact. Legacy
host slots still swap among themselves on the same device, including the raw Switch buttons (0 to 15).

The game's title, pause and map menus still use the raw Switch layer, including legacy raw-button rebinds. The runtime
overlay and launcher keep their fixed navigation shortcuts. The separate UI update can use `bind_input_name`,
and `bind_input_group` ("gyro" or NULL). Raw Switch inputs are named by what they do (`bind_input_name(BIND_A)` is
"Menu: confirm"), never by the button. No slots are hidden by these helpers; the page and overlay simply do not list
left stick press and D-pad Down (their functions are the actions Crouch, Call horse and Throw distance). Use `bind_pad_slot` for controller availability and `bind_is_default_slot` for defaults, including
`BIND_ACTION + id`; do not test for a stored `ACT_FOLLOW`.

## The Key bindings page

The game's own Options has a **Key bindings** page. Every row is one input: its name on the left, a keyboard/mouse
button in the middle and a controller button on the right, under a column header row (Keyboard/Mouse, Controller).
Both buttons are real native buttons of the game's Options row, not an overlay. Left/right switches the column,
up/down keeps it, and the mouse hovers and clicks each button on its own. Confirming a button waits for the next press:
a key or mouse button binds the keyboard side, a controller button the controller side (Esc cancels, as does waiting 5
seconds). The hovered button is lit; the other one sits under a darker band, so only one looks selected.

The rows are the 27 actions below, then the menu inputs (see "Menu inputs" below), then Move forward/back/left/right and Camera up/down/left/right (keyboard only: the
controller cell reads "Stick" and is greyed), Walk, Tilt and the seven gyro keys (both devices). The F1 overlay's Controls
page shows the same table from the same in-process state; the launcher is a separate process.

The two-button rows hook the game's Options code through one strict optional signature group
(`tools/game_sites.py`, tested by `tools/test_two_button_sites.py`). If any signature does not match exactly, the group is
off and the page falls back to the single-button rows. `SAVAGEWIND_TWO_BUTTON_ROWS=0` turns them off by hand. The layout
numbers live in `runtime/ui/binding_row_layout.h`; the research notes are in `docs/research/`.

## Menu inputs

No raw Switch button is listed on the page or in the overlay. The functions the Switch layer carries beyond the 41
actions are rows of their own, bound to the same raw slots with the same defaults (ini names unchanged):

| Row | Slot | Default (Switch button) | What it does |
|---|---|---|---|
| Pause menu | Plus | Plus | opens the pause menu |
| Map (Sheikah Slate) | Minus | Minus | opens the map |
| Quick menu: runes | D-pad up | D-pad up | hold for the rune quick menu |
| Quick menu: shields | D-pad left | D-pad left | shield quick menu |
| Quick menu: weapons | D-pad right | D-pad right | weapon quick menu |
| Menu: confirm | A | A | selects in the game's menus |
| Menu: back | B | B | goes back |
| Menu: Hero's Path | X | X | Hero's Path in the map |
| Menu: sort / sensor | Y | Y | sorts the inventory, Sheikah Sensor in the map |
| Menu: previous / next tab | L / R | L / R | switches the menu tab |
| Menu: previous / next tab 2 | ZL / ZR | ZL / ZR | the same, in the menus that use the triggers |
| Menu: inspect / zoom | right stick click | right stick click | inspect an item, zoom the map |

Left stick click (Crouch, a menu use) and D-pad down (Call horse, Throw distance) have no function of their own beyond
the actions, so they are not listed; they keep their slots and defaults. Two greyed rows, Esc and F2, are fixed
and cannot be rebound: Esc is Plus in play and B in menus, F2 frees the mouse cursor. F1 opens the Savage Wind overlay
(controller: Back + Start or Home / Guide) only when the game is started with `SAVAGEWIND_OVERLAY=1`; that overlay's
own page lists it greyed too.

## The actions

| Id | Action | Default (Switch button) | Notes |
|---|---|---|---|
| 0 | Interact | A | |
| 2 | Jump | X | |
| 3 | Attack | Y | |
| 5 | Shield bash | A | with the shield up |
| 9 | Lower shield | B | |
| 10 | Crouch step | X | crouching, stick forward: a dash forward |
| 11 | Dodge | X | side hop / backflip while locked on |
| 12 | Sprint | B | |
| 13 | Guard / lock on | ZL | |
| 14 | Cancel | B | |
| 15 | Scope | right stick click | the Sheikah Slate's spyglass |
| 17 | Crouch | left stick click | |
| 18 | Bow | ZR | |
| 19 | Paraglider | X | |
| 25 | Dismount | B | |
| 26 | Speed up horse | A | |
| 27 | Pet horse | L | |
| 28 | Throw weapon | R | |
| 31 | Take photo | R | with the Camera rune's view open |
| 32 | (hidden) | R | asked while wall climbing; nothing happens, so it has no row |
| 33 | Aim rune | L | |
| 34 | Activate rune | A | fires Stasis, Magnesis, Cryonis; the bombs ignore it |
| 35 | Call horse | d-pad down | |
| 36 | Water spin attack | Y | swimming with the Zora Helm |
| 37 | Throw distance | d-pad down | toggles how far an item in hand is put down |
| 38 | Cycle accelerate | A | Master Cycle Zero |
| 39 | Cycle brake | B | Master Cycle Zero |
| 40 | Cycle jump | L | Master Cycle Zero |

## savagewind.ini

The schema marker is written before all action and raw-button bindings, followed by factory-default differences:

```
act_schema = 2
act_key_2 = G
act_pad_3 = south
act_pad_9 = none
```

`act_key_<id>` / `act_pad_<id>` take a key or controller button name as `bind_key_*` / `bind_pad_*` do (`none`: no
binding). `<id>` is the game's action number, 0 to 40. A missing action/device line starts from its factory default;
when loading a legacy file without `act_schema = 2`, `bind_key_<button>` and `bind_pad_<button>` lines migrate into
bindings for actions reading that button unless an explicit action line exists on that device. Explicit lines win
in either file order. Migration uses the
fixed 1.9.0 table, never the current game table. On the next save, `act_schema = 2` disables that migration for future
loads. This preserves omitted factory-default action values even when raw buttons differ. Resetting actions does not
clear the schema flag. Runtime raw-button edits do not perform migration.

Added controller keys: `bind_pad_walk`, `bind_pad_tilt`, `bind_pad_gyro_up`, `bind_pad_gyro_down`, `bind_pad_gyro_left`,
`bind_pad_gyro_right`, `bind_pad_gyro_turn_left`, `bind_pad_gyro_turn_right`, `bind_pad_gyro_center`. These and existing
raw controller bindings accept `none`. All existing keyboard, action and raw-button ini names remain unchanged.

## How it works

The game asks about an action in exactly two places: `isHold(pad, id)` (is it held) and `isTrig(pad, id)` (was it
pressed this frame). Each looks the action's pad mask up in a table of 41 and tests it against the pad object's held
or pressed word, which the game fills from the Switch-style state the runtime hands it (runtime/hle/input.c).

`tools/patches.py` `_act_query` puts a call in front of both functions (sites `act_hold`, `act_trig`; the mask table
is `act_masks`; all three are found by instruction patterns in `tools/game_sites.py`, an optional group like the
others). `rt_act_query` (runtime/hle/bindings.c) answers customized actions during gameplay. It declines for actions
at factory defaults, in menus, when the window is unfocused or the runtime overlay is open, and for invalid action ids.
An atomic custom-action mask is refreshed after edits, resets and migration; only those actions are scanned and polled.

An answered action reads host keys/mouse buttons and controller buttons directly. A device left at its factory default
uses that default host input when the other device is customized. The main thread publishes which actions each side
holds on every key or button event; the game thread freezes that once per controller poll (`act_poll`, in `fill_state`), and "pressed" is the edge between two polls. A press nobody asked about
before the next poll is kept for the next query, so a short tap is not lost.

## Known limits

- Customized actions bypass the game's own **Jump button** option; bind those actions directly. Factory-default
  actions retain the game's option.
- Raw Switch injection (`SAVAGEWIND_INPUT` or a control-endpoint button) reaches factory-default actions through the
  game, but customized actions require host key/controller injection.
- The button icons the game draws follow the Switch layer and the controller family, not the per-action bindings.
- The 13 ids the game never asks about (1, 4, 6, 7, 8, 16, 20 to 24, 29, 30) are not on the page. No id above 40 is used:
  all 175 query sites (157 calls and 18 tail branches, in 91 functions) pass a constant from 0 to 40.

## Naming the actions (logging build)

`SAVAGEWIND_ACTLOG=1` prints the queried action ids, callers and host-action answers. Use host input to exercise
these bindings (frame numbers must be after the save has loaded). For example:

```
SAVAGEWIND_ACTLOG=1 SAVAGEWIND_HOST_INPUT="3000:key:Space:60" SAVAGEWIND_EXIT_AT=5000 <the game's executable>
```

`tools/actlog_script.py` emits raw Switch injection for calibrating the game table with factory-default actions.
Use host injection for customized actions.

## Tests

`make test-two-button-rows` runs the Options row hooks, column routing, header geometry and capture engine against
synthetic rows (needs no game files).

`make test-actions` (no game files): the answers to the hold / pressed queries, menus falling back, two actions on one
key, factory-default fallthrough, customization on either device, legacy migration in either order, one-time schema
migration and reload idempotence, shared action/Walk/Tilt/gyro captures on both devices, preserved raw Switch swaps,
reset and ini round trips
(`tools/test_actions.c`), and the generated hook (`tools/test_act_site.py`).
`make test-pc-controls` also checks Walk scaling, shared key/pad Tilt, mouse routing, sensorless dual-stick eligibility,
and all gyro directions with menu/focus/context gates. `tools/test_game_sites.py` covers the site patterns.
