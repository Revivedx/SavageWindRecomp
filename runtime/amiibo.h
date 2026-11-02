// Small virtual figure catalog. IDs: https://github.com/N3evin/AmiiboAPI/blob/master/database/amiibo.json
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { AMIIBO_ARCHER, AMIIBO_ZELDA, AMIIBO_GUARDIAN, AMIIBO_BOKOBLIN, AMIIBO_COUNT };
typedef struct {
    const char *key, *name;
    uint8_t character[3], series;
    uint16_t model;
    uint8_t type;
} VirtualAmiibo;

static const VirtualAmiibo virtual_amiibos[AMIIBO_COUNT] = {
    {"archer", "Link (Archer)", {0x01, 0x00, 0x00}, 0, 0x0353, 9},
    {"zelda", "Zelda (Breath of the Wild)", {0x01, 0x01, 0x00}, 0, 0x0356, 9},
    {"guardian", "Guardian", {0x01, 0x40, 0x00}, 0, 0x0355, 9},
    {"bokoblin", "Bokoblin", {0x01, 0x41, 0x00}, 0, 0x035c, 9},
};

static inline int amiibo_valid_figure(int figure) {
    return figure >= 0 && figure < AMIIBO_COUNT ? figure : AMIIBO_ARCHER;
}

// Called by Gameplay's controls. The service serializes configuration with IPC requests.
void nfp_configure(bool enabled, int figure, bool repeat);
