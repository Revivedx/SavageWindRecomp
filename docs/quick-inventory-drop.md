# Quick inventory dropping

Enable **Gameplay → Quick inventory dropping**, off by default. This feature
supports the verified BOTW 1.9.0 build only. It is independent of the sorting
setting.

Press the button mapped to Switch Y in the weapon, bow or shield quick selector
to discard the highlighted item through the game's native operation. The hint
uses the same fonts, custom bindings, icon families and overlay policy as
[sorting](quick-inventory-sort.md). B closes the selector without equipping a
different highlighted item. Protected items and native event/player restrictions
remain enforced.

Unequipped drops refresh the open selector. Equipped drops close it and wait
for native actor detach/placement. Retained actor references use generation
checks; a transaction token ties the actual native consumer call to the matching
request. Pending work consumes equipment/menu actions until completion and
continues draining if the option is switched off. A timeout logs recovery state
without pretending the native operation was rolled back.

The source uses version-aware sites and checked instruction patches for menu
callbacks, the UI-manager update and the existing native virtual actor call.
`tools/gen.py` is unchanged. Regenerate and rebuild the game library before
gameplay testing. No spawn fixtures, equipment traces or diagnostic activation
environment variables are part of this feature.

Synthetic tests, without game files:

```sh
make test-quick-drop test-quick-inventory-sort test-input-focus test-pc-controls
python tools/test_quick_drop_codegen.py
python tools/test_quick_inventory_sort_codegen.py
python tools/test_game_sites.py
python tools/i18n_check.py
```

The Python codegen checks need lz4, capstone, unicorn and clang. Tests exercise
live item identity, protected/native restrictions, asynchronous detach, cached
equipment restoration, queue/consumer interleaving, invalidated item pages,
actor generations and transaction tokens, complete HID input filtering, menu
edges and Back priority. Native-call codegen checks preserve LR, native results,
FP registers and flags with and without register locals.

Linux gameplay was checked on Steam Deck, BOTW 1.9.0, RADV VANGOGH with Mesa
26.2.0-devel, a 1280×800 window, SMAA and a 60 FPS cap, using copied saves/settings.
Native equipped weapon, shield and bow drops completed and were picked up again
with Life 2000, 1200 and 2200. Unequipped drops/repicks covered Attack Up +5 at
Life 1800, Durability Up at Life 2200, a shield at Life 500 and a bow at Life 4500.
The selected gear retained its durability/modifiers and gear counts returned to
9/5/4. Back preserved equipment; feature-off Y and enabling with Y already held
left the pouch unchanged. Sorting and dropping together retained exact selection
and showed the combined native hint, including PlayStation artwork.

A bow test setup released a drawn equipped bow and fired an unrelated arrow;
the dropped Phrenic Bow was still recovered with its original Life 4500. This
input setup is distinguished from item preservation through native discard and
pickup. Windows gameplay remains unverified.
