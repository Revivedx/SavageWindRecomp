# Controls

[Back to the README](../README.md)

Any controller SDL knows works, by button position; one with a gyroscope (DualSense, DualShock 4, Switch controllers,
Steam Deck) also aims and tilts by motion.

Controller buttons, sticks and motion only control the game while its window has focus. Switching away releases
held inputs; returning reads the controller's current state.

Keyboard and mouse defaults:

| Action | Key | Action | Key |
|---|---|---|---|
| Move | W A S D | Camera | Mouse (or I J K L) |
| Walk (hold) | C | Crouch | Left Ctrl |
| Jump (X) | Space | Sprint / back (B) | Left Shift |
| Interact / confirm (A) | E | Attack (Y) | Left click |
| Shield / lock on (ZL) | Right click | Bow (ZR) | F |
| Rune (L) | Q | Throw (R) | R |
| Look (right stick press) | Middle click | Weapons / shields / runes / whistle (d-pad) | 1 / 2 / 3 / 4 |
| Inventory (Plus) | Tab | Map (Minus) | M |
| Tilt the controller (hold, motion puzzles) | T + mouse | | |

Every game action has its own key and its own controller button, set on the Key bindings page of the game's Options
(a keyboard/mouse button and a controller button per row; see [ACTIONS.md](ACTIONS.md)). Mouse sensitivity
(horizontal and vertical), Y inversion and motion sensitivity and inversion are on the Controls page. The game's
button prompts show the keys you bound, or your controller's own buttons (Controls > Button icons: Automatic follows
the device you use). The mouse turns the camera directly, as in a PC game: 0.022° per count at 100% sensitivity (as
Quake and Source count it), on both axes (the vertical one scaled by Controls > Mouse vertical sensitivity), with no
acceleration or top speed, the same at any frame rate. The bow's and the scope's own slower speeds stay, and so do the
game's camera speed setting and its camera axis inversion options. Esc frees the cursor, a click in the window takes
it back. F10 cycles the performance overlay (off, FPS, full). Full also shows Link's XYZ coordinates from the game's
position copy, in game units, with one decimal place. The line is hidden on the title screen, while loading,
during movies and scripted events (cutscenes, talks, item pickups), or when the position is unavailable.
