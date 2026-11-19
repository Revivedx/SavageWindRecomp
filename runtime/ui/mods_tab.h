// The Mods tab of Settings (runtime/ui/mods_tab.mm), drawn by menu.mm as tab 11 (both Prelaunch Settings and
// the in-game menu). Enables/disables mod folders, reorders priority, and inspects file conflicts.
#pragma once

void mods_tab_draw(bool in_game, bool *want_restart);
void mods_tab_invalidate(void);
