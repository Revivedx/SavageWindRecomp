// The sound played when an achievement is unlocked (the overlay calls ach_sound_play when it shows the notice).
//
// The game's own: a hidden Korok's "Yahaha!", read from the player's RomFS (extracted/romfs, a voice bank common to every
// language) and decoded the first time a sound is needed (achievements_bars.c); nothing of the game ships with Savage Wind.
// Without it (game files not set up, another game version), a WAV file named achievement.wav (or sounds/achievement.wav)
// in the data root, next to savagewind.ini; failing that, a short original "ya-ha-ha!" (achievements_synth.c). The last
// achievement (ACH_META: all the others) has its own: the jingle of a quest completed, from the same RomFS (the usual
// sound without it). The volume follows the game's master volume. SAVAGEWIND_ACH_SOUND=0 turns the sound off, and so
// does SAVAGEWIND_NO_AUDIO.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "achievements.h"
#include "settings.h"

#define GAME_BANK "extracted/romfs/Voice/Common/NV_Korogu_Man_Young_Normal00.bars"
#define GAME_SOUND "NV_Korogu_Man_Young_Normal00_HiddenKorok_Appear00"
#define FINAL_BANK "extracted/romfs/Sound/Resource/M_UI.bars"
#define FINAL_SOUND "Sys_Challenge_Complete"

typedef struct {
    SDL_AudioStream *stream;
    Uint8 *pcm;
    Uint32 len;
} Sound;

static Sound usual, final;
static bool tried;

static bool off(const char *var) { return getenv(var) && !strcmp(getenv(var), "0"); }

// One sound out of a game sound bank (the data root is the working directory); false when it isn't there.
static bool game_sound(const char *bank_path, const char *name, SDL_AudioSpec *spec, Sound *out) {
    FILE *f = fopen(bank_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bank = sz > 0 && sz < (64l << 20) ? malloc((size_t)sz) : NULL;
    bool ok = bank && fread(bank, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    int rate = 0;
    size_t n = 0;
    int16_t *samples = ok ? ach_bars_sound(bank, (size_t)sz, name, &rate, &n) : NULL;
    free(bank);
    if (!samples) return false;
    spec->format = SDL_AUDIO_S16, spec->channels = 1, spec->freq = rate;
    out->pcm = (Uint8 *)samples, out->len = (Uint32)(n * sizeof *samples);
    return true;
}

static void open_stream(Sound *s, const SDL_AudioSpec *spec) {
    s->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, spec, NULL, NULL);
    if (s->stream) SDL_ResumeAudioStreamDevice(s->stream);
}

static void open_once(void) {
    tried = true;
    if (off("SAVAGEWIND_ACH_SOUND")) return;
    if (getenv("SAVAGEWIND_NO_AUDIO") && strcmp(getenv("SAVAGEWIND_NO_AUDIO"), "0")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, 48000};
    game_sound(GAME_BANK, GAME_SOUND, &spec, &usual);
    static const char *const files[] = {"achievement.wav", "sounds/achievement.wav"};
    for (size_t i = 0; i < sizeof files / sizeof files[0] && !usual.pcm; i++) {
        SDL_AudioSpec w;
        Uint8 *buf;
        Uint32 len;
        if (SDL_LoadWAV(files[i], &w, &buf, &len)) spec = w, usual.pcm = buf, usual.len = len;
    }
    if (!usual.pcm) {  // (SDL_free on a malloc'd block is fine: the buffer is never freed, it lives as long as the process)
        int16_t *samples = malloc(48000 * sizeof *samples);
        if (!samples) return;
        size_t n = ach_synth_jingle(samples, 48000, 48000);
        usual.pcm = (Uint8 *)samples, usual.len = (Uint32)(n * sizeof *samples);
    }
    open_stream(&usual, &spec);
    SDL_AudioSpec fspec;
    if (game_sound(FINAL_BANK, FINAL_SOUND, &fspec, &final)) open_stream(&final, &fspec);
}

static void play(Sound *s) {
    if (!s->stream || !s->pcm || !s->len) return;
    SDL_ClearAudioStream(s->stream);  // (two unlocks in a row restart it rather than queue it twice)
    SDL_SetAudioStreamGain(s->stream, g_settings.volume);
    SDL_PutAudioStreamData(s->stream, s->pcm, (int)s->len);
    SDL_FlushAudioStream(s->stream);
}

void ach_sound_play(void) {
    if (!tried) open_once();
    play(&usual);
}

void ach_sound_play_final(void) {
    if (!tried) open_once();
    play(final.stream ? &final : &usual);
}
