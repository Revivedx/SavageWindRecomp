// Vulkan adapter indices follow the startup log's zero-based enumeration.
#pragma once
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// -1 is automatic; reject malformed values rather than accidentally selecting GPU 0.
static inline int gpu_index_parse(const char *value) {
    if (!value || !*value || !strcmp(value, "auto")) return -1;
    for (const char *p = value; *p; p++) if (*p < '0' || *p > '9') return -1;
    char *end;
    errno = 0;
    long index = strtol(value, &end, 10);
    return errno || *end || index > INT_MAX ? -1 : (int)index;
}
