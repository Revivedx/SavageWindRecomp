// Launch options at start and at PLAY (the platform side of runtime/launch_flags.h).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// First thing in main() after the log is set up: unless this is the configuration screen (--config, which always starts
// clean so a bad flag can't keep a player out of it) or --no-launch-flags was given, puts the saved flags in the
// environment and restarts the process once so every reader, constructors included, sees them. Does nothing when no
// flags are saved.
void launch_flags_boot(int argc, char **argv);
// --no-launch-flags was given: the saved flags are ignored for this start (and PLAY doesn't restart for them)
bool launch_flags_disabled(void);
// <data root>/launch-flags.txt, also from the start screen (where the working directory is the data root)
bool launch_flags_path(char *out, size_t n);
// PLAY on the start screen: if the saved flags differ from the ones this process started with, restarts straight into
// the game (--play) with them. Returns when nothing needed to change, or the restart failed. root: the game folder
// argument of this start ("extracted" is the default).
void launch_flags_play(const char *root);

#ifdef __cplusplus
}
#endif
