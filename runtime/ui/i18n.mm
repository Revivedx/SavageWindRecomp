// Menu languages: see i18n.h. The tables are compiled in (runtime/ui/i18n/<lang>.inc, one T("English", "...") per
// text); the menu language is looked up again each frame, so choosing another text language switches the menus at
// once (the start screen and the overlay rebuild their fonts when the new language needs other glyphs).
#include "i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "imgui.h"
extern "C" {
#include "../language.h"
#include "../paths.h"
}

namespace {
struct Entry { const char *en, *tr; };
#define T(a, b) {a, b},
const Entry es[] = {
#include "i18n/es.inc"
    {nullptr, nullptr}};
const Entry fr[] = {
#include "i18n/fr.inc"
    {nullptr, nullptr}};
const Entry de[] = {
#include "i18n/de.inc"
    {nullptr, nullptr}};
const Entry it[] = {
#include "i18n/it.inc"
    {nullptr, nullptr}};
const Entry nl[] = {
#include "i18n/nl.inc"
    {nullptr, nullptr}};
const Entry ru[] = {
#include "i18n/ru.inc"
    {nullptr, nullptr}};
const Entry pt[] = {
#include "i18n/pt.inc"
    {nullptr, nullptr}};
const Entry ja[] = {
#include "i18n/ja.inc"
    {nullptr, nullptr}};
const Entry ko[] = {
#include "i18n/ko.inc"
    {nullptr, nullptr}};
const Entry zh_hans[] = {
#include "i18n/zh-Hans.inc"
    {nullptr, nullptr}};
const Entry zh_hant[] = {
#include "i18n/zh-Hant.inc"
    {nullptr, nullptr}};
const Entry th[] = {
#include "i18n/th.inc"
    {nullptr, nullptr}};
#undef T

struct Table { const char *lang; const Entry *entries; };
const Table tables[] = {{"es", es}, {"fr", fr}, {"de", de}, {"it", it}, {"nl", nl}, {"ru", ru}, {"pt", pt},
                        {"ja", ja}, {"ko", ko}, {"zh-Hans", zh_hans}, {"zh-Hant", zh_hant}, {"th", th}};

struct Lang {
    const char *code;  // LanguageCode prefix (language_text_code)
    const char *ui;    // menu language
};
// (most specific first)
const Lang langs[] = {{"es", "es"}, {"fr", "fr"}, {"de", "de"}, {"it", "it"}, {"nl", "nl"}, {"ru", "ru"},
                      {"pt", "pt"}, {"ja", "ja"}, {"ko", "ko"}, {"zh-Hant", "zh-Hant"}, {"zh-Hans", "zh-Hans"},
                      {"zh", "zh-Hans"}, {"th", "th"}};

std::mutex lock;
const char *cur_ui = "en";
int generation;
// the current language's table (keys: views of the table's own literals). By text, not by pointer: menus also pass
// texts they formatted into a buffer of their own.
std::unordered_map<std::string_view, const char *> by_text;

const char *menu_lang_for(const char *code) {
    for (const Lang &l : langs)
        if (!strncmp(code, l.code, strlen(l.code))) return l.ui;
    return "en";
}

void refresh(void) {  // (lock held)
    // SAVAGEWIND_MENU_LANG=<LanguageCode> (testing): the menus in that language, whatever the settings say
    static const char *forced = getenv("SAVAGEWIND_MENU_LANG");
    const char *ui = menu_lang_for(forced && *forced ? forced : language_text_code());
    if (ui == cur_ui) return;
    cur_ui = ui;
    generation++;
    by_text.clear();
    for (const Table &t : tables)
        if (!strcmp(t.lang, ui))
            for (const Entry *e = t.entries; e->en; e++) by_text[e->en] = e->tr;
}
}  // namespace

const char *tr(const char *en) {
    if (!en || !*en) return en;
    std::lock_guard<std::mutex> g(lock);
    refresh();
    if (by_text.empty()) return en;
    auto t = by_text.find(std::string_view(en));
    return t != by_text.end() && t->second && *t->second ? t->second : en;
}

const char *ui_lang(void) {
    std::lock_guard<std::mutex> g(lock);
    refresh();
    return cur_ui;
}

int ui_lang_generation(void) {
    std::lock_guard<std::mutex> g(lock);
    refresh();
    return generation;
}

// ------------------------------------------------------------------ upper case
static unsigned decode(const unsigned char *&p) {
    unsigned c = *p++;
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= 0x3F >> n;
    for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++) c = c << 6 | (*p++ & 0x3F);
    return c;
}

static void encode(std::string &s, unsigned c) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
    else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
    else
        s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3F)), s += (char)(0x80 | (c >> 6 & 0x3F)),
            s += (char)(0x80 | (c & 0x3F));
}

static unsigned upper(unsigned c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 0x20;      // Latin-1 (à-þ)
    if (c >= 0x100 && c <= 0x17F && c != 0x131 && c != 0x138) {      // Latin Extended-A: pairs
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c : c - 1;
        return c & 1 ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;     // Greek
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;                   // Cyrillic а-я
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;                   // ѐ-џ
    return c;
}

std::string tr_upper(const char *s) {
    std::string out;
    for (const unsigned char *p = (const unsigned char *)s; *p;) encode(out, upper(decode(p)));
    return out;
}

// ------------------------------------------------------------------ fonts
// Per script: system fonts that have it (the first one found)
static const char *const cyrillic_fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                             "/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/Helvetica.ttc", nullptr};
static const char *const ja_fonts[] = {"C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc",
                                       "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                       "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                       "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc", nullptr};
static const char *const ko_fonts[] = {"C:/Windows/Fonts/malgun.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                       "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                       "/System/Library/Fonts/AppleSDGothicNeo.ttc", nullptr};
static const char *const zh_hans_fonts[] = {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc",
                                            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/PingFang.ttc", nullptr};
static const char *const zh_hant_fonts[] = {"C:/Windows/Fonts/msjh.ttc", "C:/Windows/Fonts/mingliu.ttc",
                                            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/PingFang.ttc", nullptr};

// Thai: Noto Sans Thai, shipped (third_party/fonts/NotoSansThai, OFL: neither Fira Sans nor the CJK fonts have Thai), then
// the system's
static const char *const th_fonts[] = {"C:/Windows/Fonts/LeelawUI.ttf", "C:/Windows/Fonts/tahoma.ttf",
                                       "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
                                       "/usr/share/fonts/noto/NotoSansThai-Regular.ttf",
                                       "/System/Library/Fonts/Supplemental/Ayuthaya.ttf", nullptr};

// (Noto Sans CJK .ttc: face 0 is Japanese, 1 Korean, 2 Simplified, 3 Traditional Chinese)
static int ttc_face(const char *path, const char *lang) {
    if (!strstr(path, "NotoSansCJK")) return 0;
    return !strcmp(lang, "ko") ? 1 : !strcmp(lang, "zh-Hans") ? 2 : !strcmp(lang, "zh-Hant") ? 3 : 0;
}

bool ui_merge_fallback_font(ImFontAtlas *atlas, float size, const ImFontConfig *base) {
    const char *lang = ui_lang();
    const char *const *fonts = !strcmp(lang, "ru") ? cyrillic_fonts : !strcmp(lang, "ja") ? ja_fonts
                             : !strcmp(lang, "ko") ? ko_fonts : !strcmp(lang, "zh-Hans") ? zh_hans_fonts
                             : !strcmp(lang, "zh-Hant") ? zh_hant_fonts : !strcmp(lang, "th") ? th_fonts : nullptr;
    if (!fonts) return false;
    static char shipped[1100];
    if (fonts == th_fonts && !rt_resource("NotoSansThai-Medium.ttf", "third_party/fonts/NotoSansThai/NotoSansThai-Medium.ttf",
                                          shipped, sizeof shipped))
        shipped[0] = 0;
    // only the glyphs the language's texts use (a CJK font's full range would not fit an atlas texture), in both cases:
    // option labels are drawn upper-cased (tr_upper), with capitals the texts themselves may never use
    static ImVector<ImWchar> ranges;
    static int ranges_gen = -1;
    if (ranges_gen != ui_lang_generation()) {
        ranges_gen = ui_lang_generation();
        ImFontGlyphRangesBuilder b;
        {
            std::lock_guard<std::mutex> g(lock);
            for (auto &kv : by_text) b.AddText(kv.second), b.AddText(tr_upper(kv.second).c_str());
        }
        b.AddText("0123456789%()/:.,-+ ");
        ranges.clear();
        b.BuildRanges(&ranges);
    }
    for (int i = -1; i < 0 || fonts[i]; i++) {
        const char *path = i < 0 ? (fonts == th_fonts ? shipped : "") : fonts[i];  // (-1: a shipped font)
        FILE *f = *path ? fopen(path, "rb") : nullptr;
        if (!f) continue;
        fclose(f);
        ImFontConfig cfg = base ? *base : ImFontConfig();
        cfg.MergeMode = true;
        cfg.FontNo = ttc_face(path, lang);
        cfg.FontDataOwnedByAtlas = true;
        atlas->AddFontFromFileTTF(path, size, &cfg, ranges.Data);
        return true;
    }
    return false;
}
