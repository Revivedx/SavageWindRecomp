// amiibo without figures (runtime/amiibo.c; the reader: runtime/services/nfp.c). The game's NFC reader (nfp:user)
// reads the player's own figure dumps (.bin, NTAG215) from <data>/amiibo (made on first use; its folders are the
// series). The figure chosen in Savage Wind's options (the amiibo tab, settings amiibo_reader) is on the reader when
// the game looks for a figure (the amiibo rune): the game's own scan, animation and drops, no window over the game.
// With the player's amiibo keys there too (key_retail.bin) it also reads what a figure holds: whether it was set up,
// its nickname and its game data (Wolf Link's hearts). Nothing of a figure, nor any key, is shipped.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;    // the dump's file name, without .bin
    const char *series;  // its folder in the amiibo folder ("" for the folder's own files)
    uint8_t id[8];       // amiibo ID: character (2), variant, type, model number (2, big-endian), series, 2
    uint8_t uid[7];      // the tag's serial number
    const char *path;    // the dump
} Amiibo;

// What a figure holds, from its dump with the amiibo keys
enum { AMIIBO_SET_UP = 0x10, AMIIBO_HAS_AREA = 0x20 };
typedef struct {
    uint8_t flags;          // AMIIBO_*
    uint16_t nickname[10];  // UTF-16, as set up
    uint32_t app_id;        // the game its game data is for (Twilight Princess HD: 0x1019c800)
    uint8_t area[0xd8];     // the game data
} AmiiboData;
// false: no keys, or they don't fit the dump (d zeroed)
bool amiibo_read_data(const Amiibo *a, AmiiboData *d);

// The figures, sorted by series and name (read on first use)
int amiibo_count(void);
const Amiibo *amiibo_at(int i);

// The figure the player chose for the reader: its index (amiibo_at), -1 for none or one that's gone
int amiibo_on_reader(void);
// Figure i's key for the settings: "<series>/<name>", or its name alone for a file in the amiibo folder
bool amiibo_key(int i, char *out, size_t n);
// The folder the dumps are read from (<data>/amiibo; made on first use)
const char *amiibo_folder(void);
// Whether the amiibo keys (key_retail.bin in that folder) are there: without them a figure gives no game data
bool amiibo_has_keys(void);

// A figure's game data (its application area: Wolf Link's hearts, for one), a file per figure in <data>/amiibo_data
// once the game has written it, before that the figure's own (amiibo_read_data): its size, -1 if it has none
int amiibo_area_load(const Amiibo *a, uint32_t *access_id, uint8_t *buf, int cap);
bool amiibo_area_save(const Amiibo *a, uint32_t access_id, const uint8_t *buf, int size);

#ifdef __cplusplus
}
#endif
