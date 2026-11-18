// Cheats > Sure-footed horse (settings.h sure_footed): a running horse no longer stops dead and rears (Crash or
// Courbette) when it runs into something. The patches at the horse's crash check and at its stop check's look-ahead
// ask here (tools/patches.py _horse_crash, _horse_look_ahead); with the option on no obstacle stops the horse.
#include "settings.h"

int rt_sure_footed(void) { return g_settings.sure_footed; }
