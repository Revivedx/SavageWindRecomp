// Automatic update checking via GitHub releases API (runtime/updater.h).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the asynchronous update check thread.
void updater_init(void);

// Returns true if a newer release is available.
bool updater_has_update(void);

// Returns the latest version tag (e.g., "v0.5.1" or "0.5.1").
const char *updater_latest_version(void);

// Returns the html URL of the latest release.
const char *updater_release_url(void);

#ifdef __cplusplus
}
#endif

