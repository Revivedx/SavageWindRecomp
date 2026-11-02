# Virtual amiibo prototype

In **Options > Gameplay**:

1. Enable the game's **amiibo** setting to make its rune available.
2. Turn **Virtual amiibo** on.
3. Choose an **amiibo figure**: Link (Archer), Zelda (Breath of the Wild), Guardian, or Bokoblin.
4. Return to play and use the amiibo rune on suitable ground. The selected figure is scanned automatically.

The virtual setting and figure selection are saved in `savagewind.ini`. Changes take effect without restarting;
changing the figure during a scan applies to the next scan. Each figure normally has its own stable virtual identity.
Turning Virtual amiibo off removes a detected figure and leaves the reader waiting without presenting one.

**Repeat amiibo scans**, also in Gameplay, is an optional saved setting (off by default). Turn it on to try scanning
the same figure repeatedly without the daily wait. Each scan presents a new virtual serial number for the selected
model, as though scanning another copy of that figure. Its serial stays unchanged throughout that scan. Turning
repeat scans off restores the figure's original identity on the next scan; its usual daily limit may still apply.
Repeat scans have been confirmed working in game. The game continues to choose the rewards and check where the
rune can be used.

No figure dumps or keys are needed. All four figures and the Gameplay controls have been confirmed working in game.
Epona and Wolf Link are not included, and writable amiibo application data is not implemented.

For the original terminal workflow, `SAVAGEWIND_AMIIBO=archer` still seeds the startup settings. The values `zelda`,
`guardian`, `bokoblin`, and `off` are also supported. Gameplay controls can change these choices afterwards.

Developer checks: `make test-amiibo` covers the service's IPC responses, figure identity consistency, live
configuration, repeat identities, reset, and settings persistence across separate processes.
