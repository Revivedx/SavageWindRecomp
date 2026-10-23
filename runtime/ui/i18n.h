// Menu languages (runtime/ui/i18n.cpp): the start screen and the overlay menu follow the game's text language
// (Language > Text; Auto: the system's display language). tr("English text") is that text in the menu language: the
// English text is the key, the tables are runtime/ui/i18n/<lang>.inc (tools/tests/i18n_check.py lists what each lacks).
// Format strings translate the same way and keep their conversions in order.
#pragma once
#include <string>

struct ImFontAtlas;
struct ImFontConfig;

const char *tr(const char *en);
// UTF-8 upper case (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic; other scripts as they are)
std::string tr_upper(const char *s);
// The menu language ("en", "es", "fr", "de", "it", "nl", "ru", "pt", "ja", "ko", "zh-Hans", "zh-Hant") and a number
// that changes with it: a UI context whose fonts were made for another number makes them again (ui_fonts_*).
const char *ui_lang(void);
int ui_lang_generation(void);
// After each font a UI context adds (AddFontFromFileTTF at `size`): merges a system font with the glyphs the menu
// language's texts use that Latin-1 fonts lack (Cyrillic, kana and kanji, hangul, hanzi). Returns false if none needed.
bool ui_merge_fallback_font(ImFontAtlas *atlas, float size, const ImFontConfig *base);
