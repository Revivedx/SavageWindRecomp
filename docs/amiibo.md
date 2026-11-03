# Virtual amiibo prototype

In **Options > amiibo**:

1. Turn **amiibo rewards** on (also available on Gameplay). This enables the rune and virtual reader together.
2. Return to play and use the amiibo rune on suitable ground. The picker opens over the game.
3. Choose a figure from the 952-entry offline library and confirm. The chosen figure is presented to the reader.

While the picker is open, simulation waits at the next game-frame boundary and the last game image is re-presented
with the live picker at up to 60 updates per second. The search keyboard remains usable. Confirming, cancelling or
stopping detection releases the hold; reboot/shutdown can interrupt it too. This does not change the game's own
pause flags, and time spent choosing is excluded from the next frame's elapsed time so gameplay does not catch up.

**Automatic scanning** is off by default: the scan-time picker is the chosen workflow. Turn it on to restore the previous workflow: choose a
figure once in Options and automatically scan that saved selection whenever you use the rune. Options > Choose
amiibo remains available in either mode. Selection, Favorites and Recent are saved after a scan choice too.
Cancelling the scan picker presents no figure; use the game's Back action to leave the rune or start a new scan.

### Optional personal Wolf Link

**Options > amiibo > Wolf Link data** selects **Synthetic (20 hearts)** (the default) or **Own amiibo**.
For Own amiibo, put your own encrypted Wolf Link NTAG215 dump and amiibo keys in the data root:

```text
amiibo/wolf_link.bin       (532, 540 or 572 bytes)
amiibo/key_retail.bin      (160 bytes)
```

In a source checkout, this is the `amiibo/` folder alongside `extracted/`. No dumps or keys are supplied or
downloaded. The loader checks the Wolf Link model ID, tag identity and both HMACs before presenting a figure.
It uses the dump's serial number, nickname, registration flags and actual application data, not a forced heart
count. A registered Wolf Link with Twilight Princess HD data uses the hearts saved in that data.

Missing files, wrong keys, corrupt dumps or damaged saved application data show a warning and block Wolf Link's
confirmation rather than silently falling back to synthetic data. Other catalog figures still work. After fixing
files, close and reopen the picker to reload them. Switching the source affects the next scan, not a mounted tag.

Game writes in Own amiibo mode go to `amiibo_data/wolf_link-<serial>.bin`, separately for each serial number.
Flush, unmount and stop-detection save pending writes; write failures are reported to the game. The original
dump and keys are never overwritten. Existing application-data files take precedence over the dump's initial
data on later scans; back up and remove the matching file if you want to restore the dump's original game data.
Both personal-data folders and `key_retail.bin` are Git-ignored and excluded from release packaging.

This optional reader has automated crypto-vector, synthetic-dump, IPC and persistence tests, but still needs
verification with a player's real Wolf Link dump in game. Repeat scans remains an independent setting.

The amiibo controls have their own entry on the main Options page. Gameplay also exposes the same combined
enable switch, rather than a separate game-only toggle. Both the original Options-based workflow and the new
scan-time picker have been tried in game; the scan-time picker is the preferred interface.

The enable setting, figure selection, Favorites and Recent list are saved in `savagewind.ini`. Changes take effect without restarting;
changing the figure during a scan applies to the next scan. Each figure normally has its own stable virtual identity.
Turning amiibo rewards off removes a detected figure and leaves the reader waiting without presenting one.

The amiibo page and browser controls follow the selected menu text language, with translations for Spanish,
French, German, Italian, Dutch, Portuguese, Russian, Japanese, Korean, Simplified and Traditional Chinese, and Thai.
The generic series filters (All series and Others) are translated too. Figure names and branded series titles
remain the catalog's English names; search uses those catalog fields rather than translated names and does not
perform Unicode case folding. The browser keeps a stable window identity when its translated title changes.

The browser uses the full-screen layout and game-font reader from petrifiedsweets' PR #422: cream text, dark
rows, a glowing corner cursor and a figure-details panel. NormalS_00 and Normal_00 are read from the player's
regional font archive in RomFS. Missing bitmap fonts, including Korean, Chinese and Thai, use our existing
Fira Sans/system-font fallbacks. No Nintendo fonts are shipped. Favorites, Recent and the offline catalog remain.

Controller shortcuts: Y searches, X clears, L/R change series, triggers switch All/Favorites/Recent, and Minus/Back
toggles a favorite. Confirm selects and B cancels; search also remains reachable through normal controller navigation.
The compact button badges follow the active input device, controller family and mapped buttons instead of always
showing Nintendo labels. The names above describe the default Nintendo layout.
On the keyboard, Ctrl+F focuses search, Delete clears it, F toggles a favorite, brackets change series, and Q/E switch
tabs. Enter after typing returns focus to the results. A mouse click previews a figure; double-click or the Choose
amiibo button confirms. The star beside a figure toggles its favorite state without scanning it, and the small gold
dot marks the saved selection. Empty filters hide stale details and disable confirmation; Reset filters restores
the full catalog. The scrolling list renders only visible rows while keeping controller navigation across the catalog.

## UI attribution

Thanks to **[petrifiedsweets](https://github.com/petrifiedsweets)** for the amiibo reader and game-styled picker in
[PR #422](https://github.com/Revivedx/SavageWindRecomp/pull/422). Our picker adapts the row/cursor drawing and
game-font reader from [revision `3e329d0`](https://github.com/petrifiedsweets/SavageWindRecomp/tree/3e329d0d9dc722771ce8713ab4189d16999ff7ea),
under the project's MIT license, and adopts its scan-time selection approach. The relevant upstream files are
[`runtime/ui/menu.mm`](https://github.com/petrifiedsweets/SavageWindRecomp/blob/3e329d0d9dc722771ce8713ab4189d16999ff7ea/runtime/ui/menu.mm)
and [`runtime/ui/game_font.mm`](https://github.com/petrifiedsweets/SavageWindRecomp/blob/3e329d0d9dc722771ce8713ab4189d16999ff7ea/runtime/ui/game_font.mm).
The optional Wolf Link importer also adapts the tag ordering and key derivation from that revision's
[`runtime/amiibo.c`](https://github.com/petrifiedsweets/SavageWindRecomp/blob/3e329d0d9dc722771ce8713ab4189d16999ff7ea/runtime/amiibo.c),
using FFmpeg libavutil's AES and HMAC implementations instead of adding another crypto dependency.

The integration keeps our synthetic offline figures, Favorites/Recent, settings, repeat-scan bypass and 20-heart
Wolf Link support. Personal dump support is restricted to the optional Wolf Link source; it does not replace
the offline catalog. The bitmap fonts remain the player's own game assets and are not included in releases.

## Repeat scans and figure data

**Repeat scans**, also on this page, is an optional saved setting (off by default). It bypasses the
UID-based daily-use check and, for Wolf Link, his separate death cooldown, while retaining each figure's stable
serial number and the game's lifetime-use history.
The game still selects the rewards. This is intended to allow Epona's first-use reward to give way to subsequent
chest rolls instead of repeatedly presenting a brand-new figure. The revised bypass has been confirmed to produce subsequent chest rewards in game.
Turning repeat off restores the normal daily-use check; scans made while repeat was on still count in that history.

Earlier builds used a fresh identity for each repeat scan. Those existing save records are left alone. The first
scan with a figure's stable identity may still count as a first use, even if a previous build spawned several
Eponas. No save history is erased or migrated. A separate reset-history action is not implemented.

This hook has been located and built for game version 1.9.0. Updating from the earlier prototype requires rebuilding
the game code as well as the runtime (`make gen`, then `make -j` in a source checkout). The current workspace's
binary and game library have already been rebuilt.

No figure dumps or keys are needed for the default synthetic source. The original four figures (Archer Link, BOTW Zelda, Guardian and Bokoblin),
previous Gameplay controls and the repeat cooldown bypass have been confirmed working in game. The expanded library still needs
gameplay testing. With the default source, selecting **Midna & Wolf Link** supplies synthetic registration and Twilight Princess HD
application data for **20 hearts** (Cave of Shadows level 40). No dump or keys are required. This path has automated
IPC coverage. Synthetic writes are retained only within the current detection window;
each new scan starts with 20 hearts. BOTW's own companion-health and daily-use rules may still affect a summon.
With **Repeat scans** enabled and Wolf Link selected, reads of `WolfLink_DiedToday` now return false, allowing
the game to attempt another summon after his death. The saved flag is not cleared; switching repeat off restores
its real value. Other boolean flags, figure identities and lifetime reward history are unchanged. This additional
hook has automated coverage and has been built for 1.9.0. Resummoning after death with Repeat scans enabled
has been confirmed working in game.
Other figures have no application data. Catalog presence does not guarantee a special reward or game compatibility.

The synthetic Wolf Link data uses application ID `0x1019c800`, level byte `0x11` and quarter-heart byte `0x12`,
as documented by [TagMo's application-data implementation](https://github.com/HiddenRamblings/TagMo/blob/master/app/src/main/java/com/hiddenramblings/tagmo/amiibo/tagdata/AppDataZeldaTP.kt).

The offline catalog contains 952 entries from [AmiiboAPI](https://amiiboapi.org/), including figures, cards,
bands, blocks and yarn amiibo across 33 series filters. The first 22 entries retain their saved keys, order and
virtual identities; later entries use `id_` followed by the API's 16-digit head/tail ID. Each entry has a distinct,
stable virtual identity, including entries beyond index 255. The game determines compatibility and rewards;
new entries have automated service coverage but still need gameplay testing. Images are not bundled.

Catalog metadata is stored in `data/amiibo.json`, with a generated C catalog in `runtime/amiibo_catalog.h`.
Credit goes to 8bitDream and contributors and the original AmiiboAPI by Nevin Vu (N3evin).
See [catalog attribution](../data/README.md) and the preserved [MIT license](../data/AmiiboAPI-LICENSE.txt).
The game never contacts the API. To update the catalog:

```sh
curl -fL https://amiiboapi.org/api/amiibo/ -o /tmp/amiibo-api.json
python3 tools/update_amiibo_catalog.py --source /tmp/amiibo-api.json
make -j16
make test-amiibo
```

The importer appends new IDs and preserves existing entries, including their names, keys and ordering. It does
not remove or reorder entries when the upstream catalog changes. To regenerate from the checked-in snapshot,
run `python3 tools/update_amiibo_catalog.py`; `--check` verifies the generated header without modifying it.

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
| Library coverage | Turn amiibo rewards and Repeat scans on. Scan representative entries from each series and item type. | Each selection completes its reward flow without hanging or crashing. Report horse behavior separately for Twilight Princess Link and Smash Link. |
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
results. The series dropdown includes Breath of the Wild, The Legend of Zelda, Super Smash Bros., Animal Crossing, Metroid and the other imported series. Search also matches game, character and item type (for example, `card`).

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
