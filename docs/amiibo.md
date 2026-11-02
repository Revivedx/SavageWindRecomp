# Virtual amiibo prototype

In **Options > Gameplay**:

1. Enable the game's **amiibo** setting to make its rune available.
2. Turn **Virtual amiibo** on.
3. Choose an **amiibo figure**: Link (Archer), Zelda (Breath of the Wild), Guardian, or Bokoblin.
4. Return to play and use the amiibo rune on suitable ground. The selected figure is scanned automatically.

The virtual setting and figure selection are saved in `savagewind.ini`. Changes take effect without restarting;
changing the figure during a scan applies to the next scan. Each figure has its own stable virtual identity.
Turning Virtual amiibo off removes a detected figure and leaves the reader waiting without presenting one.

The game controls rewards and usage limits. No figure dumps or keys are needed. Archer and the Gameplay controls have been confirmed working in game; the other figures still need gameplay
verification. Epona and Wolf
Link are not included, and writable amiibo application data is not implemented.

For the original terminal workflow, `SAVAGEWIND_AMIIBO=archer` still seeds the startup settings. The values `zelda`,
`guardian`, `bokoblin`, and `off` are also supported. Gameplay controls can change these choices afterwards.

Developer checks: `make test-amiibo` covers the service's IPC responses, figure identity consistency, live
configuration, reset, and settings persistence across separate processes.
