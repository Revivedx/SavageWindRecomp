#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main unused_fs_main
#include "../runtime/settings.c"
#include "../runtime/hle/bindings.c"
#include "../runtime/nvn/glyphs.c"
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#include "../runtime/language.h"
#include "../runtime/platform.h"
#include "../runtime/perfcount.h"
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/services/fs.c"
#undef main

const TextLanguage g_text_languages[] = {{"Auto", NULL, NULL}};
const VoiceLanguage g_voice_languages[] = {{"Auto", NULL}};
const int g_ntext_languages = 1, g_nvoice_languages = 1;
GameSites g_site;
uint64_t g_program_id;
const char *g_romfs_dir;
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    (void)name, (void)dev_rel, (void)out, (void)n;
    return false;
}
int input_pad_family(void) { return 0; }
int input_last_device(void) { return 0; }
bool pc_cursor_preview_active(void) { return false; }
bool pc_cursor_sensor_active(void) { return false; }
bool fsr3_upscaler_enabled(void) { return false; }
float winsize_refresh_rate(void) { return 0; }
void menu_tex_add(const uint8_t *b, uint64_t n) { (void)b, (void)n; }
int menu_tex_count(void) { return 0; }
void text_sheet_add(const uint8_t *b, uint64_t n) { (void)b, (void)n; }
int text_sheet_count(void) { return 0; }
uint8_t *yaz0_unpack(const uint8_t *z, size_t zn, size_t *out_n) { (void)z, (void)zn, (void)out_n; return NULL; }
int sarc_find(const uint8_t *arc, size_t an, const char *name, uint32_t *doff, uint32_t *s, uint32_t *e) {
    (void)arc, (void)an, (void)name, (void)doff, (void)s, (void)e;
    return -1;
}
ssize_t romfs_image_pread(RomfsImage *img, void *buf, size_t n, uint64_t off) {
    (void)img, (void)buf, (void)n, (void)off;
    return -1;
}
RomfsImage *romfs_image_open(const char *dir, char *why, size_t why_size) {
    (void)dir, (void)why, (void)why_size;
    return NULL;
}
uint64_t plat_now_ns(void) { return 0; }
void kernel_watch_write(const char *what, uint64_t addr, uint64_t size) { (void)what, (void)addr, (void)size; }
const char *rt_version(void) { return "0.0"; }

static size_t append_sec(uint8_t *b, size_t o, const char tag[4], const void *body, uint32_t body_sz) {
    uint32_t sz = 8 + body_sz;
    memcpy(b + o, tag, 4);
    memcpy(b + o + 4, &sz, 4);
    memcpy(b + o + 8, body, body_sz);
    return o + sz;
}

static size_t append_pane(uint8_t *b, size_t o, const char tag[4], const char *name, float x, float y, float rz, float sx) {
    uint8_t body[96] = {0};
    strncpy((char *)body + 4, name, 24);
    float sy = 1.0f, w = 48.0f, h = 48.0f;
    memcpy(body + 36, &x, 4);
    memcpy(body + 40, &y, 4);
    memcpy(body + 56, &rz, 4);
    memcpy(body + 60, &sx, 4);
    memcpy(body + 64, &sy, 4);
    memcpy(body + 68, &w, 4);
    memcpy(body + 72, &h, 4);
    return append_sec(b, o, tag, body, sizeof body);
}

static size_t append_dim_txt(uint8_t *b, size_t o, const char *name, char letter, float x, float y) {
    uint8_t body[0x70] = {0};
    strncpy((char *)body + 4, name, 24);
    memcpy(body + 36, &x, 4);
    memcpy(body + 40, &y, 4);
    float one = 1.0f, fw = 20.4f, fh = 25.5f;
    memcpy(body + 60, &one, 4);
    memcpy(body + 64, &one, 4);
    uint32_t toff = 0x6C;
    memcpy(body + 0x5C, &toff, 4);
    memcpy(body + 0x68, &fw, 4);
    memcpy(body + 0x6C, &fh, 4);
    uint8_t full[0x78] = {0};
    memcpy(full, body, sizeof body);
    full[0x64] = (uint8_t)letter;
    return append_sec(b, o, "txt1", full, sizeof full);
}

static size_t build_synthetic_padocommand(uint8_t *b, int which) {
    memset(b, 0, 2048);
    memcpy(b, "FLYT\xFF\xFE\x14\x00\x00\x02\x08\x05", 12);
    size_t o = 0x14;
    uint8_t txl[40] = {0};
    txl[0] = 1;
    uint32_t toff = 8;
    memcpy(txl + 4, &toff, 4);
    strcpy((char *)txl + 8, (which == 0 || which == 3) ? "Nt_KeyBtnSubBaseAL_00^s" : "Nt_KeyBtnSubBaseBL_00^s");
    o = append_sec(b, o, "txl1", txl, sizeof txl);
    uint8_t fnl[28] = {0};
    fnl[0] = 1;
    uint32_t foff = 8;
    memcpy(fnl + 4, &foff, 4);
    strcpy((char *)fnl + 8, "Normal_00.bffnt");
    o = append_sec(b, o, "fnl1", fnl, sizeof fnl);
    o = append_pane(b, o, "pic1", "P_Base_00", 0.0f, 0.0f, (which == 2 || which == 3) ? 180.0f : 0.0f,
                    which == 2 ? -1.0f : 1.0f);
    static const char letters[4] = {'A', 'B', 'X', 'Y'};
    static const char *const tnames[4] = {"T_A_00", "T_B_00", "T_X_00", "T_Y_00"};
    static const float dim_xy[4][2] = {{26.2f, 0.8f}, {1.0f, -26.0f}, {0.8f, 26.0f}, {-25.5f, -0.2f}};
    for (int i = 0; i < 4; i++)
        if (i != which) o = append_dim_txt(b, o, tnames[i], letters[i], dim_xy[i][0], dim_xy[i][1]);
    static const float act_xy[4][2] = {{26.0f, 0.0f}, {0.0f, -26.0f}, {0.0f, 26.0f}, {-26.0f, 0.0f}};
    o = append_pane(b, o, "pan1", "N_BtnA_00", act_xy[which][0], act_xy[which][1], 0.0f, 1.0f);
    o = append_pane(b, o, "wnd1", "W_Base_00", which == 3 ? 33.0f : -33.0f, 0.0f, 0.0f, which == 3 ? -1.0f : 1.0f);
    o = append_pane(b, o, "txt1", "T_Guide_01", which == 3 ? -23.0f : 21.0f, 0.0f, 0.0f, 1.0f);
    o = append_pane(b, o, "pic1", "P_CaptureUse", which == 3 ? -60.0f : 60.0f, 0.0f, 0.0f, 1.0f);
    o = append_pane(b, o, "pan1", "N_Capture_00", act_xy[which][0] + (which == 3 ? -60.0f : 60.0f), act_xy[which][1], 0.0f, 1.0f);
    o = append_pane(b, o, "txt1", "T_Guide_00", which == 3 ? 37.0f : -39.0f, 0.0f, 0.0f, 1.0f);
    o = append_pane(b, o, "pic1", "Nt_KeyTexA_01", which == 3 ? 60.0f : -60.0f, 0.0f, 0.0f, 1.0f);
    o = append_pane(b, o, "pic1", "P_CaptureUse_01", act_xy[which][0] + (which == 3 ? -60.0f : 60.0f), act_xy[which][1], 0.0f, 1.0f);
    uint32_t total = (uint32_t)o;
    memcpy(b + 0xC, &total, 4);
    return o;
}

static const uint8_t *find_pane(const uint8_t *b, size_t n, const char *name) {
    size_t o = b[6] | b[7] << 8;
    while (o + 8 <= n) {
        uint32_t sz;
        memcpy(&sz, b + o + 4, 4);
        if (sz < 8 || o + sz > n) break;
        if (sz >= 84 && (!memcmp(b + o, "pan1", 4) || !memcmp(b + o, "pic1", 4) ||
                         !memcmp(b + o, "wnd1", 4) || !memcmp(b + o, "txt1", 4))) {
            if (!strncmp((const char *)b + o + 12, name, 24)) return b + o;
        }
        o += sz;
    }
    return NULL;
}

static bool nearf(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main(void) {
    bind_reset(BIND_DEV_PAD);
    bind_reset(BIND_DEV_KEY);

    int pos[4];
    g_settings.button_layout = BUTTON_LAYOUT_AUTO;
    glyphs_hud_positions(pos);
    assert(pos[0] == 0 && pos[1] == 1 && pos[2] == 2 && pos[3] == 3);

    // Automatic follows g_bind_pad swaps (e.g. PS5 Cross / Circle swapped)
    g_bind_pad[BIND_A] = SDL_GAMEPAD_BUTTON_SOUTH;
    g_bind_pad[BIND_B] = SDL_GAMEPAD_BUTTON_EAST;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 2 && pos[3] == 3);

    // Automatic also follows per-action swaps when g_bind_pad is at default
    bind_reset(BIND_DEV_PAD);
    g_act_pad[0] = SDL_GAMEPAD_BUTTON_SOUTH;   // Interact
    g_act_pad[14] = SDL_GAMEPAD_BUTTON_EAST;   // Cancel
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 2 && pos[3] == 3);
    bind_reset(BIND_DEV_PAD);

    // Explicit presets
    g_settings.button_layout = BUTTON_LAYOUT_SWAP_AB;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 2 && pos[3] == 3);

    g_settings.button_layout = BUTTON_LAYOUT_XBOX;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 0 && pos[2] == 3 && pos[3] == 2);

    g_settings.button_layout = BUTTON_LAYOUT_SWAP_XY;
    glyphs_hud_positions(pos);
    assert(pos[0] == 0 && pos[1] == 1 && pos[2] == 3 && pos[3] == 2);

    g_settings.button_layout = BUTTON_LAYOUT_GAMECUBE;
    glyphs_hud_positions(pos);
    assert(pos[0] == 1 && pos[1] == 3 && pos[2] == 0 && pos[3] == 2);

    // Patch synthetic PaDoCommandA/B/X/Y_00 under GameCube layout (A=South, B=West, X=East, Y=North)
    int panes = 0;
    for (int w = 0; w < 4; w++) {
        uint8_t src[2048], dst[2112];
        size_t sn = build_synthetic_padocommand(src, w);
        size_t dn = hud_letters_flyt(src, sn, dst, w, pos, &panes);
        assert(dn > sn);
        const uint8_t *btn = find_pane(dst, dn, "N_BtnA_00");
        const uint8_t *base = find_pane(dst, dn, "P_Base_00");
        const uint8_t *wbase = find_pane(dst, dn, "W_Base_00");
        assert(btn && base && wbase);
        float bx, by, rz, sx, wx, wsx;
        memcpy(&bx, btn + 44, 4);
        memcpy(&by, btn + 48, 4);
        memcpy(&rz, base + 64, 4);
        memcpy(&sx, base + 68, 4);
        memcpy(&wx, wbase + 44, 4);
        memcpy(&wsx, wbase + 68, 4);
        int act = pos[w];
        static const float exp_act[4][2] = {{26.0f, 0.0f}, {0.0f, -26.0f}, {0.0f, 26.0f}, {-26.0f, 0.0f}};
        assert(nearf(bx, exp_act[act][0]) && nearf(by, exp_act[act][1]));
        assert(nearf(rz, (act == 2 || act == 3) ? 180.0f : 0.0f));
        assert(nearf(sx, act == 2 ? -1.0f : 1.0f));
        assert(nearf(wx, act == 3 ? 33.0f : -33.0f));
        assert(nearf(wsx, act == 3 ? -1.0f : 1.0f));
    }
    assert(panes == 12);

    // Settings load/save round-trip for button_layout
    const char *tmp = "build/test_hud_button_layout.ini";
    setenv("SAVAGEWIND_SETTINGS", tmp, 1);
    g_settings.button_layout = BUTTON_LAYOUT_GAMECUBE;
    settings_save();
    FILE *fp = fopen(tmp, "r");
    assert(fp != NULL);
    char buf[4096] = {0};
    fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    unlink(tmp);
    assert(strstr(buf, "button_layout = gamecube") != NULL);

    puts("PASS: HUD button layout positions and FLYT patching");
    return 0;
}
