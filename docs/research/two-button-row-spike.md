# Two native buttons in an Options row: spike

2026-11-01, BotW 1.9.0.

**Status:** the layout now applies to every binding row, with a column header row and a row band
(darkener under the non-hovered button, slightly lighter hovered button). The text below describes
the first three-row probe. Corner rounding of the row band is left for a later change.

**Result: three rows render two real native buttons side by side in the running game.**
The hidden Windows probe reached Title > Options > Key bindings without an observed crash.
Its screenshot shows Interact, Jump and Attack with separate key/pad text and the native highlight.
Native column navigation and keyboard capture also ran. Sound and physical input acceptance remain open.

## Implemented

Only the first three action rows on P_BINDS, Interact (0), Jump (2), Attack (3), use two cells.
The existing choice control is the keyboard/mouse cell; the ordinary control is the controller cell.
Both remain registered native controls. Choice arrows are hidden. Both enabled/disabled text copies
use the game's message text setter. No ImGui controls or Nintendo assets were added.

The buttons are narrowed to half width, moved apart, and their text gets inverse horizontal scaling
to retain native font proportions. The label is narrowed too. This geometry is a candidate,
not a visually approved layout. Other rows and pages retain their existing path.

Native select, state, confirm, initial-focus and update routing now recognize both cells.
Left/right changes column, up/down retains it, mouse hit testing uses two disjoint native panes,
and confirm starts BIND_DEV_KEY or BIND_DEV_PAD capture. Mouse clicks request the native
Decide animation before its confirmed callback, rather than calling capture directly. Captures redraw both cell texts.
A sidecar stores state without enlarging the game's row object.

All 13 new sites are one strict optional group. A missing, ambiguous or wrong-address match disables
the group even on a known build, preserving the existing single-button rows. Unknown row shapes
also keep their original redraw. `SAVAGEWIND_TWO_BUTTON_ROWS=0` disables the runtime experiment.

## Evidence and actual checks

- Every new signature uniquely matches the supplied 1.9.0 main NSO at its listed KNOWN address.
  Every emitted hook's replaced opcode was checked against that binary; generated hook C passed syntax checking.
- Verified pane translation `+0x30`, scale `+0x48`, size `+0x50/+0x54` and dirty bit 4 at `+0x58`.
  Native Pane calculation at `0x70000276a0` rebuilds the matrix at `+0x70` and propagates to children
  (`0x7000027744..0x7000027758`, `0x7000027ad0..0x7000027adc`, `0x7000027bb0..0x7000027bc8`).
  Cached world matrices are never edited. No BFLYT/archive patch was necessary for this candidate geometry.
- Linux and Windows: `make test-actions test-bind-capture test-pc-controls` pass.
  Windows also passes `test-input-focus`, `test-gyro`, `test-two-button-rows`, the 45 game-site checks,
  and the strict-group failure tests. The new integration test exercises the actual Options hooks and
  real capture engine with synthetic rows, including wrong-device rejection and CPU-state preservation.
- Windows release executable, configuration executable and frozen generator build pass.
  This includes the edited `.mm` file.
- `vibe-check runtime/ui/game_options.mm` reports no findings; `git diff --check` passes.

### Hidden runtime probe, completed 2026-11-01

- Private library setup completed in 602 seconds with `Game files ready. -> READY`.
  The probe loaded game version 1.9.0 and 103 sites. It used a dedicated control port. Physical input and audio were disabled.
- The existing control API navigated the actual title menu and Options to Key bindings. The page had
  62 rows. Six `[row-spike]` configuration lines show the two parts on each of the three target rows,
  with native root x positions changed from 130 to 40/190. The game continued rendering the page.
- Native cursor-node reads confirmed right selected Interact's ordinary/pad button, down selected
  Jump's pad button, up returned to Interact's pad button, and left selected its choice/key button.
  These were real runtime node/button pointers, not host-test expectations.
- Confirm on the left cell logged `capture slot 256 device key`. Its screenshot shows `Press a key...`
  only in that cell; the controller cell still shows B. The timeout produced an Interact redraw.
- Screenshots saved successfully through the control API's `shot` command:
  `two-button-row.png` (1280x720, frame 4961), `two-button-row-left.png` (frame 11652), and
  `two-button-row-key-capture.png` (frame 11769). The first and capture images were visually inspected.
- `probe-stderr.log` contains no warning/error/failure/fatal/assertion/crash matches. Stdout is empty;
  stderr is the useful game log. The last periodic entry is frame 11962, followed by control connection
  closure near the configured `SAVAGEWIND_EXIT_AT=12000` limit. Exit status was not recorded.

**Not tested live:** controller capture, assigning a physical key/pad binding, mouse hover/click,
sounds (audio was disabled), long/localized labels, scrolling, held/repeated directions, reset/reopen
and the pause-menu entry point. The frame limit ended the probe before the controller-capture step.
Device filtering and mouse routing have host tests; these do not replace physical/native acceptance.

## Recommendation

**Go with changes.** The live render and column navigation establish the core two-native-button approach.
Finish the live input/sound checks and Fabian's visual acceptance before expanding it.
Allow roughly 2 to 4 focused days for a full page:
final widths and long labels, disabled controller cells, native navigation/scroll polish and device QA.
Column headers likely need two added text panes through the existing in-memory Common archive rebuild,
scoped to P_BINDS, plus space above the list. That remains a feasibility note, outside this spike.
