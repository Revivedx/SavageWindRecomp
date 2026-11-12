# Quick inventory replacement

When a weapon, bow or shield cannot be received because its inventory is full,
the optional **Quick inventory replacement** setting opens the native quick
selector. Choose the item to discard, confirm replacement, or go Back without
changing the inventory. The native incoming-item card retains its name,
description and modifier display. The selected outgoing slot has a gold border;
the equipped slot retains its native blue background.

The setting is off by default and requires game version 1.9.0. It is independent
of **Quick inventory dropping**. **Quick inventory sorting** controls whether
sorting is available inside the replacement selector. Prompts use the existing
raw bindings, device-family policy and bitmap-font loader, including the
player's RomFS font overlay; they do not assume physical face-button letters.

## Native lifecycle

Ground pickups keep the exact native source actor through retained,
generation-checked links. First-discovery notifications finish their native
event before opening the selector. A confirmed outgoing discard uses the same
guarded action as quick drop. Equipped items finish their asynchronous native
detach before the incoming pickup is retried. Source replacement, actor teardown
or a changed interaction target cancels that retry.

Ordinary ground gear uses the native capacity check. Shop sources retain their
purchase eligibility check; ordinary pickups never depend on the item's buying
price or the player's rupees.

Chest replacement defers the existing pending receive context. Canceling still
runs native cleanup, while confirming preserves the original reward, modifier
and event context. The owned preview card is the only modal exception; unrelated
modal screens, events, protected items and player restrictions remain guarded.

All helpers and hook locations resolve through an optional `game_sites` group.
Each instruction replacement checks the expected instruction. Entry observers
and gates also run for direct generated calls. Tick, fill and draw wrappers get
their native callback for the current invocation; no generated callback is
cached across library unloads. Query, chest and toast context lives in their
generated C invocation, including the toast's return and both tail exits.
`tools/gen.py` is unchanged. The feature contains no fixture-spawn commands or
automatic test-category injection.

## Validation

Synthetic checks use no game assets:

```sh
make test-quick-replace test-quick-drop-native test-quick-inventory-sort
python tools/test_quick_replace_codegen.py
python tools/test_game_sites.py
python tools/test_layout.py
python3 tools/i18n_check.py
```

The lifecycle harness checks retained source identity, stale retry handoff,
chest cancellation and deferred receive, asynchronous outgoing completion,
native UI restoration, and complete CPU preservation. Executed synthetic guest
code covers the checked gates, original prologues, query/chest/toast contexts,
all toast exits, callback LR, scalar/vector results and flags with and without
register locals. Existing input, drop and sorting checks cover the shared
action gate, replacement retry injection and scoped modal exceptions.

The ground-pickup regression also checks that insufficient rupees reject a
shop source while permitting ordinary full-inventory replacement. It checks
capacity transitions, missing targets, CPU preservation and origin handoff.
The same validation fix was verified in the combined-feature Linux runtime on
an isolated player-save copy with a native gear-copy fixture and a real discard:
Lightning Rod durability 900, cancel/reopen, reciprocal exchange restoring all
original inventory signatures and 9/6/3 weapon/bow/shield counts. The received
world actor disappeared; the outgoing boomerang retained durability 2300.
Steam Deck VANGOGH, RADV Mesa 26.2 development driver, 1280x800, AA off,
60 FPS cap; the complete replacement window and host prompts were inspected.
The updated split runtime was built and its lifecycle tests passed separately.

Linux regeneration and runtime/game-library builds passed for 1.9.0 with 193
resolved sites. The native font check passed with 118 text glyphs, translated
labels, four icon families, remapped pad/key/mouse inputs, Auto switching,
modified button sheets and overlay precedence. The UI-text check has no missing
or unused entries in the 13 translation tables.

Observed gameplay on an isolated save: Steam Deck VANGOGH, RADV Mesa
26.2 development driver, 1280x800, SMAA, 30 and 60 FPS caps:

- A full 9/5/4 gear inventory opened replacement for a natural Tree Branch.
  Back preserved the inventory and source actor.
- Reopening, selecting an unequipped Throwing Spear with Attack Up +5 and
  durability 1800, sorting, then confirming preserved the selected instance.
  The native pickup received the branch with durability 400. Quick dropping
  was disabled throughout.
- The outgoing spear existed in the world with the same durability and
  modifier. A reciprocal replacement at 30 FPS restored all original item
  signatures. Nintendo and PlayStation native-style prompts were inspected.

- A naturally placed chest containing an Emblazoned Shield retained its reward
  after Back and reopened normally. Sorting preserved the selected equipped
  Wooden Shield. Confirming completed the native outgoing detach, left that
  shield in the world with durability 1200, and awarded/equipped exactly one
  Emblazoned Shield with durability 1200. Gear counts returned to 9/5/4, the
  repeated card was skipped, and the native event finished. Quick dropping
  remained disabled.

- A natural iron chest containing a Spiked Boko Bow opened the bow selector
  with all five slots full. Replacing an unequipped Traveler's Bow with
  durability 1500 awarded one Spiked Boko Bow with durability 2000. All other
  item signatures, including arrows, stayed unchanged, and the native event
  finished. The outgoing bow was observed in the world with durability 1500.
  Keyboard prompts were inspected.

Ground first-discovery event handling is covered by synthetic checks; the live
checks above used known ground items. Windows gameplay has not been run.
Proton was not tested, and no 1.8.2
implementation or gameplay verification is claimed.
