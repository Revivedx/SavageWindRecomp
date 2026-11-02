// Options > Cheats (runtime/ui/game_options.mm): what the game code asks while they are on (tools/patches.py).
//
// Infinite Durability: a weapon actor's hit update (1.9.0 0x70007b08f4) hands its life (+0xd4c, from the actor's
// life pointer, vtable +0x2b8) to its damage component (actor +0xe28, vtable +0x78), which takes the hit's wear off
// it, then breaks the weapon at 0. Swords, spears, bows and shields all go through it. The call still runs (whatever
// else the component keeps stays as it is); the life is put back after it and the update told it didn't change.
//
// Infinite Arrows: PauseMenuDataMgr::removeArrow (1.9.0 0x7000d93814), which takes the arrows shot off the pouch
// (and the game data), returns at once.
#include <stdbool.h>
#include <stdint.h>

#include "../settings.h"

int rt_cheat_durability(void) { return g_settings.cheat_infinite_durability; }
int rt_cheat_arrows(void) { return g_settings.cheat_infinite_arrows; }
