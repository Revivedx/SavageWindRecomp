// The sound played when an achievement is unlocked (the overlay calls ach_sound_play when it shows the notice).
//
// The game's own: a hidden Korok's "Yahaha!", read from the player's RomFS (extracted/romfs, a voice bank common to every
// language) and decoded the first time a sound is needed (achievements_bars.c); nothing of the game ships with Savage Wind.
// Without it (game files not set up, another game version), a WAV file named achievement.wav (or sounds/achievement.wav)
// in the data root, next to savagewind.ini; failing that, a short original "ya-ha-ha!" (achievements_synth.c). The volume
// follows the game's master volume. SAVAGEWIND_ACH_SOUND=0 turns the sound off, and so does SAVAGEWIND_NO_AUDIO.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "achievements.h"
#include "settings.h"

#define GAME_BANK "extracted/romfs/Voice/Common/NV_Korogu_Man_Young_Normal00.bars"
#define GAME_SOUND "NV_Korogu_Man_Young_Normal00_HiddenKorok_Appear00"

static SDL_AudioStream *stream;
static Uint8 *pcm;
static Uint32 pcm_len;
static bool tried;

static bool off(const char *var) { return getenv(var) && !strcmp(getenv(var), "0"); }

// The "Yahaha!" out of the game's voice bank (the data root is the working directory); false when it isn't there.
static bool game_sound(SDL_AudioSpec *spec) {
    FILE *f = fopen(GAME_BANK, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bank = sz > 0 && sz < (64l << 20) ? malloc((size_t)sz) : NULL;
    bool ok = bank && fread(bank, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    int rate = 0;
    size_t n = 0;
    int16_t *samples = ok ? ach_bars_sound(bank, (size_t)sz, GAME_SOUND, &rate, &n) : NULL;
    free(bank);
    if (!samples) return false;
    spec->format = SDL_AUDIO_S16, spec->channels = 1, spec->freq = rate;
    pcm = (Uint8 *)samples, pcm_len = (Uint32)(n * sizeof *samples);
    return true;
}

static void open_once(void) {
    tried = true;
    if (off("SAVAGEWIND_ACH_SOUND")) return;
    if (getenv("SAVAGEWIND_NO_AUDIO") && strcmp(getenv("SAVAGEWIND_NO_AUDIO"), "0")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, 48000};
    game_sound(&spec);
    static const char *const files[] = {"achievement.wav", "sounds/achievement.wav"};
    for (size_t i = 0; i < sizeof files / sizeof files[0] && !pcm; i++) {
        SDL_AudioSpec w;
        Uint8 *buf;
        Uint32 len;
        if (SDL_LoadWAV(files[i], &w, &buf, &len)) spec = w, pcm = buf, pcm_len = len;
    }
    if (!pcm) {  // (SDL_free on a malloc'd block is fine: the buffer is never freed, it lives as long as the process)
        int16_t *samples = malloc(48000 * sizeof *samples);
        if (!samples) return;
        size_t n = ach_synth_jingle(samples, 48000, 48000);
        pcm = (Uint8 *)samples, pcm_len = (Uint32)(n * sizeof *samples);
    }
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (stream) SDL_ResumeAudioStreamDevice(stream);
}

void ach_sound_play(void) {
    if (!tried) open_once();
    if (!stream || !pcm || !pcm_len) return;
    SDL_ClearAudioStream(stream);  // (two unlocks in a row restart it rather than queue it twice)
    SDL_SetAudioStreamGain(stream, g_settings.volume);
    SDL_PutAudioStreamData(stream, pcm, (int)pcm_len);
    SDL_FlushAudioStream(stream);
}
