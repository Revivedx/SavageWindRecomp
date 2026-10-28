// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3bd isUseUtf8,
// +0x3c0 / +0x3c4 initial text offset / length (UTF-16) in the work buffer at +0x4d0. The answer is UTF-16 unless
// isUseUtf8 is set: BOTW's horse names (1.8.2 FUN_7100c927b0, MakePreset leaves it off) read it as UTF-16 - in UTF-8
// "Satoru" came out as three characters the game's font doesn't have, an empty name.
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";
    for (uint32_t i = 0; wb && i < n && i < sizeof initial - 1; i++) {
        uint16_t ch = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        initial[i] = ch < 0x80 ? (char)ch : '?';
    }
    char text[96];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] name entry (max %d, initial \"%s\"): %s \"%s\"\n", max_len, initial, ok ? "entered" : "cancelled", text);
    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, size);
        if (*(const uint8_t *)(uintptr_t)(arg + 0x3bd)) {
            snprintf((char *)(uintptr_t)ptr, size, "%s", text);
        } else {  // UTF-16: the typed UTF-8 decoded (characters past U+FFFF as surrogate pairs), NUL-terminated
            uint16_t *o = (uint16_t *)(uintptr_t)ptr;
            size_t k = 0, cap16 = size / 2;
            for (const unsigned char *p = (const unsigned char *)text; *p && k + 1 < cap16;) {
                uint32_t cp = *p++;
                int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
                if (more) cp &= 0x3F >> more;
                for (int i = 0; i < more && (*p & 0xC0) == 0x80; i++) cp = cp << 6 | (*p++ & 0x3F);
                if (cp >= 0x10000) {
                    if (k + 2 >= cap16) break;
                    cp -= 0x10000;
                    o[k++] = (uint16_t)(0xD800 | cp >> 10);
                    cp = 0xDC00 | (cp & 0x3FF);
                }
                o[k++] = (uint16_t)cp;
            }
        }
    }
    c->x[0] = 0;
}
