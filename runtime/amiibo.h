// Virtual figure catalog. Append entries to preserve existing per-figure serial numbers. IDs: https://github.com/N3evin/AmiiboAPI/blob/master/database/amiibo.json
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
    AMIIBO_COUNT
};
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
    {"rider", "Link (Rider)", {0x01, 0x00, 0x00}, 0, 0x0354, 9},
    {"daruk", "Daruk", {0x01, 0x05, 0x00}, 0, 0x0358, 9},
    {"mipha", "Mipha", {0x01, 0x07, 0x00}, 0, 0x035a, 9},
    {"revali", "Revali", {0x01, 0x08, 0x00}, 0, 0x035b, 9},
    {"urbosa", "Urbosa", {0x01, 0x06, 0x00}, 0, 0x0359, 9},
    {"link_8bit", "Link (8-Bit)", {0x01, 0x00, 0x00}, 0, 0x034f, 9},
    {"link_oot", "Link (Ocarina of Time)", {0x01, 0x00, 0x00}, 0, 0x034b, 9},
    {"link_mm", "Link (Majora's Mask)", {0x01, 0x00, 0x00}, 0, 0x034c, 9},
    {"link_tp", "Link (Twilight Princess)", {0x01, 0x00, 0x00}, 0, 0x034d, 9},
    {"link_ss", "Link (Skyward Sword)", {0x01, 0x00, 0x00}, 0, 0x034e, 9},
    {"toon_link_ww", "Toon Link (The Wind Waker)", {0x01, 0x00, 0x01}, 0, 0x0350, 9},
    {"toon_zelda_ww", "Toon Zelda (The Wind Waker)", {0x01, 0x01, 0x00}, 0, 0x0352, 9},
    {"link_smash", "Link (Super Smash Bros.)", {0x01, 0x00, 0x00}, 0, 0x0004, 0},
    {"zelda_smash", "Zelda (Super Smash Bros.)", {0x01, 0x01, 0x00}, 0, 0x000e, 0},
    {"toon_link_smash", "Toon Link (Super Smash Bros.)", {0x01, 0x00, 0x01}, 0, 0x0016, 0},
    {"sheik", "Sheik", {0x01, 0x01, 0x01}, 0, 0x0017, 0},
    {"ganondorf", "Ganondorf", {0x01, 0x02, 0x01}, 0, 0x001b, 0},
    {"mario", "Mario (Super Smash Bros.)", {0x00, 0x00, 0x00}, 0, 0x0000, 0},
};

static inline int amiibo_valid_figure(int figure) {
    return figure >= 0 && figure < AMIIBO_COUNT ? figure : AMIIBO_ARCHER;
}

// Called by Gameplay's controls. The service serializes configuration with IPC requests.
void nfp_configure(bool enabled, int figure, bool repeat);
