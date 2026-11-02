// Virtual amiibo catalog. Preserve enum values and append entries to keep virtual identities stable.
// Metadata: data/amiibo.json; regenerate using tools/update_amiibo_catalog.py.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    AMIIBO_ARCHER, AMIIBO_ZELDA, AMIIBO_GUARDIAN, AMIIBO_BOKOBLIN,
    AMIIBO_RIDER, AMIIBO_DARUK, AMIIBO_MIPHA, AMIIBO_REVALI,
    AMIIBO_URBOSA, AMIIBO_LINK_8BIT, AMIIBO_LINK_OOT, AMIIBO_LINK_MM,
    AMIIBO_LINK_TP, AMIIBO_LINK_SS, AMIIBO_TOON_LINK_WW, AMIIBO_TOON_ZELDA_WW,
    AMIIBO_LINK_SMASH, AMIIBO_ZELDA_SMASH, AMIIBO_TOON_LINK_SMASH, AMIIBO_SHEIK,
    AMIIBO_GANONDORF, AMIIBO_MARIO,
    AMIIBO_LEGACY_COUNT
};
typedef struct {
    const char *key, *name;
    uint8_t character[3], series;
    uint16_t model;
    uint8_t type;
    int group;
    const char *id, *game, *character_name, *kind;
} VirtualAmiibo;

#include "amiibo_catalog.h"

static inline int amiibo_valid_figure(int figure) {
    return figure >= 0 && figure < AMIIBO_COUNT ? figure : AMIIBO_ARCHER;
}

// Called by Gameplay's controls. The service serializes configuration with IPC requests.
#ifdef __cplusplus
extern "C" {
#endif
void nfp_configure(bool enabled, int figure, bool repeat);
bool amiibo_set_game_enabled(bool enabled);  // game's AmiiboItemOnOff, from a guest-thread Options callback
#ifdef __cplusplus
}
#endif
