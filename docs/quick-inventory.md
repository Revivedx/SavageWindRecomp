# Enhanced quick inventory (experimental)

Enable **Options → Gameplay → Enhanced quick inventory** to add Drop and Back
in the gear quick selector. The option defaults off and can be changed during
play. Drop uses the existing logical Switch Y binding; Back closes without
equipping the highlighted entry, including while the opening D-pad is held.

Only the verified Switch 1.9.0 build (prefix `cd57b23fa4bbad65`) is enabled.
Unequipped swords, bows and shields can be dropped. Equipped items, protected
items and forbidden game/player states are rejected. The quick selector stays
open and its list and cursor are rebuilt after a successful discard.

## Full-inventory replacement

Replacement remains a separate opt-in experiment. Enable enhanced quick
inventory, then start with `SAVAGEWIND_QUICK_REPLACE=1` and
`SAVAGEWIND_QUICK_REPLACE_EXECUTE=1`. Without the execute flag, confirmation
leaves the inventory unchanged. `SAVAGEWIND_QUICK_REPLACE_TRACE=1` adds
receive/capacity diagnostics.

When a ground pickup or weapon chest has no available inventory slot, the
native selector opens in the corresponding equipment category. Replace uses
the logical Switch A binding; Cancel uses B. Confirmation discards the chosen
eligible item and resumes the original pickup or chest award. Cancel leaves
the incoming item on the ground or in the closed chest. Equipped and protected
items remain unavailable for replacement.

Chest replacement displays the native incoming-item card above a lowered
selector. The preview stages only the temporary UI item record and restores
it before the original chest flow resumes. Award, modifiers and progression
remain native; the repeated item-card action is suppressed only for the
matching confirmed chest event. Native full-inventory messages render above
the selector blur and retain their animation. Gameplay action prompts such
as Open and Take are hidden during replacement and return after closing it.

The code generator routes the inspected direct calls through runtime dispatch
only for the verified module/build. Regenerate the game library after updating
`tools/gen.py`; replacing the host executable alone is insufficient.

## Controls layout

Drop/Back and Replace/Cancel use one layout derived from the native 1280×720
quick-selector canvas. The controls row reserves the right-stick guide's bounds
plus a fixed gap. Chest controls use exactly the same downward translation as
the item row. Single-action categories, disabled actions and different button
families retain the same vertical anchor; labels are centered by their measured
width. The UI uses fonts from the player's RomFS, with interface-font fallback.
No game resources are included.

## Validation

Linux host checks, without game files:

```sh
make test-quick-inventory test-quick-replace test-input-focus
python tools/test_quick_replace_codegen.py
python tools/i18n_check.py
```

The Python codegen check needs the generator's Python dependencies. It covers
exact-build gating, relocated targets, direct calls/tail calls and return-address
preservation. Host tests cover item identity and restrictions, category selection,
cancel/resume, UI-preview restoration, native-toast timing, prompt suppression,
input consumption, settings and viewport-relative controls placement.

`make test-quick-inventory-font ROMFS=/path/to/romfs` additionally checks fonts
from the player's files, including the existing synthetic parser checks.

Live Linux checks on an isolated save covered ordinary weapon/bow/shield and
arrow/rune selectors, ground replacement, chest preview, cancel/reopen and a
successful chest replacement. Replacing a Boko Club granted one Soldier's
Claymore with attack 20. The equipped sword remained protected. Gameplay
Open/Take prompts disappeared during replacement and returned after cancel.
Controls were inspected with keyboard, Nintendo, Xbox and PlayStation labels.
Viewport math was checked for 16:9, Steam Deck 16:10 and ultrawide; those latter
aspect-ratio checks are synthetic rather than separate live captures.

## Remaining integration work

Version-specific addresses remain local to the experimental modules. Unsupported
builds do not yet expose an unavailable state in Gameplay. Menu override ownership
still needs an agreed integration point, legacy diagnostic modes need cleanup,
and hint eligibility uses fewer checks than execution. Equipped-item replacement
requires handling the native asynchronous equipment-discard path. Bow/shield
chest variants and first-discovery edge cases need broader live coverage; the
latest replacement changes have not been validated on Windows or macOS.
