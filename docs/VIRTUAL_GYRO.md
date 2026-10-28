# Virtual gyro keyboard and apparatus drag

This experiment extends Savage Wind's existing motion pipeline. Controller gyro input and the original T + mouse fallback come from upstream.

Enable keyboard rotation with `SAVAGEWIND_PC_GYRO=1`. Enable automatic mouse dragging during the BOTW 1.9.0 apparatus interaction with `SAVAGEWIND_GYRO_SCENE=1`. Both additions are opt-in. Activate the apparatus normally, hold left mouse and drag in the game view; release to retain the orientation and re-grip elsewhere. No F3 shortcut, sphere, rings, textures or extra panel is included. Dragging depends on the click point and viewport dimensions, not inferred model bounds.

- I/K and J/L: tilt on two axes, 90 degrees/s.
- U/P: rotate the third axis, 90 degrees/s.
- O: recenter the virtual controller and cancel pending rotation.
- T + mouse retains its orientation when released. In the original captured-mouse mode, T + wheel adds yaw at six degrees per wheel unit.

The virtual sensor keeps continuous rotations through full turns. Fast packets are delivered at a bounded combined angular rate, preserving their direction and winding. Mouse drags compose quaternion increments into the virtual target orientation. Each shrine still applies its own reference frame; this does not bypass a mechanism's own constraints.

A single optional generator site supplies a DungeonRotateGyro heartbeat. Guest activation and pause state are read on the game thread. Leaving the apparatus clears virtual rotation and pending input; in-place reboot resets activation state. The render thread handles dragging, and the motion mutex protects the virtual sensor. No model, camera projection or render-transform hook is needed.

## Validation and limits

The player tested the combined release-based Windows build at 4K and reported working apparatus rotation and the final drag correction. These reports cover the tested mechanisms, not every shrine. This branch has been adapted onto `test` at `6572c35483e512d321d840daa296653ccc205adc`; its full game session has not been tested.

Changed runtime objects compile with MSYS2 UCRT64/Clang. Sensor tests cover 30/60/120/200/1000 Hz, full revolutions, fast bursts, gravity, orientation, clutch retention and reset. ImGui tests cover 720p/1080p/4K/ultrawide, click without jumps, hold/re-grip and exit without visible geometry. Synthetic guest-memory tests cover apparatus activation, pause/resume, timeout/exit, reboot and version gating. Hook tests reject unexpected instructions. The upstream game-site suite passes.

A normal Windows launcher build on this toolchain is blocked by an existing undeclared `lstat` in `runtime/ui/launcher.mm`, also present in the unmodified base. This PR does not change that launcher code. Full application build and gameplay validation on `test`, including physical controllers, remain pending.

Run from the repository root with the dependencies in the upstream build guide:

```sh
make -j8 build/rt/hle/input.o build/rt/hle/motion.o build/rt/pc_gyro_scene.o build/rt/ui/menu.o
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
