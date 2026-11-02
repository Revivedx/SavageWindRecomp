# Virtual amiibo prototype

In **Options > Gameplay**:

1. Enable the game's **amiibo** setting to make its rune available.
2. Turn **Virtual amiibo** on.
3. Choose an **amiibo figure** from the 22-entry library (the picker wraps at either end).
4. Return to play and use the amiibo rune on suitable ground. The selected figure is scanned automatically.

The virtual setting and figure selection are saved in `savagewind.ini`. Changes take effect without restarting;
changing the figure during a scan applies to the next scan. Each figure normally has its own stable virtual identity.
Turning Virtual amiibo off removes a detected figure and leaves the reader waiting without presenting one.

**Repeat amiibo scans**, also in Gameplay, is an optional saved setting (off by default). It now bypasses only the
UID-based daily-use check while retaining each figure's stable serial number and the game's lifetime-use history.
The game still selects the rewards. This is intended to allow Epona's first-use reward to give way to subsequent
chest rolls instead of repeatedly presenting a brand-new figure. The revised bypass has been confirmed to produce subsequent chest rewards in game.
Turning repeat off restores the normal daily-use check; scans made while repeat was on still count in that history.

Earlier builds used a fresh identity for each repeat scan. Those existing save records are left alone. The first
scan with a figure's stable identity may still count as a first use, even if a previous build spawned several
Eponas. No save history is erased or migrated. A separate reset-history action is not implemented.

This hook has been located and built for game version 1.9.0. Updating from the earlier prototype requires rebuilding
the game code as well as the runtime (`make gen`, then `make -j` in a source checkout). The current workspace's
binary and game library have already been rebuilt.

No figure dumps or keys are needed. The original four figures (Archer Link, BOTW Zelda, Guardian and Bokoblin),
Gameplay controls and the earlier repeat implementation have been confirmed working in game. The expanded library still needs
gameplay testing. Wolf Link is not included; writable amiibo application data is not implemented.

The library contains:

- BOTW: Archer Link, Rider Link, Zelda, Guardian, Bokoblin, Daruk, Mipha, Revali and Urbosa.
- Zelda series: 8-Bit Link, Ocarina of Time Link, Majora's Mask Link, Twilight Princess Link, Skyward Sword Link,
  Wind Waker Toon Link and Wind Waker Toon Zelda.
- Super Smash Bros.: Link, Zelda, Toon Link, Sheik, Ganondorf and Mario (generic reward testing).

The existing four entries keep their original saved keys and virtual identities. New figures are appended.

For the original terminal workflow, `SAVAGEWIND_AMIIBO=archer` still seeds the startup settings. The values `zelda`,
`guardian`, `bokoblin`, and `off` are also supported. Gameplay controls can change these choices afterwards.

Developer checks: `make test-amiibo` covers the service's IPC responses, figure identity consistency, live
configuration, stable identities in repeat mode, the generated daily-check hook, reset, and settings persistence
across separate processes.

## Gameplay test checklist

Launch normally with `./build/savagewind extracted`. For a diagnostic log, use
`./build/savagewind extracted > build/amiibo-gameplay.log 2>&1` instead. Test on suitable flat ground outside, and
record the selected figure, both virtual settings, exact error message, and whether a chest, loose items or a horse
appeared. The service logs the figure whose model it supplied. Rare loot is random and may depend on progression;
one ordinary chest does not establish that a figure's special loot is broken.

| Test | Steps | Expected result |
| --- | --- | --- |
| Library coverage | Turn Virtual amiibo and Repeat amiibo scans on. Scan each of the 22 entries once. | Each selection completes its reward flow without hanging or crashing. Report horse behavior separately for Twilight Princess Link and Smash Link. |
| Picker ends | On Archer Link, step left; from Mario, step right. Select, leave Options, and reopen it. | The picker wraps to Mario / Archer respectively, and remembers the selected figure. |
| Normal daily limit | Turn repeat off. Choose a figure whose stable identity has not been used today; scan it twice. | The first scan succeeds; the second is rejected by the game. If both are rejected, use another unused figure. |
| Repeated use | Turn repeat on and scan the same ordinary figure 10–20 times, letting each reward finish before the next scan. | Repeated rewards without a daily-limit error, stuck scan or crash. |
| Repeat off again | After the preceding test, turn repeat off and scan the same figure. | Identity stays the same. Scans made with repeat on count as uses today, so rejection is expected. Turning repeat back on allows another scan. |
| Switching figures | With repeat on, scan Archer, Guardian, a Champion and Mario, then return to Archer. | Each scan uses the newly selected model; no reward from a stale selection. Check the model lines in the log if uncertain. |
| Virtual scanning off | Keep the game's amiibo setting on, turn Virtual amiibo off, then activate and cancel the rune. Turn Virtual amiibo back on and try again. | No automatic reward while off; normal scanning resumes after re-enabling. |
| Cancel and retry | Begin aiming, cancel before confirming, switch runes and come back, then perform a scan. Repeat several times. | No unsolicited reward after cancellation and no stuck detection state. |
| Invalid location | Try in a shrine or another place where the game disallows amiibo; return to valid outdoor ground and scan. | The game's restriction is respected; an unsuccessful attempt does not break later scans. |
| Save and restart | Select a new figure, save, quit the application and relaunch without environment overrides. Check settings and scan again. | Selection and both virtual settings persist. Repeat mode still works; normal mode still respects the stable identity's daily limit. |
| Return to start screen | Quit to the Savage Wind start screen, play again, reload and scan. | Reader events and state reset cleanly; settings remain selected. |
| Load another save | Load another save and scan with repeat on; then test normal mode with an unused stable figure. | No stale scan state, and the game's usage history follows the loaded save. |

### Epona candidates

Test **Link (Super Smash Bros.)** and **Link (Twilight Princess)** on open horse-friendly ground outside the Great
Plateau, after obtaining the paraglider:

1. Turn Virtual amiibo and Repeat amiibo scans on. Select Smash Link.
2. Scan once. If the stable identity has never been used, Epona may appear. Mount and register her at a stable.
3. Scan the same figure 3–5 more times. Check whether it now produces ordinary chest/loot rolls instead of another
   guaranteed Epona. Twilight armor is random; a few chests without armor are not a failure.
4. Save, quit the application, relaunch and scan the same figure again. Check that it retains the subsequent-use
   behavior, rather than returning to a guaranteed first-use horse every launch.
5. Turn repeat off and scan again the same day. The daily-limit rejection should return. Turn repeat back on and
   confirm another scan succeeds.
6. Repeat the sequence for Twilight Princess Link. Record whether the game treats its first-use reward separately
   from Smash Link; do not assume their Epona histories are independent.

Existing stable-identity history, registered horses and the save's progression may affect the first result. Record
those along with any error message. Report repeated guaranteed Epona spawns after registration, a daily-limit
error with repeat on, or a scan that hangs. The service should log `bypassed daily use check (stable figure
identity)` for the daily check, while retaining the selected model in its model-info log lines.

For the four Champions, test with your installed DLC if available and note your progression. Confirm ordinary scan
completion first; investigate exclusive rewards over multiple scans rather than expecting one specific item on the
first attempt. Mario should exercise the generic non-Zelda reward path.
