# Virtual gyro keyboard and apparatus drag

This experiment extends Savage Wind's existing motion pipeline. Controller gyro input and the original T + mouse fallback come from upstream.

Options > Controls has two independent, persistent switches:

- **Shrine keyboard gyro**: enables keyboard rotation only while controlling a shrine apparatus. Outside that interaction it never reserves any keys or changes bow aiming. Ordinary IJKL camera bindings continue working outside apparatus control, even with this switch on.
- **Shrine mouse drag**: enables left-drag rotation during the apparatus interaction. Release to retain orientation and re-grip elsewhere. Turning this off does not disable apparatus keyboard control.

Both switches default on and apply immediately. The executable needs no custom launcher or environment variables. The old `SAVAGEWIND_PC_GYRO` and `SAVAGEWIND_GYRO_SCENE` variables remain optional startup overrides; Controls can change their values during the session.

Options > Keyboard (sticks) exposes seven remappable gyro actions with descriptions:

| Action | Default | Effect |
| --- | --- | --- |
| Gyro tilt up / down | I / K | Pitch the virtual controller up / down |
| Gyro tilt left / right | J / L | Roll the virtual controller left / right |
| Gyro turn left / right | U / O | Rotate around the third axis |
| Gyro recenter | P | Reset orientation and cancel pending rotation |

Rotation runs at 90 degrees/s. Gyro bindings are saved in `savagewind.ini`, including mouse-button bindings. Reassigning a gyro key swaps conflicts only with other gyro actions, leaving normal movement/camera mappings untouched. During apparatus keyboard control its assigned gyro keys have priority; outside it ordinary mappings resume.

The default third-axis pair is now U/O, and recenter moved from O to P. The I/K pitch direction is corrected. No F3 shortcut, sphere, rings, textures or extra panel is included. Dragging uses the click point and viewport dimensions, not inferred model bounds. Upstream's T + mouse fallback and physical-controller gyro remain separate.

The virtual sensor keeps continuous rotations through full turns. Fast packets are delivered at a bounded combined angular rate, preserving their direction and winding. Mouse drags compose quaternion increments into the virtual target orientation. Each shrine still applies its own reference frame; this does not bypass a mechanism's own constraints.

A single optional generator site supplies a DungeonRotateGyro heartbeat. Guest activation and pause state are read on the game thread. Leaving the apparatus clears virtual rotation and pending input; in-place reboot resets activation state. The render thread handles dragging, and the motion mutex protects the virtual sensor. No model, camera projection or render-transform hook is needed.

## Validation and limits

The player tested the combined release-based Windows build at 4K and reported working apparatus rotation and the final drag correction. These reports cover the tested mechanisms, not every shrine. This branch has been adapted onto `test` at `0dece28`; the contributor built and tested the updated standalone gyro instance on Windows and confirmed the new controls work. The final labels mark each action as '(only in shrines)' and use short help text.

Changed runtime objects compile with MSYS2 UCRT64/Clang. Sensor tests cover 30/60/120/200/1000 Hz, full revolutions, fast bursts, gravity, orientation, clutch retention and reset. ImGui tests cover 720p/1080p/4K/ultrawide, click without jumps, hold/re-grip and exit without visible geometry. Synthetic guest-memory tests cover apparatus activation, pause/resume, timeout/exit, reboot and version gating. Hook tests reject unexpected instructions. The upstream game-site suite passes.

The Windows executable and generated game library compile on this updated base. `make test-gyro-controls` also verifies pitch direction, remapping, camera ownership outside apparatus control, independent switches, persistence across launches and binding capture. The contributor's standalone gyro gameplay check passed; this does not establish compatibility with every shrine, physical controller or Steam Deck.

Run from the repository root with the dependencies in the upstream build guide:

```sh
make -j8
make test-gyro-controls
clang -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections tests/test_motion_keyboard.c -lSDL3 -o build/test_motion_keyboard.exe
build/test_motion_keyboard.exe
clang -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections tests/test_gyro_activation.c -o build/test_gyro_activation.exe
build/test_gyro_activation.exe
make -j8 build/imgui/imgui.o build/imgui/imgui_draw.o build/imgui/imgui_widgets.o build/imgui/imgui_tables.o
clang++ -std=c++17 -Ithird_party/imgui tests/test_gyro_drag.cpp build/imgui/imgui.o build/imgui/imgui_draw.o build/imgui/imgui_widgets.o build/imgui/imgui_tables.o -o build/test_gyro_drag.exe
build/test_gyro_drag.exe
python tools/test_game_sites.py
python tests/test_gyro_site.py
```

I developed and tested these controls with Codex assistance. I do not know whether someone is already working on a similar approach elsewhere; I am sharing my progress and tests for review.
