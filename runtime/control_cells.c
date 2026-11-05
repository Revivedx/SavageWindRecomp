// Named game state cells (control_cells.h) and the `cell` command (developer only).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "control_cells.h"
#include "control_mem.h"
#include "game_sites.h"

static uint64_t root_object(const ControlCell *c) {
    uint64_t site = *(const uint64_t *)((const char *)&g_site + c->root);
    uint64_t obj = 0;
    return site && control_mem_peek(site, &obj, sizeof obj) ? obj : 0;
}
static size_t elem_size(CellType t) { (void)t; return 4; }  // (all three types are 32 bits)

uint64_t control_cell_addr(const ControlCell *c) {
    uint64_t root = root_object(c);
    if (!root) return 0;
    size_t sz = elem_size(c->type) * (size_t)c->count;
    for (int k = 0; k < c->nchains; k++) {
        const CellChain *ch = &c->chains[k];
        uint64_t p = root;
        int i = 0;
        for (; i + 1 < ch->len; i++)
            if (!control_mem_peek(p + ch->off[i], &p, sizeof p) || p < 0x10000) break;
        if (i + 1 < ch->len) continue;
        p += ch->off[ch->len - 1];
        uint8_t v[16];
        if (!control_mem_peek(p, v, sz)) continue;
        if (!c->check || c->check(v)) return p;
    }
    return 0;
}

bool control_cell_get(const ControlCell *c, double *out) {
    uint64_t a = control_cell_addr(c);
    uint8_t v[16];
    if (!a || !control_mem_peek(a, v, elem_size(c->type) * (size_t)c->count)) return false;
    for (int i = 0; i < c->count; i++) {
        switch (c->type) {
        case CELL_F32: { float f; memcpy(&f, v + 4 * i, 4); out[i] = f; break; }
        case CELL_U32: { uint32_t u; memcpy(&u, v + 4 * i, 4); out[i] = u; break; }
        case CELL_S32: { int32_t s; memcpy(&s, v + 4 * i, 4); out[i] = s; break; }
        }
    }
    return true;
}

bool control_cell_camera_angles(double e[3], double t[3], double *yaw, double *pitch) {
    double p[1];
    if (!control_cell_get(&control_cell_camera_eye, e) || !control_cell_get(&control_cell_camera_target, t) ||
        !control_cell_get(&control_cell_camera_pitch, p)) return false;
    double dx = t[0] - e[0], dy = t[1] - e[1], dz = t[2] - e[2];
    if (dx * dx + dy * dy + dz * dz < 1e-12) return false;
    *yaw = atan2(dx, dz) * 57.29577951308232;
    *pitch = -p[0];
    return true;
}

bool control_cell_set(const ControlCell *c, const double *in) {
    uint64_t a = control_cell_addr(c);
    uint8_t v[16];
    if (!a) return false;
    for (int i = 0; i < c->count; i++) {
        switch (c->type) {
        case CELL_F32: { float f = (float)in[i]; memcpy(v + 4 * i, &f, 4); break; }
        case CELL_U32: { uint32_t u = (uint32_t)in[i]; memcpy(v + 4 * i, &u, 4); break; }
        case CELL_S32: { int32_t s = (int32_t)in[i]; memcpy(v + 4 * i, &s, 4); break; }
        }
    }
    return control_mem_poke(a, v, elem_size(c->type) * (size_t)c->count);
}

// ---- the cells

// Link's position: the copy at PlayerInfo +0x88 is what the game rewrites each frame, so a chain is accepted only if it
// ends on a vec3 within a metre (x and z) of that copy. Primary chain first; the other two reach the same object through
// an array whose slot moved by 8 between boots (found with mem path, 1.9.0).
static bool pos_check(const void *value) {
    uint64_t pinfo = g_site.player_info ? *(const uint64_t *)(uintptr_t)g_site.player_info : 0;
    if (!pinfo) return false;
    const float *pp = (const float *)(uintptr_t)(pinfo + 0x88), *v = value;
    return fabsf(v[0] - pp[0]) <= 1.0f && fabsf(v[2] - pp[2]) <= 1.0f;
}
static const CellChain pos_chains[] = {
    {5, {0x60, 0x578, 0x80, 0x410, 0x1c0}},
    {5, {0x10, 0xe8, 0x750, 0x78, 0x1c0}},
    {5, {0x10, 0xe8, 0x758, 0x78, 0x1c0}},
};
const ControlCell control_cell_player_pos = {
    "player.pos", "Link's real position x y z (what `teleport` writes)", offsetof(GameSites, player_info), CELL_F32, 3,
    pos_chains, (int)(sizeof pos_chains / sizeof *pos_chains), pos_check,
};

// Fall damage: the game keeps the highest point Link reached since he left the ground and, on landing, takes the drop from
// there. It lives in two copies on the player object (found by flying up, then down, and reading which slots follow the
// peak and not the current height, 1.9.0). A reading is accepted when it is not below Link's own height (the copy at
// PlayerInfo +0x8c) and not absurdly above it.
static bool fall_check(const void *value) {
    uint64_t pinfo = g_site.player_info ? *(const uint64_t *)(uintptr_t)g_site.player_info : 0;
    if (!pinfo) return false;
    float py = *(const float *)(uintptr_t)(pinfo + 0x8c), v = *(const float *)value;
    return isfinite(v) && v >= py - 2.0f && v <= py + 20000.0f;
}
static const CellChain fall_top_chains[] = {{2, {0x60, 0x860}}};
static const CellChain fall_top2_chains[] = {{2, {0x60, 0x20d0}}};
const ControlCell control_cell_player_fall_top = {
    "player.fall_top", "highest y since Link left the ground (fall damage is the drop from it)", offsetof(GameSites, player_info), CELL_F32, 1,
    fall_top_chains, 1, fall_check,
};
const ControlCell control_cell_player_fall_top2 = {
    "player.fall_top2", "second copy of player.fall_top (the game keeps both)", offsetof(GameSites, player_info), CELL_F32, 1,
    fall_top2_chains, 1, fall_check,
};

// The orbit camera behind Link (found by rotating candidate eye positions around him and watching the camera, 1.9.0).
// One object holds eye (x y z), target (x y z, a little above Link), up, fov, near and far; the game keeps the eye as
// state, not as a copy: moving it around the target turns the camera, and it stays there. The pitch is a separate float
// (minus the pitch in degrees, positive = looking down) a few objects further on. Yaw has no cell of its own: it is the
// direction of eye -> target. Both chains start at the same two objects (PlayerInfo +0x60 -> +0x2b8, or the twin through
// +0x328) and were found with mem path. A reading is accepted when it is near Link's own position copy.
static bool near_link(const float *v, float within) {
    uint64_t pinfo = g_site.player_info ? *(const uint64_t *)(uintptr_t)g_site.player_info : 0;
    if (!pinfo) return false;
    const float *pp = (const float *)(uintptr_t)(pinfo + 0x88);
    for (int i = 0; i < 3; i++) if (!isfinite(v[i])) return false;
    float dx = v[0] - pp[0], dy = v[1] - pp[1], dz = v[2] - pp[2];
    return dx * dx + dy * dy + dz * dz <= within * within;
}
static bool eye_check(const void *value) { return near_link(value, 300.0f); }
static bool target_check(const void *value) { return near_link(value, 20.0f); }
static bool pitch_check(const void *value) { float v = *(const float *)value; return isfinite(v) && fabsf(v) <= 90.01f; }
static const CellChain eye_chains[] = {{3, {0x60, 0x2b8, 0x5b0}}, {3, {0x60, 0x328, 0x540}}};
static const CellChain target_chains[] = {{3, {0x60, 0x2b8, 0x5bc}}, {3, {0x60, 0x328, 0x54c}}};
static const CellChain pitch_chains[] = {{4, {0x60, 0x2b8, 0x2a8, 0xd14}}, {4, {0x60, 0x328, 0x238, 0xd14}}};
const ControlCell control_cell_camera_eye = {
    "camera.eye", "the camera's position x y z (move it around camera.target to turn the camera)", offsetof(GameSites, player_info), CELL_F32, 3,
    eye_chains, 2, eye_check,
};
const ControlCell control_cell_camera_target = {
    "camera.target", "the point the camera looks at x y z (a little above Link)", offsetof(GameSites, player_info), CELL_F32, 3,
    target_chains, 2, target_check,
};
const ControlCell control_cell_camera_pitch = {
    "camera.pitch", "minus the camera pitch in degrees (positive looks down; the game clamps it to +-89)", offsetof(GameSites, player_info), CELL_F32, 1,
    pitch_chains, 2, pitch_check,
};

static const ControlCell *const all_cells[] = {&control_cell_player_pos, &control_cell_player_fall_top, &control_cell_player_fall_top2,
                                               &control_cell_camera_eye, &control_cell_camera_target, &control_cell_camera_pitch};

const ControlCell *control_cell_find(const char *name) {
    for (size_t i = 0; i < sizeof all_cells / sizeof *all_cells; i++)
        if (!strcmp(all_cells[i]->name, name)) return all_cells[i];
    return NULL;
}

// cell | cell <name> | cell <name> <v> [<v>...]
static void cmd_cell(int argc, char **argv, ControlReply *r) {
    if (argc == 1) {
        for (size_t i = 0; i < sizeof all_cells / sizeof *all_cells; i++) {
            const ControlCell *c = all_cells[i];
            control_field_str(r, c->name, c->what);
        }
        return;
    }
    const ControlCell *c = control_cell_find(argv[1]);
    if (!c) { control_fail(r, "Unknown cell: %s", argv[1]); return; }
    if (argc > 2) {
        if (argc - 2 != c->count) { control_fail(r, "%s needs %d value(s)", c->name, c->count); return; }
        double v[4];
        for (int i = 0; i < c->count; i++) {
            char *end;
            v[i] = strtod(argv[2 + i], &end);
            if (end == argv[2 + i] || *end || !isfinite(v[i])) { control_fail(r, "Not a number: \"%s\"", argv[2 + i]); return; }
        }
        if (!control_cell_set(c, v)) { control_fail(r, "Cannot write %s", c->name); return; }
    }
    double v[4];
    if (!control_cell_get(c, v)) { control_fail(r, "%s not found", c->name); return; }
    char a[32];
    snprintf(a, sizeof a, "0x%llx", (unsigned long long)control_cell_addr(c));
    control_field_str(r, "address", a);
    for (int i = 0; i < c->count; i++) {
        char k[16];
        snprintf(k, sizeof k, "v%d", i);
        control_field_num(r, k, v[i]);
    }
}

// sites: the game's known root addresses (game_sites.h) and the pointer stored at each, the starting points for mem path
static void cmd_sites(int argc, char **argv, ControlReply *r) {
    (void)argv;
    if (argc != 1) { control_fail(r, "Usage: sites"); return; }
#define SITE(n)                                                                                                      \
    do {                                                                                                             \
        char a[32], k[48];                                                                                           \
        uint64_t v = 0;                                                                                              \
        snprintf(k, sizeof k, "%s", #n);                                                                             \
        snprintf(a, sizeof a, "0x%llx", (unsigned long long)g_site.n);                                               \
        control_field_str(r, k, a);                                                                                  \
        if (g_site.n && control_mem_peek(g_site.n, &v, sizeof v)) {                                                  \
            snprintf(k, sizeof k, "%s.value", #n);                                                                   \
            snprintf(a, sizeof a, "0x%llx", (unsigned long long)v);                                                  \
            control_field_str(r, k, a);                                                                              \
        }                                                                                                            \
    } while (0);
    GAME_SITES(SITE)
#undef SITE
}

void control_cells_init(void) {
    static const ControlCmd cmds[] = {
        {"cell", "[<name> [<value>...]]", "List, read or write a named game state cell", cmd_cell},
        {"sites", "", "Known root addresses and their values", cmd_sites},
    };
    control_register(cmds, (int)(sizeof cmds / sizeof *cmds));
}
