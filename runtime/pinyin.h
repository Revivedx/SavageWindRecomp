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

// The readings of a UTF-8 text, syllable by syllable with a space between ("烤鲈鱼" -> "kao lu yu"). A character the
// table has no reading for, and every other byte, is kept as it is (ASCII letters lowercased). Writes at most cap - 1
// bytes and a NUL; false when the text needed more room (out then holds what fit).
bool pinyin_text(const char *utf8, char *out, size_t cap);

// Does the pinyin of a name (what pinyin_text writes) hold `needle`? Spaces are left out of the comparison, so
// "luyu", "lu yu", "lu" and "kaolu" all find 烤鲈鱼; the case is ignored, as in the plain search, and a v typed for
// ü (a Chinese IME's key for it) matches the u the table writes. An empty needle matches, like has_text.
bool pinyin_match(const char *pinyin, const char *needle);

#ifdef __cplusplus
}
#endif
