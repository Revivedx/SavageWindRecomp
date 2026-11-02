#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
// Non-repeating key press starts a three-second hold; release or focus loss cancels it.
void blood_moon_key(bool down);
void blood_moon_frame(void);
#ifdef __cplusplus
}
#endif
