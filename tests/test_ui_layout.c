#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../runtime/ui_layout.c"
#include "../runtime/nvn/cmdbuf.c"
#include "../runtime/nvn/hud_scale.h"

GameSites g_site;
const char *g_game_version = "1.9.0";
Settings g_settings = {.hud_edges = true, .hud_size = 100};
void kmutex_lock(KMutex *m) { assert(!pthread_mutex_lock(&m->pm)); }
void kmutex_unlock(KMutex *m) { assert(!pthread_mutex_unlock(&m->pm)); }
uint64_t renderer_present_count(void) { return 1; }
uint32_t kernel_current_thread_handle(void) { return 1; }
uint64_t plat_now_ns(void) { return 1; }
bool nvn_timeline(void) { return false; }
void rt_fatal_trace(void) { abort(); }
void guest_call(Cpu *c, uint64_t address) { (void)c; (void)address; assert(!"unexpected guest callback"); }

static bool pause_menu;
bool framepace_menu_ui(void) { return pause_menu; }
static unsigned calls;
static void native_draw(Cpu *c) { calls++; c->x[5] = 123; }

static void native_pane(Cpu *c) {
    if (rt_ui_layout_pane_draw(c, native_pane)) return;
    calls++; c->x[5] = 456;
}

static unsigned scope_at(uint64_t at) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    if (r->op == CMD_JUMP) r = (CmdRec *)(uintptr_t)r->x[0];
    assert(r->op == CMD_UI_SCOPE);
    return r->x[0];
}

int main(void) {
    // Unsupported versions and disabled options must not inspect version-specific guest fields.
    Cpu unsupported = {0};g_site.screen_mgr=1;g_game_version="unknown";
    assert(!ui_layout_begin(&unsupported));
    g_game_version="1.9.0";g_settings.hud_edges=false;
    assert(!ui_layout_begin(&unsupported));
    g_site.screen_mgr=0;g_settings.hud_edges=true;
    float crop[27] = {0};crop[23] = 1.8359375f;crop[24] = .440625f;crop[25] = .17777778f;
    assert(hud_capture_crop(crop, .9f, -.1f));
    assert(fabsf((.5f+crop[23]*crop[25])*800-675)<.001f);
    assert(fabsf(crop[25]-.16f)<.00001f);
    crop[22]=.3f;crop[23]=1.1f;crop[25]=.2f;crop[26]=.6f;
    float u0=.5f+crop[24]*(cosf(crop[26])*crop[22]-sinf(crop[26])*crop[23]);
    float v0=.5f+crop[25]*(sinf(crop[26])*crop[22]+cosf(crop[26])*crop[23]);
    assert(hud_capture_crop(crop,.9f,-.1f));
    float u1=.5f+crop[24]*(cosf(crop[26])*crop[22]-sinf(crop[26])*crop[23]);
    float v1=.5f+crop[25]*(sinf(crop[26])*crop[22]+cosf(crop[26])*crop[23]);
    assert(fabsf(u1-u0)<1e-6f&&fabsf(v1-(.5f+.9f*(v0-.5f)+.05f))<1e-6f);
    crop[25]=0;assert(!hud_capture_crop(crop,.9f,-.1f));
    uint64_t base = HEAP_BASE;
#ifdef _WIN32
    assert(VirtualAlloc((void *)(uintptr_t)base, 0x10000, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE) == (void *)(uintptr_t)base);
#else
    assert(mmap((void *)(uintptr_t)base, 0x10000, PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
#endif
    uint64_t manager = base + 0x100, slots = base + 0x200, screen = base + 0x1000;
    uint64_t context = base + 0x2000, wrapper = base + 0x3000, commands = base + 0x4000;
    g_site.screen_mgr = base;
    ST64(base, manager); ST32(manager+0x28, 99); ST64(manager+0x30, slots);
    ST64(slots+8*0x25, screen); ST64(context+0x20, wrapper);
    ST64(wrapper+8, 0x11); ST64(wrapper+0xb8, commands);
    Cpu c = {0}; c.x[0] = screen; c.x[1] = context;
    // Inactive layouts must return before touching an unusable render context.
    c.x[1] = 0; assert(!ui_layout_begin(&c));
    rt_ui_layout_draw(&c, native_draw); assert(calls == 1); calls = 0;
    ST8(screen+0x100, 1); ST8(screen+0xfe, 2); c.x[1] = context;
    for (unsigned i = 0; i < 4; i++) {
        ST8(screen+0x100, i == 0 ? 0 : 1); ST8(screen+0x102, i == 1);
        ST8(screen+0xfe, i == 2 ? 0 : 3); ST8(screen+0xff, i == 3 ? 2 : 0);
        c.x[1] = 0; assert(!ui_layout_begin(&c));
    }
    ST8(screen+0x100, 1); ST8(screen+0x102, 0); ST8(screen+0xfe, 3); ST8(screen+0xff, 0);
    c.x[1] = context;
    ST64(commands, 0x11); // the previously crashing non-NVN object is rejected without dereference
    assert(!ui_layout_begin(&c));
    Cpu init = {0}; init.x[0] = commands; hle_nvnCommandBufferInitialize(&init);
    assert(!ui_layout_begin(&c)); // initialized but not recording
    cb_of(commands)->recording = true;
    assert(!ui_layout_begin(&c)); // no initial host storage; must not call the guest allocator
    cb_of(commands)->recording = false;
    init.x[0] = commands; hle_nvnCommandBufferBeginRecording(&init);
    NvnCmdBuf *cb = cb_of(commands);
    cb->gused = cb->gsize + 16; cb->callback = 123;
    uint64_t guest_used = cb->gused;
    Cpu original = c; rt_ui_layout_draw(&c, native_draw);
    original.x[5] = 123;
    assert(!memcmp(&c, &original, sizeof c) && calls == 1 && cb->gused == guest_used);
    CmdRec *begin = (CmdRec *)(uintptr_t)(cb->start + sizeof(CmdRec));
    CmdRec *end = (CmdRec *)((char *)begin + begin->size);
    assert(begin->op == CMD_UI_SCOPE && begin->x[0] == UI_LAYOUT_HUD);
    assert(end->op == CMD_UI_SCOPE && end->x[0] == UI_LAYOUT_NONE);
    // MainScreen3D and Time use the same stable HUD policy as the flat HUD.
    ST64(slots+8*0x25, 0); ST64(slots+8*0x2, screen);
    uint64_t at = cb->mem + cb->used;
    rt_ui_layout_draw(&c, native_draw);
    assert(scope_at(at) == UI_LAYOUT_HUD);
    ST64(slots+8*0x2, 0); ST64(slots+8*0x1c, screen);
    at = cb->mem + cb->used; rt_ui_layout_draw(&c, native_draw);
    assert(scope_at(at) == UI_LAYOUT_HUD);
    ST64(slots+8*0x1c, 0); ST64(slots+8*0x2a, screen);
    for (unsigned paused = 0; paused < 2; paused++) {
        pause_menu = paused; at = cb->mem + cb->used;
        rt_ui_layout_draw(&c, native_draw);
        assert(scope_at(at) == (paused ? UI_LAYOUT_MENU : UI_LAYOUT_HUD));
    }
    // Native MiniMap endpoints, not the global pause state, choose the presentation.
    uint64_t animation = base + 0x6000, resource = base + 0x7000;
    ST64(screen+0x3640, animation); ST64(animation+0x18, resource); ST16(resource+8, 1);
    float map_frame = 0; memcpy((void *)(uintptr_t)(animation+0x20), &map_frame, 4);
    pause_menu = false; assert(ui_layout_map_mode(screen) == 1);
    at = cb->mem + cb->used; rt_ui_layout_draw(&c, native_draw); assert(scope_at(at) == UI_LAYOUT_MENU);
    map_frame = 1; memcpy((void *)(uintptr_t)(animation+0x20), &map_frame, 4);
    pause_menu = true; assert(ui_layout_map_mode(screen) == 0);
    at = cb->mem + cb->used; rt_ui_layout_draw(&c, native_draw); assert(scope_at(at) == UI_LAYOUT_HUD);
    map_frame = NAN; memcpy((void *)(uintptr_t)(animation+0x20), &map_frame, 4);
    assert(ui_layout_map_mode(screen) == -1);
    ST64(animation+0x18, GUEST_HI-4); assert(ui_layout_map_mode(screen) == -1);
    ST64(screen+0x3640, 0); assert(ui_layout_map_mode(screen) == -1);
    ST64(slots+8*0x2a, 0);
    // Quest notifications, item cards and complete quick selectors retain native geometry.
    const unsigned native_slots[] = {0xb, 0x27, 0x2d};
    for (unsigned i = 0; i < 3; i++) {
        ST64(slots+8*native_slots[i], screen); at = cb->mem + cb->used;
        rt_ui_layout_draw(&c, native_draw); assert(scope_at(at) == UI_LAYOUT_DIALOGUE);
        ST64(slots+8*native_slots[i], 0);
    }
    ST64(slots+8*0x4a, screen);
    at = cb->mem + cb->used; rt_ui_layout_draw(&c, native_draw);
    assert(scope_at(at) == UI_LAYOUT_TALK);
    // The quest title is a part inside MainScreen, not the separate ChallengeWin screen.
    uint64_t pane = base + 0x8000, pane_wrapper = base + 0x9000, controller = base + 0xa000;
    ST64(slots+8*0x25, screen); ST64(screen+0x3cc0, controller);
    memcpy((void *)(uintptr_t)(pane+0xb8), "Pa_QuestName_00", sizeof "Pa_QuestName_00");
    ST64(pane+0xf0, controller); ST64(pane_wrapper+0xb0, commands);
    Cpu part = {0}; part.x[0] = pane; part.x[2] = pane_wrapper;
    assert(nvn_ui_scope(commands, UI_LAYOUT_HUD));
    unsigned previous_calls = calls; Cpu expected = part; expected.x[5] = 456;
    at = cb->mem + cb->used;
    assert(rt_ui_layout_pane_draw(&part, native_pane));
    assert(calls == previous_calls+1 && !memcmp(&part, &expected, sizeof part));
    CmdRec *part_begin = (CmdRec *)(uintptr_t)at;
    if (part_begin->op == CMD_JUMP) part_begin = (CmdRec *)(uintptr_t)part_begin->x[0];
    CmdRec *part_end = (CmdRec *)((char *)part_begin+part_begin->size);
    assert(part_begin->op == CMD_UI_SCOPE && part_begin->x[0] == UI_LAYOUT_DIALOGUE);
    assert(part_end->op == CMD_UI_SCOPE && part_end->x[0] == UI_LAYOUT_NONE);
    assert(rt_ui_layout_pane_draw(&part, native_pane) && calls == previous_calls+2);
    ST64(pane+0xf0, controller+8); assert(!rt_ui_layout_pane_draw(&part, native_pane));
    ST64(pane+0xf0, controller); part.x[2] = 0; assert(!rt_ui_layout_pane_draw(&part, native_pane));
    assert(nvn_ui_scope(commands, UI_LAYOUT_NONE));
    // Nested cached lists restore the executing owner, including bounded overflow.
    HudScope scope = {0};
    hud_scope_enter(&scope, UI_LAYOUT_HUD); hud_scope_enter(&scope, UI_LAYOUT_DIALOGUE);
    assert(scope.kind == UI_LAYOUT_DIALOGUE);
    hud_scope_leave(&scope); assert(scope.kind == UI_LAYOUT_HUD);
    for (int i = 0; i < 40; i++) hud_scope_enter(&scope, UI_LAYOUT_CONTEXT);
    for (int i = 0; i < 40; i++) hud_scope_leave(&scope);
    assert(scope.kind == UI_LAYOUT_HUD);
    hud_scope_leave(&scope); assert(!scope.kind && !scope.depth && !scope.overflow);
    cb->gused = 0; cb->callback = 0;
    init.x[0] = commands; hle_nvnCommandBufferEndRecording(&init);
    init.x[0] = commands; hle_nvnCommandBufferFinalize(&init);
    assert(!ui_layout_begin(&c) && !g_command_buffers);
    cmdbuf_shadow_reset();
    assert(!g_command_buffers);
#ifdef _WIN32
    VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE);
#else
    munmap((void *)(uintptr_t)base, 0x10000);
#endif
    puts("PASS: native eligibility, invalid/stopped NVN contexts, balanced deferred scopes, zero guest cost, CPU preservation and lifetime reset");
}
