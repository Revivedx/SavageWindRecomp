# Quick inventory sorting

Enable **Options → Gameplay → Quick inventory sorting**. It defaults off and
can be changed during play. This feature supports BOTW 1.9.0.

In the weapon, bow or shield quick selector, press the button mapped to Switch
X to use the game's native sorting order. The Sort hint follows the keyboard
or controller binding and the selected button-icon family. Other selector
categories keep their normal controls.

The hint uses the game's own font and button artwork from the player's RomFS,
with the interface font as a fallback. No game assets ship with the feature.
It uses the same icon renderer as the game's prompts: changing Button icons,
the last device in Automatic mode, or a raw Switch binding updates the hint.
Font archive replacements from `SAVAGEWIND_ROMFS_OVERLAY` take precedence over
the base archive; modified button sheets retain their supplied artwork, as in
the game renderer. Mods that replace only individual layout textures need
separate verification.

Sorting preserves the exact highlighted item, including duplicate names with
different modifiers or durability. The equipped item and inventory counts stay
the same. Holding X does not repeat sorting, and A, B and Y take priority over
a simultaneous sort request. Native event and modal restrictions still apply.

The hooks use version-aware sites and checked instruction patches. There is no
change to `tools/gen.py` or to how it emits direct calls. Regenerate and rebuild
the game library after updating: replacing the runtime alone is insufficient.
If the complete optional site group cannot be located, sorting is unavailable
and loading the game does not depend on these helpers.

Synthetic checks, without game files:

```sh
make test-quick-inventory-sort test-input-focus test-pc-controls
python tools/test_quick_inventory_sort_codegen.py
python tools/test_game_sites.py
python tools/test_layout.py
python tools/i18n_check.py
```

The Python generator checks need the generator's environment (lz4, capstone,
unicorn); the codegen check also needs clang. Runtime tests cover instance
identity, equipment, full selectors, the unequip cell, input priority, locks,
register preservation and rejection of stale UI pointers. Optional-site loading
and patched direct calls are tested separately. The optional local font check,
`make test-quick-inventory-font ROMFS=/path/to/your/romfs`, checks the native
font, four icon families, key/mouse/controller remaps and overlay precedence.

Linux gameplay was checked on Steam Deck, BOTW 1.9.0, RADV VANGOGH with Mesa
26.2.0-devel, a 1280×800 window and a 60 FPS cap, using copied saves/settings.
Weapon, bow and shield sorts kept the exact highlighted and equipped instances
and counts (9/5/4); duplicate weapons retained their identity, durability and
modifiers. Holding X sorted once; release/repress sorted again. With the option
off, X left the weapon selector unchanged. The restored native hint was checked
live with Nintendo, Xbox, PlayStation and keyboard icons. Synthetic font tests
also covered Automatic device changes, remaps and modified sheets. No specific
third-party icon mod or Windows gameplay has been verified.
