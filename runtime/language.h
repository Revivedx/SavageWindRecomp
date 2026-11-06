// Game languages (runtime/language.c): the text language the game is told is the system's, and the voice language,
// which can differ from it (on the console voices always follow the system language).
#pragma once

typedef struct {
    const char *name;    // shown in the menu
    const char *code;    // nn::settings::LanguageCode the game is given (GetDesiredLanguage)
    const char *folder;  // BOTW's region language (Pack/Bootup_<folder>.pack); NULL: Auto
} TextLanguage;

typedef struct {
    const char *name;
    const char *folder;  // RomFS Voice/<folder>; NULL: the text language's own voices
} VoiceLanguage;

extern const TextLanguage g_text_languages[];
extern const int g_ntext_languages;
extern const VoiceLanguage g_voice_languages[];
extern const int g_nvoice_languages;

// The LanguageCode the game gets: the setting's, or for Auto the host's UI language (en-US if none matches)
const char *language_text_code(void);
// How many of g_text_languages the game files have (Thai, the last, only from 1.9.0 on), and the setting's index
// among those (0, Auto, for one they don't have): what the language menus show.
int language_text_count(void);
int language_text_index(void);
// The text language's folder the game loads (USen, EUde, JPja, ...: Pack/Bootup_<it>.pack, Font/Font_<its first two>.sbfarc)
const char *language_text_folder(void);
// The voice folder chosen in the settings, NULL: the game's own choice
const char *language_voice_folder(void);
// The game reads both when it starts: language_booted() (each boot) notes the settings it starts with,
// language_pending() tells whether they changed since
void language_booted(void);
int language_pending(void);
