# The virtual gyro at a shrine apparatus

Some shrines are solved by tilting a platform with the controller's motion sensors (the game's DungeonRotateGyro
mechanisms). A controller with a gyroscope (DualSense, DualShock 4, Switch Pro, Joy-Con, the Steam Deck) works as on
the Switch. Without one, Savage Wind turns a virtual controller instead, while the player controls such a mechanism
(game version 1.9.0):

- **Keyboard**: the gyro keys turn it at 90 degrees per second while held, and it keeps its pose when they are let go.
- **Mouse**: left-drag to rotate like rolling a ball under your hand. A straight horizontal drag turns the table.
  Right-drag horizontally to bank it or vertically to tilt it front to back. Release either button to keep the pose
  and take a new grip. The tilt key (T) and mouse wheel also work.
- **A controller without a gyroscope** (Xbox and most XInput pads): the right stick turns the platform (X) and tilts
  it front / back (Y), the left stick banks it (X) and tilts it front / back (Y). R3 or L3 levels it again. The sticks
  move neither Link nor the camera meanwhile. Raise Motion sensitivity in Options > Controls for a faster turn.
  A fully deflected stick turns a single axis at 100 degrees per second at 100% sensitivity. The inversion options apply.
- **A controller with a gyroscope**: the gyroscope tilts it as it is held (no sensitivity, inversion or lean). The
  sticks take over once pushed past half way (a gyroscope lying still, a handheld on its dock); R3 or L3 gives it back
  to the gyroscope.

Leaving the mechanism levels the virtual controller and drops any turn still to come.

## Options

Options > Controls:

- **Shrine keyboard gyro** (on): the gyro keys, only while controlling a mechanism. Anywhere else the keys they share
  (I / J / K / L turn the camera by default) work as they always do.
- **Shrine mouse drag** (on): enables left and right drag. The tilt key and gyro keys also work with it off.

`SAVAGEWIND_PC_GYRO=0|1` and `SAVAGEWIND_GYRO_SCENE=0|1` set the two switches at start.

Options > Keyboard (sticks) binds the gyro keys (`savagewind.ini`: `bind_key_gyro_*`):

| Action | Default |
| --- | --- |
| Gyro tilt up / down | I / K |
| Gyro tilt left / right | J / L |
| Gyro turn left / right | U / O |
| Gyro recenter | P |

A gyro key rebound onto another gyro key's swaps with it; it never takes a camera or movement key's binding away,
since they are only the gyro's during the mechanism.

## The tilt key

T held, the mouse tilts the virtual controller and the wheel turns it (six degrees a step), without the old 50 degree
limit. At a mechanism letting go of T keeps the pose (a clutch: move the mouse back, hold T again, go on). Anywhere
else the tilt levels again within a fraction of a second once T is let go, so it never stays in front of a
controller's own gyroscope for motion aiming.

## How it works

- `tools/game_sites.py` `gyro_scene_actor` (optional; the game code is built with it): the call in which a mechanism
  fetches its input; `tools/patches.py` adds a heartbeat after it. `runtime/pc_gyro_scene.c` turns the heartbeat into
  "the player controls a mechanism" (held through a pause; reset on a reboot).
- `runtime/hle/motion.c`: the virtual controller is kept as continuous angles, so whole turns keep their winding, and
  delivered to the game at the Switch's 200 Hz at a bounded speed (`runtime/pc_gyro.h`). The drag and the sticks are
  rotations in the table's axes, composed as quaternions and then turned back into those angles (`virtual_turn`), so
  any axis works at any pose.
- `runtime/hle/input.c`: the gyro keys and the sticks; `runtime/ui/gyro_drag.h`: the drag (nothing is drawn).

`make test-gyro` runs the tests: the keys through the bindings, the two switches saved and loaded, turns at polling
rates from 30 to 1000 Hz, full and fast turns, the drag's and the sticks' composition, the mechanism's heartbeat,
pause and reboot, and the patch's instruction check. None needs the game files.
