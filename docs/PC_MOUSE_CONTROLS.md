# PC mouse controls

Mouse pointer controls for menus, inventory, quick equipment and map, and right-mouse bow aiming are enabled by default. Open the executable directly; no custom launcher or environment variables are needed. The native mouse camera already exists upstream and is not reintroduced here. Virtual gyro changes remain separate.

## Options

In the game's **System > Options > Controls** (also in the configuration UI):

- **PC mouse pointer**: hover/click menus, inventory and map. With it on, **Esc** opens pause or goes back and **F2** frees the cursor. Click once inside the window to take it back; that click is consumed. With it off, Esc retains upstream cursor-release behavior.
- **Mouse bow aiming**: hold right mouse to draw/aim, then left-click to fire. Off restores normal mouse bindings. Disabling it or freeing the cursor safely cancels an arrow already being drawn.

Changes apply immediately and are saved as `pc_cursor` and `pc_bow` in `savagewind.ini`. Existing files without these keys keep both defaults on. `SAVAGEWIND_PC_CURSOR=0/1` and `SAVAGEWIND_PC_BOW=0/1` are optional startup overrides; the Controls switches can still change their values during that session. Remove the overrides to resume using the saved choices on the next boot.

## Pointer behavior

Inventory categories support clicking their icons, wheel navigation and horizontal dragging. Quick equipment supports wheel and drag selection, retaining the selected item when its selector closes. The current `test` defaults remain **1 weapons / 2 shields / 3 runes / 4 whistle**. Saved custom bindings are not overwritten.

Map navigation supports wheel zoom, left-drag panning, WASD and progressive edge scrolling. Marker hover has a small attraction radius without recentering the map. Stamp placement stays anchored while choosing an icon, and Edit/Delete/Cancel accepts pointer input. Coordinates account for resolution, aspect ratio and letterboxing.

The system cursor is hidden over the map canvas while the game pointer is active, without enabling relative mouse mode there. It is restored for host UI, cursor release, loss of focus and when pointer mode is off. Controllers and existing bindings remain available.

The specialized map/inventory adapters target BOTW 1.9.0. Optional generator sites validate instruction prologues; missing groups disable their adapters instead of guessing addresses. Reboot clears cached guest pointers and gesture state.

## Validation

Rebased onto `test` at `79ede35528fce6d251acf2c47a11d5598c2c2911`, retaining color-grading includes and the upstream quick-selector fix/defaults. The Windows executable builds. Automated coverage includes:

- `make test-pc-controls`: actual host cursor-update transitions (including map entry while relative mode is already off), focus, overlay/F2 policy, safe bow cancellation, number-key defaults, options persistence across boots and startup overrides.
- `tests/test_bow.c`: draw, shot, redraw and cancellation at 30/60/120 FPS.
- `tests/test_cursor_math.c`: seven resolutions/aspect ratios, letterboxing, 1,200 pan/zoom samples, time-based WASD/edge navigation.
- `tools/test_game_sites.py`: 45 checks.

Earlier versions were tested in game by the contributor and maintainer on Windows. **Manual gameplay validation of this revision is pending the contributor's separate-instance test**, including the two native Options rows, cursor visibility, focus transitions, persistence and controller regression. Steam Deck and broader controller testing remain pending.

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
