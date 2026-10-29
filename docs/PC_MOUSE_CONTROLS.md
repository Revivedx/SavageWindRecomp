# PC mouse controls

Mouse pointer controls for menus, inventory, quick equipment and map, and right-mouse bow aiming are enabled by default. Open the executable directly; no custom launcher or environment variables are needed. The native mouse camera already exists upstream and is not reintroduced here. Virtual gyro changes remain separate.

## Options

In the game's **System > Options > Controls** (also in the configuration UI):

- **PC mouse pointer**: hover/click menus, inventory and map. With it on, **Esc** opens pause or goes back and **F2** frees the cursor. Click once inside the window to take it back; that click is consumed. With it off, Esc retains upstream cursor-release behavior.
- **Mouse bow aiming**: hold right mouse to draw/aim, then left-click to fire. Off restores normal mouse bindings. Disabling it or freeing the cursor safely cancels an arrow already being drawn.

Changes apply immediately and are saved as `pc_cursor` and `pc_bow` in `savagewind.ini`. Existing files without these keys keep both defaults on. `SAVAGEWIND_PC_CURSOR=0/1` and `SAVAGEWIND_PC_BOW=0/1` are optional startup overrides; the Controls switches can still change their values during that session. Remove the overrides to resume using the saved choices on the next boot.

## Pointer behavior

In native Options pages, the wheel scrolls the view without generating d-pad
input. Hover activates the native row highlight and its localized help text; left-click operates its
toggle, button or choice arrow. Hover does not recenter the scroll view. These
controls use **PC mouse pointer**, including immediate disable and reboot reset.
Mouse and keyboard/controller share one native highlight and help text. Options does not draw a separate mouse highlight. Keyboard/controller navigation retains focus until the mouse moves, clicks or scrolls again, including when Options captures the game button state. Rebuilt pages start at the top, so returning from a long key-binding list cannot carry its scroll offset into the parent menu.

Inventory categories support clicking their icons, wheel navigation and horizontal dragging. Quick equipment supports wheel and drag selection, retaining the selected item when its selector closes. The current `test` defaults remain **1 weapons / 2 shields / 3 runes / 4 whistle**. Saved custom bindings are not overwritten.

Map navigation supports wheel zoom, left-drag panning, WASD and progressive edge scrolling. Marker hover has a small attraction radius without recentering the map. Stamp placement stays anchored while choosing an icon, and Edit/Delete/Cancel accepts pointer input. Coordinates account for resolution, aspect ratio and letterboxing.

The system cursor is hidden over the map canvas while the game pointer is active, without enabling relative mouse mode there. It is restored for host UI, cursor release, loss of focus and when pointer mode is off. Controllers and existing bindings remain available.

The specialized map/inventory adapters target BOTW 1.9.0. Optional generator sites validate instruction prologues; missing groups disable their adapters instead of guessing addresses. Reboot clears cached guest pointers and gesture state.

## Validation

Updated onto `test` at `4249c88`, retaining upstream changes. The Windows executable builds. Automated coverage includes:

- `make test-pc-controls`: actual host cursor-update transitions (including map entry while relative mode is already off), focus, overlay/F2 policy, safe bow cancellation, number-key defaults, options persistence across boots and startup overrides.
- `tests/test_options_pointer.c`: synthetic Options viewport scrolling without d-pad/selection changes, scroll limits, transformed hover rectangles, direct row actions, native focus activation without auto-scrolling, input ownership, disabled pointer behavior and reboot reset. The contributor also checked Options hit detection, wheel scrolling, help text, keyboard/mouse focus transfer and returning from binding subpages in game.
- `tests/test_bow.c`: draw, shot, redraw and cancellation at 30/60/120 FPS.
- `tests/test_cursor_math.c`: seven resolutions/aspect ratios, letterboxing, 1,200 pan/zoom samples, time-based WASD/edge navigation.
- `tools/test_game_sites.py`: 45 checks.

The contributor tested this revision in a separate Windows build based on `test` at `4249c88`, at 1920 x 1080 on an NVIDIA RTX 3080 Ti. The local playable build also contains the separate virtual-gyro contribution; that code is not included in this PR. The executable opens directly, with no launcher script or environment overrides required. The contributor confirmed native Options click actions, viewport wheel scrolling, one shared highlight and help text, keyboard/mouse focus handoff, and returning from long keyboard-binding pages. Earlier gameplay checks covered menus, inventory, quick equipment, bow controls, map navigation and stamps, including 4K. The PC-pointer-only executable also builds and passes the host tests. Steam Deck and broader controller testing remain pending.

```sh
make -j8
make test-pc-controls
clang tests/test_bow.c -o build/test_bow
build/test_bow
clang tests/test_cursor_math.c -o build/test_cursor_math
build/test_cursor_math
python tools/test_game_sites.py
```

Developed with Codex assistance.
