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
void amiibo_picker_open_scan(uint64_t generation);
void amiibo_picker_cancel_scan(void);
bool nfp_scan_complete(uint64_t generation, const AmiiboPickerResult *result);
bool amiibo_picker_active(void);
bool amiibo_picker_take(AmiiboPickerResult *result);  // guest thread: commit the result after closing
void amiibo_picker_reset(void);
#ifdef __cplusplus
}
struct ImFont;
#include <string>
struct AmiiboPickerControls {
    std::string confirm = "Enter", cancel = "Esc", search = "Ctrl+F", clear = "Del";
    std::string favorite = "F", series = "[ / ]", tabs = "Q / E";
};
void amiibo_picker_set_controls(const AmiiboPickerControls &controls);
void amiibo_picker_set_fonts(ImFont *light, ImFont *bold, ImFont *edge);
void amiibo_picker_draw(float scale);  // render thread, inside the menu's ImGui frame
#endif
