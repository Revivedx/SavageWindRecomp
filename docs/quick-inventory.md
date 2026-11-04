# Enhanced quick inventory (experimental)

Enable **Options → Gameplay → Enhanced quick inventory** for gear Drop, Sort,
Back and full-inventory replacement. This single option defaults off and can be
changed during play; no environment flags are required. Only the verified
Switch 1.9.0 build (prefix `cd57b23fa4bbad65`) is enabled.

The ordinary weapon, bow and shield quick selectors use logical Switch Y to
Drop, X for native Sort and B to close without equipping the highlighted entry,
including while the opening D-pad is held. Dropping unequipped gear rebuilds the
list and cursor while keeping the selector open. Dropping equipped gear closes
the selector so the native asynchronous equipment detach can finish. Both paths
create a native world item with its current durability and modifiers. Protected
gear, active events and forbidden game/player states remain guarded.

Sort uses the native category order and preserves the exact highlighted item,
equipped item and inventory counts, including when duplicate items are present.
The replacement cursor is gold; blue marks equipped gear.

## Full-inventory replacement

When a ground pickup or equipment chest has no available slot, the native
selector opens in the corresponding weapon, bow or shield category. Replace
uses logical Switch A, native Sort uses X and Cancel uses B. Equipped gear can
also be selected. Confirmation waits for native discard and, for equipped gear,
asynchronous actor detach before resuming the original pickup or chest award.
Incoming modifiers and progression remain native. Pending work consumes equip
and menu inputs until completion; a timeout does not pretend to undo a native
operation that may still be running.

Cancel leaves the incoming item on the ground or in the chest and allows the
interaction to be reopened. First-discovery and known ground items use the
native incoming-item card. Full chests show one incoming card above the lowered
selector; the initial chest window remains internal and is not drawn a second
time. The card shows the incoming stat and modifier aligned left, without an
Inventory+ label or a comparison against current gear. Native item names and
category dots remain. This is a native UI adaptation, not a pixel-exact replica
of another game's inventory.

The temporary card preview is restored before the chest flow resumes. The
repeated item-card action is suppressed only for the matching confirmed chest
event. Native full-inventory messages retain their animation above the reduced
selector blur. Gameplay Open/Take prompts are hidden during replacement and
return after closing it.

## Controls and assets

The ordinary Drop/Sort/Back and replacement Replace/Sort/Cancel rows share a
layout derived from the native 1280×720 quick-selector canvas. The controls row
reserves the right-stick guide's bounds plus a fixed gap. Chest controls move
with the item row. Disabled actions and different button families retain the
same vertical anchor; labels are centered by their measured width.

The UI uses fonts from the player's RomFS, with an interface-font fallback.
No proprietary game resources are included. The code generator routes inspected
direct calls and tail calls through runtime dispatch only for the verified
module/build. Regenerate the game library after updating `tools/gen.py`;
replacing the host executable alone is insufficient.

## Validation

Linux host checks, without game files:

```sh
make test-quick-inventory test-quick-replace test-input-focus test-control
python tools/test_quick_replace_codegen.py
python tools/i18n_check.py
```

The Python codegen check needs the generator's Python dependencies. It covers
exact-build gating, relocated targets, direct calls/tail calls and return-address
preservation. Host tests cover item identity, native sort categories,
restrictions, asynchronous completion, cancel/resume, preview restoration,
input consumption, settings and viewport-relative controls placement.

`make test-quick-inventory-font ROMFS=/path/to/romfs` checks fonts from the
player's files, including the synthetic parser checks; 114 native glyphs passed.
Developer gear-copy, chest and equipment-trace commands are described in
[Control commands](CONTROL.md).

Live Linux checks on an isolated save covered equipped ordinary drop and
repick for weapons, bows and shields, preserving their exact durability;
equipped ground replacement in all three categories; and sequential equipped
weapon, bow and shield chest replacements without resetting, plus cancel/reopen.
Modifier checks preserved Attack Up +5 with durability 1800 through equipped
replacement, and Durability Up (flags 2, value 2) with durability 2200 through
unequipped replacement. Duplicate-item selection was also checked.

Keyboard, Nintendo, Xbox and PlayStation button labels were inspected.
Live window captures checked viewport placement at 16:9, Steam Deck 16:10 and
ultrawide, in addition to synthetic layout checks. The disabled option was also
checked live: native selectors/cards remain, and Y leaves the pouch unchanged.

## Limitations

Unsupported builds do not yet expose an unavailable-state hint in Gameplay.
The feature has not been validated in gameplay on Windows or macOS.
