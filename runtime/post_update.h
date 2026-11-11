// What the game does around an update: on Windows SW Update (runtime/platform/windows_update.c) installs it, in a Linux
// AppImage the game itself does (runtime/update_appimage.c). In a source build nothing here does anything.
//
// After an update, the first start of the new version: the shader caches are emptied before Vulkan opens them, the
// game code is built if this version needs it, every shader is precompiled again, and only then is the update
// finished (cache/last-version.txt) and the previous version's files removed. A zip unpacked by hand over an older copy
// comes the same way. Cut off at any point, the next start carries on.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POST_UPDATE_HEALTH_EXIT 73  // savagewind.exe --update-health: it starts, so its DLLs are all there

// An update cut off halfway (a power cut): true once SW Update has put the old files back and started this program again.
bool post_update_recover_if_cut(void);
bool post_update_shipped(void);                      // a release (sw-manifest.txt next to the exe), not a source build
bool post_update_pending(const char *version);       // game files are set up and this version hasn't finished setting up
void post_update_reset_shaders(const char *version); // before Vulkan starts; once per version
void post_update_finish(const char *version);        // after a good setup: remembers the version, removes update/
void post_update_tidy(const char *version);          // what a busy file kept post_update_finish from removing
void post_update_mark_failed(const char *version);   // not retried by itself at every start
bool post_update_failed_before(const char *version);
bool post_update_can_rollback(void);                 // the previous version's files are still there

typedef enum { UPDATE_IDLE, UPDATE_CHECKING, UPDATE_AVAILABLE, UPDATE_CURRENT, UPDATE_ERROR } UpdateState;

void update_check_start(void);  // asks SW Update, in the background (no network code in the game itself)
UpdateState update_check_state(char *version, size_t version_size, char *message, size_t message_size);
// Starts SW Update to install it / to go back; true: this program must quit now so its files can be replaced.
bool update_apply_start(void);
bool update_rollback_start(void);
// Linux: the update runs inside this program, on a thread of the caller's. update_apply_here returns only if it failed,
// saying why; when it works the new version replaces this process. update_apply_progress: 0..1, -1 when not running.
bool update_applies_here(void);
void update_apply_here(char *title, size_t title_size, char *what, size_t what_size);
float update_apply_progress(void);

#ifdef __cplusplus
}
#endif
