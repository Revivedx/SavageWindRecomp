// Pinyin for the menus' search fields: see runtime/pinyin.h. The table is the Unicode Han Database's kMandarin
// (runtime/pinyin_data.inc), so nothing here comes from the game: a character the table has no reading for (or one
// that is no hanzi at all) simply keeps its own text in the search string.
#include "pinyin.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "pinyin_data.inc"

// The reading of a codepoint, NULL when the table has none
static const char *reading_of(uint32_t cp) {
    int lo = 0, hi = PINYIN_CODEPOINTS - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t c = g_pinyin_codepoints[mid];
        if (c == cp) return g_pinyin_readings + g_pinyin_offsets[mid];
        if (c < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// The next UTF-8 codepoint, *p moved past it; -1 for a byte that isn't the start of one (one byte on)
static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (!more) { *p += 1; return c; }
    for (int k = 1; k <= more; k++)
        if ((s[k] & 0xc0) != 0x80) { *p += 1; return (uint32_t)-1; }
    uint32_t cp = more == 3 ? c & 0x07 : more == 2 ? c & 0x0f : c & 0x1f;
    for (int k = 1; k <= more; k++) cp = cp << 6 | (s[k] & 0x3f);
    *p += more + 1;
    return cp;
}

static void put(char *out, size_t cap, size_t *at, bool *fits, char c) {
    if (*at + 1 < cap) out[(*at)++] = c;
    else *fits = false;
}

// A letter of the table and one of the search: the case is ignored, and a v typed for ü matches the u the table
// writes (a Chinese IME's ü key is v; the other way round would fold the v of every Latin name)
static bool same_letter(unsigned char have, unsigned char want) {
    if (have >= 'A' && have <= 'Z') have = (unsigned char)(have - 'A' + 'a');
    if (want >= 'A' && want <= 'Z') want = (unsigned char)(want - 'A' + 'a');
    return have == want || (have == 'u' && want == 'v');
}

bool pinyin_text(const char *utf8, char *out, size_t cap) {
    size_t at = 0;
    bool fits = true;
    if (!cap) return false;
    out[0] = 0;
    for (const char *p = utf8; *p;) {
        const char *start = p;
        uint32_t cp = utf8_next(&p);
        const char *r = reading_of(cp);
        if (r) {
            if (at) put(out, cap, &at, &fits, ' ');  // (syllables apart: what pinyin_text gives out reads back)
            for (; *r; r++) put(out, cap, &at, &fits, *r);
        } else if (cp < 0x80) {
            put(out, cap, &at, &fits, (char)tolower((int)cp));
        } else {  // no reading: kana, hangul, a broken byte, ... keep the bytes it was
            for (const char *q = start; q < p; q++) put(out, cap, &at, &fits, *q);
        }
    }
    out[at] = 0;  // (put never goes past cap - 1)
    return fits;
}

bool pinyin_match(const char *pinyin, const char *needle) {
    while (*needle == ' ') needle++;
    if (!*needle) return true;
    for (; *pinyin; pinyin++) {
        if (*pinyin == ' ') continue;
        const char *h = pinyin, *n = needle;
        for (;;) {
            while (*n == ' ') n++;
            while (*h == ' ') h++;
            if (!*n) return true;
            if (!same_letter((unsigned char)*h, (unsigned char)*n)) break;
            h++, n++;
        }
    }
    return false;
}
