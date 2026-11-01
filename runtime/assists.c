// Options > Accessibility (settings.h): the assists, each off by default. Their patches in the game code
// (tools/patches.py) ask here while it runs; a game version whose code has no place for one (tools/game_sites.py
// OPTIONAL_GROUPS) goes without it.
#include "settings.h"

// Infinite arrows: the bow's shot takes its arrows off the pouch through one function, which returns before taking
// any (tools/patches.py _remove_arrows). One arrow is still needed to shoot.
int rt_infinite_arrows(void) { return g_settings.infinite_arrows; }
