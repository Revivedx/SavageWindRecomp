// Mods (runtime/mods.c, docs/MODS.md): applying what is in mods/ when the game boots, and turning them on and off.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

void mods_sync(void);  // before a boot, in the data folder

// The mods in mods/ and mods/disabled/, by title
void mods_scan(void);
int mods_count(void);
const char *mods_title(int i);
bool mods_on(int i);
void mods_set(int i, bool on);  // moves its file; the game applies it the next time it boots
bool mods_pending(void);        // a mod was turned on or off since mods_scan

#ifdef __cplusplus
}
#endif
