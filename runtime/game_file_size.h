// Informational logical size of the recognized extracted game files. Never used to authorize deletion.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned long long bytes;
    bool complete;   // false on an access/enumeration error; bytes is then only a partial total
    bool cancelled;
} RtGameFilesSize;

// The caller owns game_dir for the duration of the call. Windows paths are UTF-8.
// cancelled may be NULL; otherwise it is polled between entries and directory operations.
RtGameFilesSize rt_game_files_size_scan(const char *game_dir, bool (*cancelled)(void *), void *user);
// Legacy synchronous total; callers needing error/cancellation status use the scan API above.
unsigned long long rt_game_files_size(const char *game_dir);

#ifdef __cplusplus
}
#endif
