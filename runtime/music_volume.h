// Music volume (runtime/music_volume.c): the music_volume setting, applied to the game's music voices only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A RomFS path of a music file: Sound/Resource/Stream/BGM_*.bfstm or Bgm*.bfstm, or Sound/Resource/*Bgm*.bars.
bool music_path(const char *path);
// The bytes of a music file (a whole BARS bank, or at least a BFSTM's header and INFO block): its DSP-ADPCM channels'
// coefficient tables are noted as music. Returns the number of tables found.
int music_note_file(const char *path, const uint8_t *data, size_t size);
// The setting's gain for a volume in percent (100: 1, 0: silent).
float music_gain(int volume_pct);

// nn::audio voices (runtime/services/audio.c). A voice acquired with a music coefficient table is a music voice
// until it is released; set_voice tells which (coefs NULL: not ADPCM). music_voice_volume records the volume the game
// asked a music voice for and returns 1 with the volume to set instead (0 for any other voice).
void music_voice_set(uint64_t voice, const int16_t *coefs);
void music_voice_drop(uint64_t voice);
int music_voice_volume(uint64_t voice, float asked, float *out);
// The music voices and the volumes the game asked for, when the gain changed since the last call (else 0): the
// caller sets each again. At most max.
int music_voice_changed(uint64_t *voice, float *vol, int max);
