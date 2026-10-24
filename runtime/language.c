// Game languages: which text language the game is told is the system's (am GetDesiredLanguage, set GetLanguageCode)
// and which voices it plays. BOTW picks its region language (USen, EUes, JPja, ...) from that code and loads
// Pack/Bootup_<it>.pack (the texts) and Voice/<it or a fallback>/ (the voices: JPja for Korean and Chinese, USen for
// British English). On the console the voices always follow; here Voice/ can be redirected to another language's
// folder (runtime/services/fs.c, voice_redirect).
#include "language.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "paths.h"
#include "settings.h"
#ifdef _WIN32
#include <windows.h>
#endif

const TextLanguage g_text_languages[] = {
    {"Auto (system)", NULL, NULL},
    {"English (US)", "en-US", "USen"},
    {"English (UK)", "en-GB", "EUen"},
    {"Spanish (Latin America)", "es-419", "USes"},
    {"Spanish (Spain)", "es", "EUes"},
    {"French (Canada)", "fr-CA", "USfr"},
    {"French (France)", "fr", "EUfr"},
    {"German", "de", "EUde"},
    {"Italian", "it", "EUit"},
    {"Dutch", "nl", "EUnl"},
    {"Russian", "ru", "EUru"},
    {"Portuguese (Brazil)", "pt-BR", "USpt"},
    {"Japanese", "ja", "JPja"},
    {"Korean", "ko", "KRko"},
    // (the switch's own codes, as the game compares them: it has zh-Hans / zh-Hant in its table, not zh-CN / zh-TW)
    {"Chinese (Simplified)", "zh-Hans", "CNzh"},
    {"Chinese (Traditional)", "zh-Hant", "TWzh"},
    {"Thai", "th", "THth"},  // (1.9.0 on: language_text_count; last, so the others keep their place)
};
const int g_ntext_languages = sizeof g_text_languages / sizeof g_text_languages[0];

// Thai came with 1.9.0 (Pack/Bootup_THth.pack and its fonts; the game takes the LanguageCode "th"): offered when the
// game files have it. On 1.8.2 a Thai setting is kept but reads as Auto.
static bool has_thai(void) {
    char p[1200];
    struct stat st;
    snprintf(p, sizeof p, "%s/extracted/romfs/Pack/Bootup_THth.pack", rt_data_root());
    return stat(p, &st) == 0;
}

int language_text_count(void) { return has_thai() ? g_ntext_languages : g_ntext_languages - 1; }

int language_text_index(void) {
    int i = g_settings.text_language;
    return i > 0 && i < language_text_count() ? i : 0;
}

const VoiceLanguage g_voice_languages[] = {
    {"Same as text", NULL},
    {"English", "USen"},
    {"Spanish (Latin America)", "USes"},
    {"Spanish (Spain)", "EUes"},
    {"French", "EUfr"},
    {"German", "EUde"},
    {"Italian", "EUit"},
    {"Russian", "EUru"},
    {"Japanese", "JPja"},
};
const int g_nvoice_languages = sizeof g_voice_languages / sizeof g_voice_languages[0];

// A host locale name ("es-MX", "pt_BR.UTF-8", "zh-Hant-TW") -> one of the game's codes
static const char *code_for_locale(const char *loc) {
    char l[16] = {0};
    for (int i = 0; i < 15 && loc[i] && loc[i] != '.' && loc[i] != '@'; i++) l[i] = loc[i] == '_' ? '-' : loc[i];
    if (!strncmp(l, "en-GB", 5) || !strncmp(l, "en-IE", 5) || !strncmp(l, "en-AU", 5) || !strncmp(l, "en-NZ", 5))
        return "en-GB";
    if (!strncmp(l, "es-ES", 5) || !strcmp(l, "es")) return "es";
    if (!strncmp(l, "es", 2)) return "es-419";  // (es-MX, es-AR, es-419, ...)
    if (!strncmp(l, "fr-CA", 5)) return "fr-CA";
    if (!strncmp(l, "fr", 2)) return "fr";
    if (!strncmp(l, "pt", 2)) return "pt-BR";  // (the only Portuguese the game has)
    if (!strncmp(l, "zh", 2))
        return strstr(l, "Hant") || strstr(l, "TW") || strstr(l, "HK") || strstr(l, "MO") ? "zh-Hant" : "zh-Hans";
    static const char *const two[] = {"de", "it", "nl", "ru", "ja", "ko"};
    for (int i = 0; i < 6; i++) if (!strncmp(l, two[i], 2)) return two[i];
    return "en-US";
}

static const char *host_code(void) {
    static const char *code;
    if (code) return code;
#ifdef _WIN32
    wchar_t w[LOCALE_NAME_MAX_LENGTH];  // (the display language, not the formats' locale)
    char n[LOCALE_NAME_MAX_LENGTH] = {0};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), w, LOCALE_NAME_MAX_LENGTH, 0))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, n, sizeof n, NULL, NULL);
    code = code_for_locale(n);
#else
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
    code = code_for_locale(e && *e && strcmp(e, "C") && strcmp(e, "POSIX") ? e : "en-US");
#endif
    return code;
}

const char *language_text_code(void) {
    int i = language_text_index();
    return i > 0 ? g_text_languages[i].code : host_code();
}

static int booted_text = -1, booted_voice = -1;
void language_booted(void) { booted_text = g_settings.text_language, booted_voice = g_settings.voice_language; }
int language_pending(void) {
    return booted_text >= 0 && (booted_text != g_settings.text_language || booted_voice != g_settings.voice_language);
}

const char *language_voice_folder(void) {
    int i = g_settings.voice_language;
    return i > 0 && i < g_nvoice_languages ? g_voice_languages[i].folder : NULL;
}
