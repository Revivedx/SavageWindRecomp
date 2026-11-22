// Pinyin for the menus' search fields (runtime/pinyin.c): the reading of each hanzi, from the Unicode Han Database
// (runtime/pinyin_data.inc, tools/gen_pinyin_table.py). A controller types on the on-screen keyboard, which has no
// IME and only ASCII keys (runtime/ui/osk.mm), so a Chinese name is otherwise impossible to search for: with this,
// "luyu" finds 鲈鱼 and everything cooked from it.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A capacity of this many times the input's UTF-8 byte capacity (including NUL) always fits.
enum { PINYIN_UTF8_EXPANSION = 3 };

// The readings of a UTF-8 text, syllable by syllable ("烤鲈鱼" -> "kao lu yu"), with V marking ü for pinyin_match.
// Characters without a reading keep their bytes (ASCII letters lowercased). Writes at most cap - 1 bytes and a NUL.
// Returns false when the text needed more room (out then holds what fit).
bool pinyin_text(const char *utf8, char *out, size_t cap);

// Does the pinyin of a name (what pinyin_text writes) hold `needle`? Spaces are left out of the comparison, so
// "luyu", "lu yu", "lu" and "kaolu" all find 烤鲈鱼. The case is ignored and both u and v match a pinyin ü.
// An empty needle matches, like has_text.
bool pinyin_match(const char *pinyin, const char *needle);

#ifdef __cplusplus
}
#endif
