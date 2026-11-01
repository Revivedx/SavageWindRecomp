# Enhanced quick inventory (experimental)

Enable **Options → Gameplay → Enhanced quick inventory** to add Drop and Back
in the gear quick selector. The option defaults off and can be changed during
play. Drop uses the existing logical Switch Y binding; Back closes without
equipping the highlighted entry, including while the opening D-pad is held.

Only the verified Switch 1.9.0 build (prefix `cd57b23fa4bbad65`) is enabled.
Unequipped swords, bows and shields can be dropped. Equipped items, protected
items and forbidden game/player states are rejected. The quick selector stays
open and its list and cursor are rebuilt after a successful discard.

The UI hints use bitmap fonts from the player's RomFS, with interface-font
fallback. No game resources are included in this change.

## Validation

On Linux, `make test-quick-inventory` exercises mocked native discard and list
refresh, identity checks, rejection paths, input consumption and persisted
settings. It requires the normal build dependencies, but no game files.
`make test-quick-inventory-font ROMFS=/path/to/romfs` additionally checks fonts
from the player's files.

The widget-count regression checks ordinary swords, shields, bows and the
special sword mapping. Ordinary swords also have an unequip slot; excluding
it from the widget count hid the final retained item after dropping another.
The revised count follows the native mapping.

## Draft follow-up

Before marking the PR ready: consolidate the version-specific addresses,
expose unsupported-build availability in Gameplay, settle shared ownership of
the two Menu hooks, separate legacy research environment modes and logging
from normal execution, and align hint eligibility with all native restrictions.
Broaden synthetic font-parser tests and validate Windows, locale layouts and
aspect ratios. The latest widget-count fix still needs an in-game recheck.
