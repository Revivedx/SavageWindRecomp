# PC mouse controls

Mouse pointer controls for menus, inventory, quick equipment and map are enabled by default. Open the executable directly; no custom launcher or environment variables are needed. The native mouse camera already exists upstream and is not reintroduced here. Virtual gyro changes remain separate.

## Options

In the game's **System > Options > Controls** (also in the configuration UI):

- **PC mouse pointer**: hover/click menus, inventory and map. With it on, **Esc** opens pause or goes back and **F2** frees the cursor. Click once inside the window to take it back; that click is consumed. With it off, Esc retains upstream cursor-release behavior.

The switch applies immediately and is saved as `pc_cursor` in `savagewind.ini`. Existing files without the key default to on. `SAVAGEWIND_PC_CURSOR=0/1` is an optional startup override; Controls can change it during the session.

Bow controls are outside this contribution. The normal bindings remain: F for the bow, right mouse for shield/lock-on, and left mouse for attack.

## Pointer behavior

In native Options pages, the wheel scrolls the view without generating d-pad
input. Hover activates the native row highlight and its localized help text; left-click operates its
toggle, button or choice arrow. Hover does not recenter the scroll view. These
controls use **PC mouse pointer**, including immediate disable and reboot reset.
Mouse and keyboard/controller share one native highlight and help text. Options does not draw a separate mouse highlight. Keyboard/controller navigation retains focus until the mouse moves, clicks or scrolls again, including when Options captures the game button state. Rebuilt pages start at the top, so returning from a long key-binding list cannot carry its scroll offset into the parent menu.

Inventory categories support clicking their icons, wheel navigation and horizontal dragging. Quick equipment supports wheel and drag selection, retaining the selected item when its selector closes. The current `test` defaults remain **1 weapons / 2 shields / 3 runes / 4 whistle**. Saved custom bindings are not overwritten.

Adventure Log and Ability Controls hover select through their registered native row lists, keeping their details and shared highlight synchronized. The outer category list is kept separate from its nested row lists. Hover uses cursor selection rather than the scroll-page selection function, so it does not rebuild or recenter the view. A stationary mouse retains focus until keyboard/controller input takes over; new mouse input takes it back. Clicking a category icon switches the native list while retaining disabled/locked categories. Hover preserves the viewport and does not repeatedly select the same quest. Cursor hit-testing clips visible bounds without calling the native clipping callback, which clears focus for offscreen rows. Category hit areas use their 32 px cursor panes rather than their overlapping 211 px containing layouts. Clicking the upper Adventure Log / Inventory / System labels uses the normal section transition, with a hover frame and pressed feedback; modal submenus retain input priority.

Map navigation supports wheel zoom, left-drag panning, WASD and progressive edge scrolling. Marker hover has a small attraction radius without recentering the map. Stamp placement stays anchored while choosing an icon, and Edit/Delete/Cancel accepts pointer input. Coordinates account for resolution, aspect ratio and letterboxing.

The system cursor is hidden while keyboard/controller navigation owns menu focus, and over the map canvas while the game pointer is active, without enabling relative mouse mode there. It is restored for host UI, cursor release, loss of focus and when pointer mode is off. Controllers and existing bindings remain available.

The specialized map/inventory adapters target BOTW 1.9.0. Optional generator sites validate instruction prologues; missing groups disable their adapters instead of guessing addresses. Reboot clears cached guest pointers and gesture state.

Dialogue pointer input uses the shared conversation screen: left-click advances text when no responses are selectable, while response clicks use their native cursor channel. It also advances shop dialogue while the background item screen remains allocated.

Shop purchase lists and selling grids share native list selection, so the item highlight and details follow hover instead of returning to the old keyboard selection. Quantity entry uses the game's existing number and arrow panes: click minus/plus, hold to repeat, or click the number to type. Hover highlights the available arrows and number; number editing has a blinking caret. Native menu cues provide hover and activation feedback. Typed values and held arrows clamp to the dialog's native minimum and maximum without confirming a transaction. An additional affordability cap derived from rupees has not been implemented or verified.

Dragging horizontally over Link's inventory preview adjusts its native yaw targets without modifying the actor's translation or entering the Rotate mode that hides inventory information. The gesture retains its original owner, so crossing into the item grid cannot start a page drag. Inventory category clicks refill the destination page cache directly rather than stepping through intermediate categories. Header and category buttons use native menu hover/confirmation sounds. The map pointer is confined to the map edge zone and released in other screens or host UI.

## Validation

Updated onto `test` at `232bb49`, retaining upstream changes. The Windows executable builds. Automated coverage includes:

- `make test-pc-controls`: actual host cursor-update transitions (including map entry while relative mode is already off), focus, overlay/F2 policy, number-key defaults, options persistence across boots and startup overrides.
- `tests/test_options_pointer.c`: synthetic Options viewport scrolling without d-pad/selection changes, scroll limits, transformed hover rectangles, direct row actions, native focus activation without auto-scrolling, input ownership, disabled pointer behavior and reboot reset. The contributor also checked Options hit detection, wheel scrolling, help text, keyboard/mouse focus transfer and returning from binding subpages in game.
- `tests/test_cursor_math.c`: seven resolutions/aspect ratios, letterboxing, 1,200 pan/zoom samples, time-based WASD/edge navigation.
- `tools/test_game_sites.py`: 45 checks.

The contributor tested the final local Windows/BOTW 1.9.0 revision in game, covering Adventure Log and Ability Controls hover/category selection, shared input focus, upper section buttons and sounds, inventory categories and Link rotation, NPC dialogue and responses, shop hover and dialogue, quantity typing/held arrows/feedback, and map navigation/stamps. The maintainer previously tested the earlier menu, Options, inventory and map revision on Windows. The bow override has been removed: ordinary gameplay bindings remain unchanged.

Synthetic host tests also cover stationary list hover, the derived purchase-list type, locked categories, viewport preservation, quantity repeat timing/limits/release, native preview yaw changes without actor translation or UI-mode changes, and reset behavior. These tests supplement gameplay checks; Steam Deck and broader controller testing remain pending.

```sh
make -j8
make test-pc-controls
clang tests/test_cursor_math.c -o build/test_cursor_math
build/test_cursor_math
python tools/test_game_sites.py
```

Developed with Codex assistance.
