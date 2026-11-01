# Per-action bindings

The **Controls** page is a table of the game's actions: one row per thing the game does (Jump, Attack, Cancel...), with a
controller button and a key of their own each. By default every action does what it always did: it follows the button of
the Switch input the game ties it to (Jump follows X, Interact follows A...), so nothing changes until you bind one.
Below the table are the inputs no action covers (sticks, camera, motion, Plus / Minus, the d-pad's quick menus), and,
folded under **Advanced**, the Switch buttons the actions follow. Game version 1.9.0.

- Click a cell, then press the new key or button (Esc or 5 seconds cancels). Mouse buttons work for keys.
- A cell shows its binding in gold. In grey it shows the button of the Switch input the action follows, which is what
  it uses until it is given a binding of its own.
- Right-click a cell: **Use default** (follow the Switch input again) or **Unbind** (nothing presses the action from
  that device).
- Two actions can share a key, and an action can leave its Switch button: nothing swaps. (The Switch table still
  swaps a button that is already in use.)
- **Reset actions to defaults** clears all of them.

An action with no binding of its own is untouched: the game answers from the Switch layer as before. The game's menus
(title, pause, map), a window that is not focused and the overlay menu always use the Switch layer too, so no binding
can lock the player out.

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
| 32 | Climb (no effect) | R | asked while wall climbing; nothing happens |
| 33 | Aim rune | L | |
| 34 | Activate rune | A | fires Stasis, Magnesis, Cryonis; the bombs ignore it |
| 35 | Call horse | d-pad down | |
| 36 | Water spin attack | Y | swimming with the Zora Helm |
| 37 | Throw distance | d-pad down | toggles how far an item in hand is put down |
| 38 | Cycle accelerate | A | Master Cycle Zero |
| 39 | Cycle brake | B | Master Cycle Zero |
| 40 | Cycle jump | L | Master Cycle Zero |

## savagewind.ini

Only the actions that differ from the default are written:

```
act_key_2 = G
act_pad_3 = south
act_pad_9 = none
```

`act_key_<id>` / `act_pad_<id>` take a key or controller button name as `bind_key_*` / `bind_pad_*` do (`none`: no
binding; a missing line: follow). `<id>` is the game's action number, 0 to 40.

## How it works

The game asks about an action in exactly two places: `isHold(pad, id)` (is it held) and `isTrig(pad, id)` (was it
pressed this frame). Each looks the action's pad mask up in a table of 41 and tests it against the pad object's held
or pressed word, which the game fills from the Switch-style state the runtime hands it (runtime/hle/input.c).

`tools/patches.py` `_act_query` puts a call in front of both functions (sites `act_hold`, `act_trig`; the mask table
is `act_masks`; all three are found by instruction patterns in `tools/game_sites.py`, an optional group like the
others). `rt_act_query` (runtime/hle/bindings.c) answers for an action with a binding of its own and declines for every
other, in which case the game's own code runs.

An answered action reads the host: the keyboard side from the keys held, the controller side from the buttons held. A
side left at the default reads the Switch input the game's table assigns the action, as it did before. The main thread
publishes which actions each side holds on every key or button event; the game thread freezes that once per
controller poll (`act_poll`, in `fill_state`), and "pressed" is the edge between two polls. A press nobody asked about
before the next poll is kept for the next query, so a short tap is not lost.

## Known limits

- An action with a binding of its own ignores the game's own **Jump button** option (it swaps the A and X reads for
  Jump and Interact); bind what you want directly.
- The button icons the game draws follow the Switch layer and the controller family, not the per-action bindings.
- The 13 ids the game never asks about (1, 4, 6, 7, 8, 16, 20 to 24, 29, 30) are not on the page. No id above 40 is used:
  all 175 query sites (157 calls and 18 tail branches, in 91 functions) pass a constant from 0 to 40.

## Naming the actions (logging build)

`SAVAGEWIND_ACTLOG=1` makes the runtime print which action ids the game asks about, from where (the caller's address
and the pad object), and, once per poll, which actions the game's own pad word says are true. With a script that
presses one Switch button at a time the log says which actions each button drives, which names the actions and checks
the runtime's mask-to-button table (the D-pad's up / down are not verified yet).

```
python3 tools/actlog_script.py --state stand        # then --state run, --state aim
SAVAGEWIND_ACTLOG=1 SAVAGEWIND_INPUT="$(python3 tools/actlog_script.py --start 3000 --state stand 2>/dev/null)" \
    SAVAGEWIND_EXIT_AT=5000 <the game's executable>
```

`--start` must fall after the save has loaded and Link stands in the open (the scripted frames count from the boot);
`actlog_script.py` prints the frame of each press on stderr.

## Tests

`make test-actions` (no game files): the answers to the hold / pressed queries, menus falling back, two actions on one
key, capture of an action slot, the ini round trip (`tools/test_actions.c`), and the generated hook
(`tools/test_act_site.py`). `tools/test_game_sites.py` covers the site patterns.
