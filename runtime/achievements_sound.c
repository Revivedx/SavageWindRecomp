// The sound played when an achievement is unlocked (the overlay calls ach_sound_play when it shows the notice).
//
// By default a short original "ya-ha-ha!" (achievements_synth.c). To use another sound, such as a Korok's own "Yahaha!"
// from a recording you have the right to use, put a WAV file named achievement.wav (or sounds/achievement.wav) in the
// data root, next to savagewind.ini: it is looked for the first time a sound is needed. The volume follows the game's master
// volume. SAVAGEWIND_ACH_SOUND=0 turns the sound off, and so does SAVAGEWIND_NO_AUDIO.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "achievements.h"
#include "settings.h"

static SDL_AudioStream *stream;
static Uint8 *pcm;
static Uint32 pcm_len;
static bool tried;

static bool off(const char *var) { return getenv(var) && !strcmp(getenv(var), "0"); }

static void open_once(void) {
    tried = true;
    if (off("SAVAGEWIND_ACH_SOUND")) return;
    if (getenv("SAVAGEWIND_NO_AUDIO") && strcmp(getenv("SAVAGEWIND_NO_AUDIO"), "0")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, 48000};
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
