// runtime/pinyin.c: the readings of the table, and the search that reads them. No game files: the texts below are
// ordinary Chinese ("luyu" for 鲈鱼 is the case the search exists for), not the game's names.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pinyin.h"

static int failures;

static void check(bool ok, const char *what, const char *got) {
    if (!ok) printf("FAIL %s%s%s\n", what, got ? ": got \"" : "", got ? got : "");
    if (!ok) failures++;
}

// The search as the menus do it: does `text`'s pinyin hold `needle`?
static void expect(const char *text, const char *needle, bool want) {
    char py[256];
    pinyin_text(text, py, sizeof py);
    char what[512];
    snprintf(what, sizeof what, "\"%.100s\" <- \"%.100s\" (pinyin \"%.100s\")", needle, text, py);
    check(pinyin_match(py, needle) == want, what, want ? "no match" : NULL);
}

int main(void) {
    char py[256];

    // The readings themselves
    pinyin_text("鲈鱼", py, sizeof py);
    check(!strcmp(py, "lu yu"), "鲈鱼 -> lu yu", py);
    pinyin_text("烤鲈鱼", py, sizeof py);
    check(!strcmp(py, "kao lu yu"), "烤鲈鱼 -> kao lu yu", py);
    pinyin_text("冰冷生命鲈鱼", py, sizeof py);
    check(!strcmp(py, "bing leng sheng ming lu yu"), "冰冷生命鲈鱼", py);

    // One syllable is enough, spaced or not, and the case doesn't matter
    expect("烤鲈鱼", "yu", true);
    expect("烤鲈鱼", "luyu", true);
    expect("烤鲈鱼", "lu yu", true);
    expect("烤鲈鱼", "kaolu", true);
    expect("烤鲈鱼", "KAO LU YU", true);
    expect("烤鲈鱼", "kaoluyu", true);
    expect("烤鲈鱼", "luy", true);   // (a prefix of the next syllable: as the name search does it)
    expect("烤鲈鱼", "luu", false);
    expect("烤鲈鱼", "kaolou", false);
    expect("烤鲈鱼", "lu yu kao", false);  // (the order counts, as in a substring search)

    // Characters with no reading, and no hanzi at all, stay what they are
    pinyin_text("精力鲈鱼 x", py, sizeof py);
    check(!strcmp(py, "jing li lu yu x"), "精力鲈鱼 x", py);
    expect("精力鲈鱼 x", "x", true);
    expect("精力鲈鱼 x", "luyux", true);
    pinyin_text("Hylian Shield", py, sizeof py);
    check(!strcmp(py, "hylian shield"), "ASCII stays (lowercased)", py);
    expect("Hylian Shield", "hylian", true);
    expect("Hylian Shield", "hylianshield", true);
    pinyin_text("アカリ", py, sizeof py);  // (kana: no reading, the bytes are kept)
    check(!strcmp(py, "アカリ"), "kana kept", py);

    // ü is written u in the table, and typed u or v (a Chinese IME's ü key is v)
    pinyin_text("女", py, sizeof py);
    check(!strcmp(py, "nu"), "ü -> u (女)", py);
    expect("女", "nv", true);
    expect("女", "nu", true);

    // An empty search matches everything, as has_text
    expect("烤鲈鱼", "", true);
    expect("烤鲈鱼", "   ", true);

    // A broken or cut short text: never reads past its end, and still terminates
    check(pinyin_text("鲈", py, 1) == false && py[0] == 0, "cap 1 -> empty", py);
    check(!pinyin_text("烤鲈鱼", py, 4), "small cap reports no room", py);
    check(py[0] && strlen(py) < 4, "small cap keeps what fit", py);
    char broken[4] = {0};
    check(pinyin_text("\xe9\x9c", broken, sizeof broken) && !memcmp(broken, "\xe9\x9c", 2),
          "broken UTF-8 is copied, not read past", broken);
    char emoji[8] = {0};
    check(pinyin_text("\xf0\x9f\x98\x80", emoji, sizeof emoji) && !memcmp(emoji, "\xf0\x9f\x98\x80", 4),
          "above U+FFFF has no reading: kept", emoji);

    // The table is what tools/gen_pinyin_table.py describes: sorted codepoints, readings inside the pool
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
