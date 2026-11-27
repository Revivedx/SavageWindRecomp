// Cheats > Improved horse controls (settings.h improved_horse), with runtime/improved_horse.c: a moving horse no
// longer stops dead and rears (Crash, Courbette or the short Dislike) when it runs into something, at any gear. The
// patches at the horse's crash check and at its stop check ask here (tools/patches.py _horse_crash,
// _horse_stop_check); with the option on no obstacle stops the horse.
#include "settings.h"

int rt_sure_footed(void) { return g_settings.improved_horse; }
