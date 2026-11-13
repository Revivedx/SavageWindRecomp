// Exercise the actual Options hooks with the real capture engine and synthetic guest rows.
#include <cassert>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../runtime/ui/game_options.mm"

Settings g_settings = {};
GameSites g_site = {};
const char *g_game_version = "1.9.0";
static Cpu test_cpu;
static unsigned focused_kind, focus_count, help_count;
static uint64_t focused_row;
const char *tr(const char *s) { return s; }
std::string ui_visual_order(const char *s) { return s; }  // (i18n.mm: right-to-left reordering, not under test)
static int test_preset_applied = -1;
static int test_aa_row_applied = -1;
extern "C" {
int graphics_preset_current(void) { return GPRESET_ORIGINAL; }
void graphics_preset_apply(int p) { test_preset_applied = p; }
int settings_aa_row(void) { return AA_ROW_FXAA; }
void settings_aa_row_set(int r) { test_aa_row_applied = r; }
int graphics_preset_count(void) { return GPRESET_COUNT; }
const char *const g_gpreset_names[GPRESET_COUNT] = {"Potato", "Original", "High", "Savage"};
// Reached by live Options code (window size rows), never called by this routing test.
void menu_request_window_apply(void) { abort(); }
void winsize_name(WinSize, const char *, char *, int) { abort(); }
// PE resolves unused UI references before discarding their sections. These unrelated
// services must never be called by this routing test, on either test platform.
#ifdef _WIN32
int color_lut_list(void) { abort(); }
void pc_cursor_options_page_changed(void) { abort(); }
void pc_cursor_options_tick(uint64_t) { abort(); }
bool upscaler_available(int) { abort(); }
bool renderer_gpu_rtx(void) { abort(); }
int windisplay_list(WinDisplay *) { abort(); }
int windisplay_target(void) { abort(); }
void windisplay_set(int) { abort(); }
int winsize_list(WinSize *, bool) { abort(); }
WinSize winsize_effective(bool) { abort(); }
WinSize winsize_desktop(void) { abort(); }
float winsize_refresh_rate(void) { abort(); }
bool upscale_modes_available(void) { abort(); }
int color_preset_current(void) { abort(); }
void color_preset_apply(int) { abort(); }
const char *color_lut_name(int) { abort(); }
int color_lut_index(const char *) { abort(); }
const char *color_lut_id(int) { abort(); }
int language_text_count(void) { abort(); }
int language_text_index(void) { abort(); }
const int g_color_preset_count = 0, g_nvoice_languages = 0;
const ColorPreset g_color_presets[] = {{}};
const TextLanguage g_text_languages[] = {{}};
const VoiceLanguage g_voice_languages[] = {{}};
#endif
Cpu *kernel_current_cpu(void) { return &test_cpu; }
void settings_save(void) {}
void menu_size_list_open(void) { assert(false); }
bool menu_size_list_up(void) { return false; }
void rt_request_reboot(int) { assert(false); }
int language_pending(void) { return 0; }
int input_last_device(void) { return DEVICE_KEYBOARD; }
int input_pad_family(void) { return 1; }
void pc_cursor_options_focus(uint64_t, uint64_t row, unsigned kind) {
    focused_row = row; focused_kind = kind; focus_count++;
}
void guest_call(Cpu *c, uint64_t fn) {
    assert(fn == g_site.opt_two_help);
    help_count++;
    c->x[19] = 0xdead; c->v[0] = 42;
}
}

int main() {
    constexpr uint64_t base = 0x7000000000ull;
#ifdef _WIN32
    void *mem = VirtualAlloc((void *)base, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *mem = mmap((void *)base, 0x20000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    assert(mem == (void *)base);
    // Both parts keep the native row plate, centred on the row.
    const auto geometry = binding_row_layout();
    assert(geometry.label_right < geometry.key.x);
    assert(geometry.key.x + geometry.key.width < geometry.pad.x);
    // Both columns and the label sit inside the (widened) row band, and the row is centred.
    const float half_band = binding_row_plate_width * .5f;
    assert(geometry.label_right - geometry.label_width >= -half_band);
    assert(geometry.key.x >= -half_band && geometry.pad.x + geometry.pad.width <= half_band);
    assert(fabsf((geometry.label_right - geometry.label_width) + (geometry.pad.x + geometry.pad.width)) <= 24.f);
    assert(geometry.key.width > 0 && geometry.pad.width > 0);
    uint64_t root = base + 0xa000;
    const char *names[] = {"T_Text_00", "T_Text_01", "N_Cursor_00", "B_Hit_00", "W_Base_00",
                          "W_Base_01", "W_BaseLine_02", "W_BaseLine_01", "W_Decide_00", "Window_00", "W_Base_02"};
    wr64(root + 0x28, root + 0x208);
    for (unsigned i = 0; i < 11; i++) {
        uint64_t pane = root + (i + 1) * 0x200;
        strcpy((char *)(uintptr_t)(pane + 0xb8), names[i]);
        wr64(pane + 0x28, pane + 0x20);
        wr64(pane + 0x10, i == 10 ? root + 0x20 : pane + 0x208);
        two_float(pane, 0x48, 1.f); two_float(pane, 0x50, i == 10 ? 540.f : 276.f);
        wr8(pane + 0x58, 1);
    }
    TwoButtonPanes panes;
    assert(two_button_panes(root, panes));
    two_geometry(panes, geometry.key);
    assert(*(float *)(uintptr_t)(root + 0x48) == 1.f);
    assert(*(float *)(uintptr_t)(panes.texts[0] + 0x48) == 1.f);
    // The part's own plate stays hidden: the toggle part's plate is the row band.
    assert(!(*(uint8_t *)(uintptr_t)(panes.plate + 0x58) & 1));
    two_geometry(panes, geometry.pad);
    assert(!(*(uint8_t *)(uintptr_t)(panes.plate + 0x58) & 1));
    two_geometry(panes, geometry.key);
    assert(*(float *)(uintptr_t)(panes.frames[0] + 0x50) == geometry.key.width);
    assert(*(float *)(uintptr_t)(panes.frames[1] + 0x50) == geometry.key.width);
    assert(*(uint8_t *)(uintptr_t)(panes.frames[0] + 0x58) & 0x10);
    // Column headers share the column centers and widths, hide all button chrome, and scale uniformly.
    assert(binding_row_header_scale > 0.f && two_header_scale() == binding_row_header_scale);
    for (int col = 0; col < 2; col++) {
        const auto column = col ? geometry.pad : geometry.key;
        two_header_geometry(panes, column, binding_row_header_scale);
        assert(*(float *)(uintptr_t)(root + 0x30) == column.center());
        assert(*(float *)(uintptr_t)(panes.texts[0] + 0x30) == 0.f);
        assert(*(float *)(uintptr_t)(panes.texts[0] + 0x48) == binding_row_header_scale);
        assert(*(float *)(uintptr_t)(panes.texts[0] + 0x4c) == binding_row_header_scale);
        assert(fabsf(*(float *)(uintptr_t)(panes.texts[0] + 0x50) * binding_row_header_scale - column.width) < 1e-3f);
        for (uint64_t frame : panes.frames) assert(!(*(uint8_t *)(uintptr_t)(frame + 0x58) & 1));
        assert(!(*(uint8_t *)(uintptr_t)(panes.plate + 0x58) & 1));
    }
    uint64_t header = base + 0xf00, row = base + 0x1000, next = base + 0x1100, ordinary = base + 0x1200;
    uint64_t screen = base + 0x2000;
    g_site.opt_table = base; g_site.opt_two_help = base + 0x8000; g_site.opt_screen_rows = 0x36b0;
    g_installed = 1; wr64(OPT_ENTRIES, g_installed);
    g_items.push_back(button(P_BINDS, "", "test", "", A_BINDS_HEADER, 0));
    wr32(header + 0x50, GAME_ENTRIES); wr64(header, screen);
    wr64(header + 0x20, base + 0x5e00); wr64(header + 0x28, base + 0x5f00);
    for (int i = 0; i < 3; i++) {
        g_items.push_back(button(P_BINDS, "test", "test", nullptr, A_BIND, BIND_ACTION + i));
        uint64_t r = i == 0 ? row : i == 1 ? next : ordinary;
        wr32(r + 0x50, GAME_ENTRIES + 1 + i); wr64(r, screen);
        wr64(r + 0x20, base + 0x6000 + i * 0x200);
        wr64(r + 0x28, base + 0x6100 + i * 0x200);
    }
    g_entries.resize(GAME_ENTRIES + g_items.size());
    for (size_t i = 0; i < g_items.size(); i++) g_entries[GAME_ENTRIES + i].kind = KIND_BUTTON;
    uint64_t row_list = base + 0x2400, order_list = base + 0x2500, msg_out = base + 0x2600;
    wr32(screen + 0x36b0, 4); wr64(screen + 0x36b8, row_list);
    // Reverse row array order vs visual OPT_ORDER: initial focus must still land on row, never header.
    wr64(row_list + 0, ordinary); wr64(row_list + 8, next); wr64(row_list + 16, row); wr64(row_list + 24, header);
    for (uint32_t i = 0; i < 4; i++) wr32(order_list + 4 * i, GAME_ENTRIES + i);
    wr32(OPT_ROWS, 4); wr64(OPT_ORDER, order_list);
    g_two_header = header; g_cur_page = P_BINDS;
    // Fake toggle parts: tog, band, disable, dark, box0, box1 per row (0x400 apart)
    auto fake = [&](uint64_t at, uint64_t r, uint64_t next_row) {
        for (int i = 0; i < 6; i++) wr8(at + i * 0x400 + 0x58, 0x80);
        *(float *)(uintptr_t)(at + 0x30) = 130.f;
        for (int i = 4; i < 6; i++) *(float *)(uintptr_t)(at + i * 0x400 + 0x54) = 40.f;
        (void)r; (void)next_row;
        return TwoRow{r, at, at + 0x400, at + 0x800, at + 0xc00, {at + 0x1000, at + 0x1400}, {-135.f, 53.f}, -1};
    };
    g_two_rows = {fake(base + 0xc000, row, next), fake(base + 0xe000, next, row)};
    const TwoRow &r0 = g_two_rows[0], &r1 = g_two_rows[1];
    auto on = [](uint64_t pl) { return (*(uint8_t *)(uintptr_t)(pl + 0x58) & 1) != 0; };
    auto bright = [](const TwoRow &r) { return *(uint8_t *)(uintptr_t)(r.band + two_mat_alpha[0]) == two_band_selected; };
    auto hovered = [](const TwoRow &r, int col) {
        return *(uint8_t *)(uintptr_t)(r.box[col] + 0x59) == two_hover_alpha &&
               *(uint8_t *)(uintptr_t)(r.box[col] + two_mat_white[0]) == two_hover_tint &&
               *(uint8_t *)(uintptr_t)(r.box[col] + two_mat_white[1] + 2) == two_hover_tint;
    };
    assert(rt_opt_msg(MSG_BASE + MSG_KEY, msg_out) && std::u16string((const char16_t *)(uintptr_t)rd64(msg_out)) == u"Keyboard/Mouse");
    assert(rt_opt_msg(MSG_BASE + MSG_PAD, msg_out) && std::u16string((const char16_t *)(uintptr_t)rd64(msg_out)) == u"Controller");
    assert(rt_opt_msg(MSG_BASE + MSG_TITLE, msg_out) && std::u16string((const char16_t *)(uintptr_t)rd64(msg_out)).empty());
    assert(rt_opt_pointer_kind(header) == -1 && rt_opt_two_column(header) == -1);
    assert(rt_opt_two_control(header) == rd64(row + 0x20) && rd64(screen + 0x36c8) == row);
    int dev, slot, ms;
    wr32(screen + 0x36c0, 0);
    assert(rt_opt_decide(header) == 2 && !bind_capture_active(&dev, &slot, &ms));
    assert(rt_opt_two_callback(screen, rd64(header + 0x28), 2) == 1 && !bind_capture_active(&dev, &slot, &ms));
    focused_row = 0;
    assert(rt_opt_two_callback(screen, rd64(header + 0x28), 0) == 1 && focused_row == row);
    act_reset();
    wr64(screen + 0x36c8, row); wr32(screen + 0x36c0, 0);
    assert(rt_opt_decide(row) == 2);
    assert(bind_capture_active(&dev, &slot, &ms) && dev == BIND_DEV_KEY && slot == BIND_ACTION);
    int before = g_act_key[0];
    SDL_Event e = {}; e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_NORTH;
    bind_capture_event(&e);
    assert(bind_capture_active(&dev, &slot, &ms) && g_act_key[0] == before);
    e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_F8;
    bind_capture_event(&e); assert(!bind_capture_active(&dev, &slot, &ms) && g_act_key[0] == SDL_SCANCODE_F8);
    rt_opt_two_horizontal(screen, 2);
    assert(!bind_capture_active(&dev, &slot, &ms) && focused_kind == KIND_BUTTON && focused_row == row);
    assert(rd32(screen + 0x36c0) == 3);
    rt_opt_decide(row); assert(!bind_capture_active(&dev, &slot, &ms)); // idle is not confirmation
    wr32(screen + 0x36c0, 0); rt_opt_decide(row);
    assert(bind_capture_active(&dev, &slot, &ms) && dev == BIND_DEV_PAD);
    before = g_act_pad[0]; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_F10;
    bind_capture_event(&e); assert(bind_capture_active(&dev, &slot, &ms) && g_act_pad[0] == before);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_NORTH;
    bind_capture_event(&e); assert(g_act_pad[0] == SDL_GAMEPAD_BUTTON_NORTH && g_act_key[0] == SDL_SCANCODE_F8);
    // Native confirmed callbacks can arrive before the screen update sets its action code.
    wr32(screen + 0x36c0, 3);
    rt_opt_two_callback(screen, rd64(row + 0x28), 2);
    assert(bind_capture_active(&dev, &slot, &ms) && dev == BIND_DEV_PAD);
    assert(rd32(screen + 0x36c0) == 3);
    bind_capture_cancel();
    // Up/down enters a different row's left control but preserves the right column.
    test_cpu.x[19] = 17; test_cpu.v[0] = 23;
    assert(rt_opt_two_callback(screen, rd64(next + 0x20), 0));
    assert(focused_row == next && focused_kind == KIND_BUTTON && rd64(screen + 0x36c8) == next);
    assert(test_cpu.x[19] == 17 && test_cpu.v[0] == 23 && help_count == 1);
    // The selected row's band is bright over the whole row, the other row's faint. The unselected button of the selected
    // row gets the darkener under it, the selected button's box is a little see-through.
    assert(on(r0.band) && on(r1.band) && !bright(r0) && bright(r1));
    assert(on(r1.disable) && !on(r0.disable) && r1.dark_col == 0);
    assert(*(float *)(uintptr_t)(r1.dark + 0x30) == r1.dark_x[0]);
    assert(hovered(r1, 1) && !hovered(r1, 0) && !hovered(r0, 0) && !hovered(r0, 1));
    // Explicit mouse hover selects either device even after crossing rows.
    rt_opt_two_pointer(row, KIND_CHOICE);
    assert(rt_opt_two_callback(screen, rd64(row + 0x20), 0) && rt_opt_two_column(row) == 0);
    assert(bright(r0) && !bright(r1) && on(r0.disable) && !on(r1.disable) && r0.dark_col == 1);
    assert(*(float *)(uintptr_t)(r0.dark + 0x30) == r0.dark_x[1] && hovered(r0, 0) && !hovered(r0, 1));
    // A redraw by the game that hides the toggle part again is undone on the next update.
    wr8(r0.tog + 0x58, 0x10); wr8(r0.band + two_mat_alpha[0], 0x02);
    two_band_refresh(screen);
    assert(on(r0.tog) && bright(r0));
    assert(!rt_opt_two_callback(screen, rd64(ordinary + 0x28), 0));
    assert(!bright(r0) && !bright(r1) && !on(r0.disable) && !on(r1.disable) && !hovered(r0, 0));
    assert(rt_opt_two_column(ordinary) == -1 && !rt_opt_two_control(ordinary));
    wr32(screen + 0x36c0, 0); rt_opt_decide(ordinary);
    assert(bind_capture_active(&dev, &slot, &ms) && dev == BIND_DEV_ANY);
    bind_capture_cancel();

    // Verify Graphics preset and AA row contract stubs
    assert(graphics_preset_count() == 4);
    assert(!strcmp(g_gpreset_names[GPRESET_POTATO], "Potato"));
    assert(!strcmp(g_gpreset_names[GPRESET_SAVAGE], "Savage"));

    // Verify Video & Advanced section items
    g_items.clear();
    build_items();

    const Item *preset_item = nullptr;
    const Item *adv_button = nullptr;
    const Item *hud_edges_item = nullptr;
    const Item *drawdist_item = nullptr;
    const Item *rscale_item = nullptr;
    const Item *shadow_item = nullptr;
    const Item *aa_item = nullptr;
    const Item *mvec_item = nullptr;
    const Item *fgen_item = nullptr;

    const Item *bloom_item = nullptr;
    const Item *dof_item = nullptr;
    const Item *dynres_item = nullptr;
    const Item *aniso_item = nullptr;
    const Item *texlod_item = nullptr;
    const Item *wetfix_item = nullptr;
    const Item *haze_item = nullptr;
    const Item *guides_item = nullptr;

    for (const auto &it : g_items) {
        if (it.page == P_VIDEO) {
            if (!strcmp(it.title, "Graphics preset")) preset_item = &it;
            else if (!strcmp(it.title, "Advanced options")) adv_button = &it;
            else if (!strcmp(it.title, "HUD at edges")) hud_edges_item = &it;
            else if (!strcmp(it.title, "Draw distance")) drawdist_item = &it;
            else if (!strcmp(it.title, "Render scale")) rscale_item = &it;
            else if (!strcmp(it.title, "Shadow resolution")) shadow_item = &it;
            else if (!strcmp(it.title, "Anti-aliasing")) aa_item = &it;
            else if (!strcmp(it.title, "Motion vectors")) mvec_item = &it;
            else if (!strcmp(it.title, "Frame generation (AMD FSR 3)")) fgen_item = &it;
        } else if (it.page == P_VIDEO_ADV) {
            if (!strcmp(it.title, "Bloom")) bloom_item = &it;
            else if (!strcmp(it.title, "Depth of field")) dof_item = &it;
            else if (!strcmp(it.title, "Dynamic resolution")) dynres_item = &it;
            else if (!strcmp(it.title, "Anisotropic filtering")) aniso_item = &it;
            else if (!strcmp(it.title, "Texture sharpness")) texlod_item = &it;
            else if (!strcmp(it.title, "Wet grass fix")) wetfix_item = &it;
            else if (!strcmp(it.title, "Haze")) haze_item = &it;
        } else if (it.page == P_GAMEPLAY) {
            if (!strcmp(it.title, "Transparent control guides")) guides_item = &it;
        }
    }

    assert(preset_item != nullptr);
    assert(adv_button != nullptr);
    assert(hud_edges_item != nullptr);
    assert(drawdist_item != nullptr);
    assert(rscale_item != nullptr);
    assert(shadow_item != nullptr);
    assert(aa_item != nullptr);
    assert(mvec_item != nullptr);
    assert(fgen_item != nullptr);

    assert(bloom_item != nullptr);
    assert(dof_item != nullptr);
    assert(dynres_item != nullptr);
    assert(aniso_item != nullptr);
    assert(texlod_item != nullptr);
    assert(wetfix_item != nullptr);
    assert(haze_item != nullptr);
    assert(guides_item != nullptr);

    // Test preset getter and setter
    assert(preset_item->get(*preset_item) == GPRESET_ORIGINAL);
    preset_item->set(*preset_item, GPRESET_SAVAGE);
    assert(test_preset_applied == GPRESET_SAVAGE);

    // Test AA getter and setter
    assert(aa_item->get(*aa_item) == AA_ROW_FXAA);
    aa_item->set(*aa_item, AA_ROW_SMAA);
    assert(test_aa_row_applied == AA_ROW_SMAA);

    // Test Advanced toggles
    bloom_item->set(*bloom_item, 0);
    assert(g_settings.bloom == false);
    dof_item->set(*dof_item, 0);
    assert(g_settings.depth_of_field == false);
    dynres_item->set(*dynres_item, 0);
    assert(g_settings.dynamic_res == false);
    aniso_item->set(*aniso_item, 3);
    assert(g_settings.anisotropy == 3);
    texlod_item->set(*texlod_item, TEX_LOD_SHARPER);
    assert(g_settings.tex_lod_bias == TEX_LOD_SHARPER);
    wetfix_item->set(*wetfix_item, 1);
    assert(g_settings.wet_fix == true);
    hud_edges_item->set(*hud_edges_item, 1);
    assert(g_settings.hud_at_edges == true);
    rscale_item->set(*rscale_item, RSCALE_150);
    assert(g_settings.render_scale == RSCALE_150);

    puts("PASS: real Options hooks and capture engine, separate devices, navigation, mouse, CPU preservation, fallback");
}
