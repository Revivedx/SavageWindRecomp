# Virtual amiibo prototype

In **Options > amiibo**:

1. Turn **amiibo rewards** on. This enables the game's rune setting and virtual scanning together.
2. Open **Choose amiibo** and select a figure from the 22-entry library. The button shows your current selection.
3. Return to play and use the amiibo rune on suitable ground. The selected figure is scanned automatically.

The amiibo controls now have their own entry on the main Options page. The former Gameplay controls and separate
game amiibo toggle are replaced by this page. The dedicated page, combined enable switch and searchable browser have been confirmed working in game.

The enable setting, figure selection, Favorites and Recent list are saved in `savagewind.ini`. Changes take effect without restarting;
changing the figure during a scan applies to the next scan. Each figure normally has its own stable virtual identity.
Turning amiibo rewards off removes a detected figure and leaves the reader waiting without presenting one.

**Repeat scans**, also on this page, is an optional saved setting (off by default). It now bypasses only the
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
previous Gameplay controls and the repeat cooldown bypass have been confirmed working in game. The expanded library still needs
gameplay testing. Wolf Link is not included; writable amiibo application data is not implemented.

The library contains:

- BOTW: Archer Link, Rider Link, Zelda, Guardian, Bokoblin, Daruk, Mipha, Revali and Urbosa.
- Zelda series: 8-Bit Link, Ocarina of Time Link, Majora's Mask Link, Twilight Princess Link, Skyward Sword Link,
  Wind Waker Toon Link and Wind Waker Toon Zelda.
- Super Smash Bros.: Link, Zelda, Toon Link, Sheik, Ganondorf and Mario (generic reward testing).

The existing four entries keep their original saved keys and virtual identities. New figures are appended.

For the original terminal workflow, `SAVAGEWIND_AMIIBO=archer` still seeds the startup settings. The values `zelda`,
`guardian`, `bokoblin`, and `off` are also supported. The amiibo page can change these choices afterwards.

Developer checks: `make test-amiibo` covers the service's IPC responses, figure identity consistency, live
configuration, search and filters, Favorites / Recent persistence, browser rendering and keyboard selection,
the combined enable-switch bridge, stable identities in repeat mode, the generated daily-check hook, reset, and settings persistence
across separate processes.

## Gameplay test checklist

Launch normally with `./build/savagewind extracted`. For a diagnostic log, use
`./build/savagewind extracted > build/amiibo-gameplay.log 2>&1` instead. Test on suitable flat ground outside, and
record the selected figure, amiibo rewards and Repeat scans, exact error message, and whether a chest, loose items or a horse
appeared. The service logs the figure whose model it supplied. Rare loot is random and may depend on progression;
one ordinary chest does not establish that a figure's special loot is broken.

| Test | Steps | Expected result |
| --- | --- | --- |
| Library coverage | Turn amiibo rewards and Repeat scans on. Scan each of the 22 entries once. | Each selection completes its reward flow without hanging or crashing. Report horse behavior separately for Twilight Princess Link and Smash Link. |
| Browser | Open Choose amiibo, search for a character, select a result, leave Options and reopen it. | The result becomes the selected figure and the button shows its name. Back / Cancel leaves the previous selection unchanged. |
| Normal daily limit | Turn repeat off. Choose a figure whose stable identity has not been used today; scan it twice. | The first scan succeeds; the second is rejected by the game. If both are rejected, use another unused figure. |
| Repeated use | Turn repeat on and scan the same ordinary figure 10–20 times, letting each reward finish before the next scan. | Repeated rewards without a daily-limit error, stuck scan or crash. |
| Repeat off again | After the preceding test, turn repeat off and scan the same figure. | Identity stays the same. Scans made with repeat on count as uses today, so rejection is expected. Turning repeat back on allows another scan. |
| Switching figures | With repeat on, scan Archer, Guardian, a Champion and Mario, then return to Archer. | Each scan uses the newly selected model; no reward from a stale selection. Check the model lines in the log if uncertain. |
| Virtual scanning off | Turn amiibo rewards off, return to play, then turn it on again and use the rune. | The rune is disabled while off; it becomes available and scans again after re-enabling. |
| Cancel and retry | Begin aiming, cancel before confirming, switch runes and come back, then perform a scan. Repeat several times. | No unsolicited reward after cancellation and no stuck detection state. |
| Invalid location | Try in a shrine or another place where the game disallows amiibo; return to valid outdoor ground and scan. | The game's restriction is respected; an unsuccessful attempt does not break later scans. |
| Save and restart | Select a new figure, save, quit the application and relaunch without environment overrides. Check settings and scan again. | Selection and both virtual settings persist. Repeat mode still works; normal mode still respects the stable identity's daily limit. |
| Return to start screen | Quit to the Savage Wind start screen, play again, reload and scan. | Reader events and state reset cleanly; settings remain selected. |
| Load another save | Load another save and scan with repeat on; then test normal mode with an unused stable figure. | No stale scan state, and the game's usage history follows the loaded save. |

### Epona candidates

Test **Link (Super Smash Bros.)** and **Link (Twilight Princess)** on open horse-friendly ground outside the Great
Plateau, after obtaining the paraglider:

1. Turn amiibo rewards and Repeat scans on. Select Smash Link.
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

### Dedicated amiibo page checks

- Open Options from both the title screen and the pause menu. The main list should contain **amiibo**; Gameplay
  should no longer contain any amiibo controls. Back from the amiibo page should return to the main Options list.
- Turn **amiibo rewards** off and on. Leave Options and verify the rune is disabled/enabled accordingly and scans
  work when enabled. A second enable switch should not be needed.
- Pick a figure and change **Repeat scans**; leave and reopen the page, then save and restart the application.
  Verify all three values persist and that the rune and virtual scanning agree with the enable switch. Also try
  enabling from title-screen Options before loading a save.
- Check mouse, keyboard and controller navigation, including scrolling to amiibo on the main Options page.

## Searchable figure browser

**Options > amiibo > Choose amiibo** opens the library. Search matches words across character names, edition keys
and series names without regard to case. For example, `link smash`, `wind toon` or `guardian wild` narrow the
results. The series dropdown offers All series, Breath of the Wild, The Legend of Zelda and Super Smash Bros.

- **All** lists the catalog. **Favorites** lists pinned figures. **Recent** lists the last eight selections, newest
  first, without duplicates. Search and series filters apply within each tab.
- **[+]** pins a figure; **[-]** removes it. Favorites edits are retained when leaving the browser, including when
  cancelling the figure selection. Favorites and recent entries are saved by stable catalog key.
- Click a figure or focus it and confirm to select. Up/down navigate; Enter or the bound confirm key works when
  not typing. A controller uses its bound confirm/back buttons, d-pad or left stick, and L/R to cycle tabs.
- Activate Search with a controller to use the existing on-screen keyboard. While typing, letters are search text
  rather than gameplay shortcuts. Tab and Shift+Tab can move keyboard focus between controls.
- Escape, controller Back, or Cancel closes the browser without changing the selected figure. If editing text or
  choosing a series, first back out of that control, then close the browser.

The currently selected figure receives focus on opening. Preview text below the results shows the focused or
hovered figure and its series. Choosing a figure does not automatically enable rewards or perform a scan.

Browser testing:

1. Search for `link`, then `link smash`; check that the second search narrows the results. Try a search with no
   matches, then clear it. Confirm every figure remains reachable after clearing search and the series filter.
2. Pin two figures, open Favorites, and unpin one. Cancel, reopen, and verify the remaining favorite persists.
3. Select several different figures and reselect one. Recent should put that figure first without duplicating it.
4. Restart and check Favorites, Recent and the selected figure. Use the rune to confirm the new selection scans.
5. Try mouse, keyboard and controller selection, controller text entry, series selection, and cancellation. The
   Options page behind the browser should not move or close when you confirm or back out of the browser.
