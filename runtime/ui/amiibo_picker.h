#pragma once
#include "../amiibo_library.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int selected;  // -1: cancelled; Favorites edits can still be saved
    bool favorites[AMIIBO_COUNT];
} AmiiboPickerResult;
void amiibo_picker_open(void);       // guest thread: snapshot the current settings
bool amiibo_picker_active(void);
bool amiibo_picker_take(AmiiboPickerResult *result);  // guest thread: commit the result after closing
void amiibo_picker_reset(void);
#ifdef __cplusplus
}
void amiibo_picker_draw(float scale);  // render thread, inside the menu's ImGui frame
#endif
