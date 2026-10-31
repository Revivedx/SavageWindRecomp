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
#include <vector>

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
const Entry he[] = {
#include "i18n/he.inc"
    {nullptr, nullptr}};
#undef T

struct Table { const char *lang; const Entry *entries; };
const Table tables[] = {{"es", es}, {"fr", fr}, {"de", de}, {"it", it}, {"nl", nl}, {"ru", ru}, {"pt", pt},
                        {"ja", ja}, {"ko", ko}, {"zh-Hans", zh_hans}, {"zh-Hant", zh_hant}, {"th", th}, {"he", he}};

struct Lang {
    const char *code;  // LanguageCode prefix (language_text_code)
    const char *ui;    // menu language
};
// (most specific first. "he": the game has no Hebrew, so only SAVAGEWIND_MENU_LANG=he picks it)
const Lang langs[] = {{"es", "es"}, {"fr", "fr"}, {"de", "de"}, {"it", "it"}, {"nl", "nl"}, {"ru", "ru"},
                      {"pt", "pt"}, {"ja", "ja"}, {"ko", "ko"}, {"zh-Hant", "zh-Hant"}, {"zh-Hans", "zh-Hans"},
                      {"zh", "zh-Hans"}, {"th", "th"}, {"he", "he"}};

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

bool rtl_lang(const char *ui) { return !strcmp(ui, "he"); }
void visual_line(const char *b, const char *e, char *out);  // (right to left, below)

void refresh(void) {  // (lock held)
    // SAVAGEWIND_MENU_LANG=<LanguageCode> (testing): the menus in that language, whatever the settings say
    static const char *forced = getenv("SAVAGEWIND_MENU_LANG");
    const char *ui = menu_lang_for(forced && *forced ? forced : language_text_code());
    if (ui == cur_ui) return;
    cur_ui = ui;
    generation++;
    GImTextReorderLine = rtl_lang(ui) ? visual_line : nullptr;  // (ImGui draws right-to-left lines with it)
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

// ------------------------------------------------------------------ right to left
// Hebrew texts are typed in reading order. Drawn, each line is laid out left to right on screen (visual order) by this
// subset of the Unicode Bidirectional Algorithm for a right-to-left paragraph:
// - letters and digits of left-to-right scripts (Latin, numbers) make runs that keep their order: a run goes from one
//   of them to the last one before the next right-to-left letter, with the spaces and punctuation between them
//   ("Savage Wind", "1920 x 1080", "16:9"); a % after a digit, and a + or - before one (not right after a
//   right-to-left letter, as in "ב-60"), belong to the number, and a . before a letter to a file extension (".cube");
// - brackets around left-to-right text only join the run when left-to-right text comes before them too ("FSR (AMD)"),
//   as in the algorithm's rule N0 (which counts digits as right to left there);
// - everything else is reversed, character by character (combining marks stay after their letter), and brackets,
//   « » and ‹ › outside the runs are mirrored, so that "(" still opens.
// Lines without a right-to-left letter are left as they are. The result has the same number of bytes.
namespace {
enum { BIDI_N, BIDI_L, BIDI_R };
struct Cluster {
    int at, len, cls;  // bytes (a character and its combining marks), class
    unsigned c;        // the character
};

unsigned decode_in(const unsigned char *p, const unsigned char *end, int *len) {
    unsigned c = *p;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, i = 1;
    if (n) c &= 0x3F >> n;
    for (; i <= n && p + i < end && (p[i] & 0xC0) == 0x80; i++) c = c << 6 | (p[i] & 0x3F);
    *len = i;
    return c;
}

int bidi_class(unsigned c) {
    if ((c >= 0x590 && c <= 0x8FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return BIDI_R;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return BIDI_L;
    if (c >= 0xC0 && c < 0x2000 && c != 0xD7 && c != 0xF7) return BIDI_L;  // (other letters: Latin, Greek, Cyrillic...)
    if (c >= 0x2E80) return BIDI_L;                                         // (CJK and on)
    return BIDI_N;
}

bool combining(unsigned c) {
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x591 && c <= 0x5BD) || c == 0x5BF || c == 0x5C1 || c == 0x5C2 ||
           c == 0x5C4 || c == 0x5C5 || c == 0x5C7 || (c >= 0x610 && c <= 0x61A) || (c >= 0x64B && c <= 0x65F) ||
           c == 0x670 || (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D;
}

unsigned mirrored(unsigned c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0xAB: return 0xBB;  // « »
    case 0xBB: return 0xAB;
    case 0x2039: return 0x203A;  // ‹ ›
    case 0x203A: return 0x2039;
    }
    return 0;
}

void visual_line(const char *b, const char *e, char *out) {
    const unsigned char *p = (const unsigned char *)b, *end = (const unsigned char *)e;
    static thread_local std::vector<Cluster> k;
    static thread_local std::vector<char> ltr;
    k.clear();
    bool rtl = false;
    for (const unsigned char *q = p; q < end;) {
        int n;
        unsigned c = decode_in(q, end, &n);
        if (combining(c) && !k.empty()) k.back().len += n;
        else {
            k.push_back({(int)(q - p), n, bidi_class(c), c});
            rtl |= k.back().cls == BIDI_R;
        }
        q += n;
    }
    if (!rtl) {
        memcpy(out, b, (size_t)(e - b));
        return;
    }
    const int N = (int)k.size();
    // a number's sign and percent sign, a file extension's dot
    for (int i = 0; i < N; i++) {
        if (k[i].c == '.' && i + 1 < N && k[i + 1].cls == BIDI_L && (k[i + 1].c < '0' || k[i + 1].c > '9') &&
            (i == 0 || k[i - 1].cls != BIDI_L))
            k[i].cls = BIDI_L;
        if (k[i].c < '0' || k[i].c > '9') continue;
        if (i + 1 < N && k[i + 1].c == '%') k[i + 1].cls = BIDI_L;
        if (i > 0 && (k[i - 1].c == '-' || k[i - 1].c == '+') && (i < 2 || k[i - 2].cls != BIDI_R))
            k[i - 1].cls = BIDI_L;
    }
    // bracket pairs (a closing bracket closes the nearest open one of its kind; at most 63 open, as in BD16)
    int open[63], nopen = 0;
    for (int i = 0; i < N; i++) {
        unsigned c = k[i].c, o = c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : 0;
        if (c == '(' || c == '[' || c == '{') {
            if (nopen == 63) break;
            open[nopen++] = i;
            continue;
        }
        if (!o) continue;
        int s = nopen;
        while (s > 0 && k[open[s - 1]].c != o) s--;
        if (!s) continue;
        int a = open[s - 1];
        nopen = s - 1;
        bool in_l = false, in_r = false;
        for (int j = a + 1; j < i; j++) in_l |= k[j].cls == BIDI_L, in_r |= k[j].cls == BIDI_R;
        if (in_r || !in_l) continue;
        int before = BIDI_R;  // (the start of the line: right to left)
        for (int j = a - 1; j >= 0; j--)
            if (k[j].cls != BIDI_N) {
                before = k[j].cls;
                break;
            }
        if (before == BIDI_L) k[a].cls = k[i].cls = BIDI_L;
    }
    // the left-to-right runs
    ltr.assign((size_t)N, 0);
    for (int i = 0; i < N;) {
        if (k[i].cls != BIDI_L) {
            i++;
            continue;
        }
        int last = i, j = i;
        for (; j < N && k[j].cls != BIDI_R; j++)
            if (k[j].cls == BIDI_L) last = j;
        for (int x = i; x <= last; x++) ltr[x] = 1;
        i = j;
    }
    // runs and the other characters, last first
    char *o = out;
    for (int i = N; i > 0;) {
        int j = i - 1;
        if (ltr[j]) {
            int s = j;
            while (s > 0 && ltr[s - 1]) s--;
            size_t from = (size_t)k[s].at, to = (size_t)(k[j].at + k[j].len);
            memcpy(o, b + from, to - from);
            o += to - from;
            i = s;
            continue;
        }
        const Cluster &x = k[j];
        memcpy(o, b + x.at, (size_t)x.len);
        if (unsigned m = mirrored(x.c)) {  // (same length in UTF-8: change the character's last byte)
            int n;
            decode_in(p + x.at, end, &n);
            unsigned last = m < 0x80 ? m : (0x80 | (m & 0x3F));
            if ((n == 1) == (m < 0x80)) o[n - 1] = (char)last;
        }
        o += x.len;
        i = j;
    }
}
}  // namespace

std::string ui_visual_order(const char *s) {
    std::string o = s ? s : "";
    if (o.empty()) return o;
    {
        std::lock_guard<std::mutex> g(lock);
        refresh();
        if (!rtl_lang(cur_ui)) return o;
    }
    for (size_t a = 0; a <= o.size();) {
        size_t z = o.find('\n', a);
        if (z == std::string::npos) z = o.size();
        visual_line(s + a, s + z, &o[a]);
        a = z + 1;
    }
    return o;
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

// Hebrew: Noto Sans Hebrew, shipped (third_party/fonts/NotoSansHebrew, OFL: Fira Sans has no Hebrew), then the system's
static const char *const he_fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                       "/usr/share/fonts/truetype/noto/NotoSansHebrew-Regular.ttf",
                                       "/usr/share/fonts/noto/NotoSansHebrew-Regular.ttf",
                                       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/ArialHB.ttc",
                                       nullptr};

// (Noto Sans CJK .ttc: face 0 is Japanese, 1 Korean, 2 Simplified, 3 Traditional Chinese)
static int ttc_face(const char *path, const char *lang) {
    if (!strstr(path, "NotoSansCJK")) return 0;
    return !strcmp(lang, "ko") ? 1 : !strcmp(lang, "zh-Hans") ? 2 : !strcmp(lang, "zh-Hant") ? 3 : 0;
}

bool ui_merge_fallback_font(ImFontAtlas *atlas, float size, const ImFontConfig *base) {
    const char *lang = ui_lang();
    const char *const *fonts = !strcmp(lang, "ru") ? cyrillic_fonts : !strcmp(lang, "ja") ? ja_fonts
                             : !strcmp(lang, "ko") ? ko_fonts : !strcmp(lang, "zh-Hans") ? zh_hans_fonts
                             : !strcmp(lang, "zh-Hant") ? zh_hant_fonts : !strcmp(lang, "th") ? th_fonts
                             : !strcmp(lang, "he") ? he_fonts : nullptr;
    if (!fonts) return false;
    static char shipped[1100];
    shipped[0] = 0;
    if (fonts == th_fonts && !rt_resource("NotoSansThai-Medium.ttf", "third_party/fonts/NotoSansThai/NotoSansThai-Medium.ttf",
                                          shipped, sizeof shipped))
        shipped[0] = 0;
    if (fonts == he_fonts &&
        !rt_resource("NotoSansHebrew-Medium.ttf", "third_party/fonts/NotoSansHebrew/NotoSansHebrew-Medium.ttf", shipped,
                     sizeof shipped))
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
        const char *path = i < 0 ? shipped : fonts[i];  // (-1: a shipped font)
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
