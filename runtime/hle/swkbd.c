// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

static uint32_t next_char(const unsigned char **text) {
    const unsigned char *p = *text;
    unsigned n = *p < 0x80 ? 1 : *p >= 0xc2 && *p <= 0xdf ? 2 :
                 *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
    uint32_t ch = n == 1 ? *p : *p & (0x7f >> n);
    if (!n) goto invalid;
    for (unsigned i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) goto invalid;
        ch = (ch << 6) | (p[i] & 0x3f);
    }
    if ((n == 2 && ch < 0x80) || (n == 3 && ch < 0x800) || (n == 4 && ch < 0x10000) ||
        (ch >= 0xd800 && ch <= 0xdfff) || ch > 0x10ffff) goto invalid;
    *text = p + n;
    return ch;
invalid:
    *text = p + 1;
    return 0xfffd;
}

static void write_text(unsigned char *out, size_t cap, const char *text, bool utf8) {
    size_t used = 0, terminator = utf8 ? 1 : 2;
    if (cap < terminator) return;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t ch = next_char(&p);
        unsigned char bytes[4];
        size_t n;
        if (utf8) {
            if (ch < 0x80) {
                bytes[0] = (unsigned char)ch;
                n = 1;
            } else if (ch < 0x800) {
                bytes[0] = 0xc0 | (ch >> 6);
                bytes[1] = 0x80 | (ch & 0x3f);
                n = 2;
            } else if (ch < 0x10000) {
                bytes[0] = 0xe0 | (ch >> 12);
                bytes[1] = 0x80 | ((ch >> 6) & 0x3f);
                bytes[2] = 0x80 | (ch & 0x3f);
                n = 3;
            } else {
                bytes[0] = 0xf0 | (ch >> 18);
                bytes[1] = 0x80 | ((ch >> 12) & 0x3f);
                bytes[2] = 0x80 | ((ch >> 6) & 0x3f);
                bytes[3] = 0x80 | (ch & 0x3f);
                n = 4;
            }
        } else {
            uint16_t first = (uint16_t)ch, second = 0;
            n = 2;
            if (ch >= 0x10000) {
                ch -= 0x10000;
                first = (uint16_t)(0xd800 + (ch >> 10));
                second = (uint16_t)(0xdc00 + (ch & 0x3ff));
                n = 4;
            }
            bytes[0] = first & 0xff;
            bytes[1] = first >> 8;
            bytes[2] = second & 0xff;
            bytes[3] = second >> 8;
        }
        // Keep a whole character and its terminator, even when the byte capacity is odd.
        if (n > cap - terminator - used) break;
        memcpy(out + used, bytes, n);
        used += n;
    }
    memset(out + used, 0, terminator);
}

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}, capacity in bytes.
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0. +0x3bd selects UTF-8 output; otherwise UTF-16LE.
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
        // Horse naming leaves IsUseUtf8 clear and copies the result as 16-bit units (#271).
        write_text((unsigned char *)(uintptr_t)ptr, size, text, LD8(arg + 0x3bd) != 0);
    }
    c->x[0] = 0;
}
