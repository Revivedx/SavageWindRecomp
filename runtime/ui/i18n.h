// Menu languages (runtime/ui/i18n.cpp): the start screen and the overlay menu follow the game's text language
// (Language > Text; Auto: the system's display language). tr("English text") is that text in the menu language: the
// English text is the key, the tables are runtime/ui/i18n/<lang>.inc (tools/i18n_check.py lists what each lacks).
// Format strings translate the same way and keep their conversions in order.
#pragma once
#include <string>

struct ImFontAtlas;
struct ImFontConfig;

const char *tr(const char *en);
// UTF-8 upper case (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic; other scripts as they are)
std::string tr_upper(const char *s);
// The menu language ("en", "es", "fr", "de", "it", "nl", "ru", "pt", "ja", "ko", "zh-Hans", "zh-Hant", "th", "he")
// and a number that changes with it: a UI context whose fonts were made for another number makes them again
// (ui_fonts_*).
const char *ui_lang(void);
int ui_lang_generation(void);
// Right to left (Hebrew): the texts are in reading order, and ImGui draws each line right to left (i18n.mm sets
// GImTextReorderLine, third_party/imgui). For text something else draws left to right as it is (the game's own
// Options pages): `s` with each line in visual order, left to right on screen. Other menu languages: `s` as it is.
std::string ui_visual_order(const char *s);
// After each font a UI context adds (AddFontFromFileTTF at `size`): merges a system font with the glyphs the menu
// language's texts use that Latin-1 fonts lack (Cyrillic, kana and kanji, hangul, hanzi, Thai, Hebrew). Returns false
// if none needed.
bool ui_merge_fallback_font(ImFontAtlas *atlas, float size, const ImFontConfig *base);
