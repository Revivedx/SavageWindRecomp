// Native-selector ownership, CPU preservation and deferred replacement handoff.
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/quick_replace.c"


static void tick_original(Cpu *), fill_original(Cpu *), popup_original(Cpu *), item_popup_original(Cpu *);
static void receive_original(Cpu *), card_action_original(Cpu *), close_gate_original(Cpu *);
static void chest_original(Cpu *), query_original(Cpu *), full_info_original(Cpu *);
static void pane_original(Cpu *), anim_original(Cpu *);
static GuestFn replace_ui_draw_original;
static void replace_tick(Cpu *c) { rt_quick_replace_tick(c, tick_original); }
static void replace_fill(Cpu *c) { rt_quick_replace_fill(c, fill_original); }
static void replace_popup(Cpu *c) { rt_quick_replace_popup(c); popup_original(c); }
static void replace_item_popup(Cpu *c) { rt_quick_replace_item_popup(c); item_popup_original(c); }
static void replace_receive(Cpu *c) { if (!rt_quick_replace_receive(c)) receive_original(c); }
static void replace_card_action(Cpu *c) { if (!rt_quick_replace_card_action(c)) card_action_original(c); }
static void replace_card_close_gate(Cpu *c) { if (!rt_quick_replace_card_close(c)) close_gate_original(c); }
static void replace_pane_draw(Cpu *c) { if (!rt_quick_replace_pane(c)) pane_original(c); }
static void replace_anim_advance(Cpu *c) { if (!rt_quick_replace_anim(c)) anim_original(c); }
static void replace_chest_update(Cpu *c) {
    uint64_t ai = c->x[0]; unsigned mode = rt_quick_replace_chest_begin(c);
    if (!mode) return;
    chest_original(c); rt_quick_replace_chest_end(c, ai, mode);
}
static void replace_query(Cpu *c) { uint64_t query = c->x[0]; query_original(c); rt_quick_replace_query(c, query); }
static void replace_full_info(Cpu *c) {
    Cpu native = *c; native.x[0] += 0x20; rt_quick_replace_full_info(&native, full_info_original);
}
static void replace_ui_draw(Cpu *c) { rt_quick_replace_ui_draw(c, replace_ui_draw_original); }
static void test_selector_draw(Cpu *c) { replace_selector_draw(c, replace_ui_draw_original); }

static bool developer_enabled;
bool control_enabled(void) { return developer_enabled; }

static const uint64_t manager = 0x7240000000ull;
static const uint64_t input = 0x7240070000ull;
static const uint64_t screen = 0x7240080000ull;
static uint64_t buttons, consumed;
static uint32_t expected_held, expected_edge;
static unsigned ticks, fills, capacities;
static unsigned native_popups, overrides;
static unsigned native_helper_calls;
static HostFn installed_tick, installed_popup, installed_fill, installed_ui_draw, installed_card_draw;
static bool inspect_input, chest_ready = true, chest_capacity;
static bool emulate_direction_latch;
static unsigned open_attempts;
static unsigned chest_calls, predicate_calls, releases, drops;
static unsigned action_ticks, allocations, owner_refs;
static bool defer_drop;
static bool chest_emits_receive;
static bool chest_defer_receive;
static bool chest_poison_on_defer;
static bool weak_ref;
static bool index_capture_fails;
static bool resolve_refused;
static bool clobber_release_stack;
static unsigned resolve_calls;
static unsigned index_blocks, index_retains, index_releases;
static QuickDropResult drop_result;
GameSites g_site;
static unsigned item_popup_calls, full_info_calls, toast_draws, toast_calcs;
static unsigned pane_calls, anim_calls, apply_calls;
static unsigned tinted_calcs;
static unsigned card_opens, card_closes, receive_calls, card_action_calls, close_gate_calls;
static void close_gate_original(Cpu *c) { (void)c; close_gate_calls++; }
static void receive_original(Cpu *c) { (void)c; receive_calls++; }
static void card_action_original(Cpu *c) { (void)c; card_action_calls++; }
static void pane_original(Cpu *c) { (void)c; pane_calls++; }
static void anim_original(Cpu *c) { assert((uint32_t)c->v[0] == 0x3f800000); anim_calls++; }
static void query_original(Cpu *c) { c->x[0] = 0; }
static void full_info_original(Cpu *c) { (void)c; full_info_calls++; }
static uint64_t presents = 100, ground_actor;
static int ground_result = 7;
uint64_t renderer_present_count(void) { return presents; }
void *hle_alloc(uint64_t size) {
    assert(size == 0x18 || size == 0x10);
    void *p = (void *)(uintptr_t)(manager + 0x88000 + allocations++ * 0x20);
    memset(p, 0, size); return p;
}
Settings g_settings;
const char *g_game_version = "1.9.0";
#define feature_enabled g_settings.quick_replace
bool quick_drop_action_enabled(void) { return g_settings.quick_drop; }
bool quick_drop_action_init(void) { return true; }
void quick_drop_action_request(Cpu *c, uint64_t scr, uint64_t mgr, uint64_t item,
                               int tab, int slot, const char *name) {
    (void)c; assert(scr == screen && mgr == manager && item == input + 0x2000);
    assert(tab == 0 && slot == 0 && !strcmp(name, "Weapon_Bow_002")); drops++;
    drop_result = QUICK_DROP_PENDING;
}
void quick_drop_action_close(Cpu *c) {
    (void)c;
    if (drop_result == QUICK_DROP_PENDING && !defer_drop) {
        chest_capacity = true; ground_result = 0; drop_result = QUICK_DROP_COMPLETE;
    }
}
void quick_drop_action_tick(Cpu *c) { (void)c; action_ticks++; }
QuickDropResult quick_drop_action_result(void) { return drop_result; }
bool quick_drop_action_busy(void) { return defer_drop && drop_result == QUICK_DROP_PENDING; }
static void chest_original(Cpu *c) {
    chest_calls++;
    if (chest_defer_receive) {
        if (chest_poison_on_defer) assert(!mprotect((void *)(uintptr_t)c->x[0], 0x1000, PROT_NONE));
        return;
    }
    ST8(c->x[0] + 0xb0, 0); // Native handler consumes pending receive context.
    if (chest_emits_receive || replace_receive_ai == c->x[0]) {
        Cpu receive = *c;
        receive.x[0] = LD64(c->x[0] + 8);
        receive.x[1] = LD64(c->x[0] + 0x70);
        receive.x[4] = c->x[0] + 0xc8;
        replace_receive(&receive);
    }
}
void guest_call(Cpu *c, uint64_t address) {
    native_helper_calls++;
    if (address == 0x7000f6c9b4ull) { replace_anim_advance(c);
    } else if (address == 0x7000e78a30ull) { replace_popup(c);
    } else if (address == 0x70013e942cull) {
        assert(c->x[0] == manager + 0x64000 &&
               (c->x[1] == input + 0x600 || c->x[1] == input + 0x1a08)); card_opens++;
    } else if (address == 0x7000d91414ull) {
        c->x[0] = 1; // Bow, matching the incoming test actor.
    } else if (address == 0x7000573698ull) {
        strcpy((char *)(uintptr_t)LD64(c->x[0] + 8), (char *)(uintptr_t)LD64(c->x[1] + 8));
    } else if (address == 0x70013e99c4ull) {
        card_closes++;
    } else if (address == 0x7000e7a7f0ull) {
        item_popup_calls++;
    } else if (address == 0x7000022c60ull) {
        apply_calls++;
    } else if (address == 0x7000025c64ull) {
        assert(c->x[0] == manager + 0x54000 && c->x[1] == manager + 0x53000); toast_calcs++;
        if (LD8(manager + 0x826e8) == 255 && LD8(manager + 0x826e9) == 218 &&
            LD8(manager + 0x826ea) == 68) tinted_calcs++;
    } else if (address == 0x7000025cd0ull) {
        uint64_t info = manager + 0x53000;
        assert(c->x[0] == manager + 0x51000 && c->x[1] == info && c->x[2] == manager + 0x56008);
        assert(LD64(info + 0xf8) == manager + 0x55000 && LD64(info + 0xc0) == manager + 0x54000);
        assert(LD16(info + 0xeb) == 6 && LD8(info + 0xed) == 0 && LD8(info + 0xef) == 1); toast_draws++;
    } else if (address == 0x7000f60944ull) {
        assert(c->x[0] == manager + 0x53000);
    } else if (address == 0x7001537800ull) {
        predicate_calls++;
        assert(c->x[0] == input + 0x400 || c->x[0] == manager + 0x69000);
        assert(LD64(c->x[1] + 8) == 0x700164f739ull);
        c->x[0] = chest_ready;
    } else if (address == 0x7000be21e0ull) {
        if (ground_actor && c->x[0] == ground_actor + 8) {
            c->x[0] = ground_result == 0;
        } else {
            assert(c->x[0] == input + 0x600 || c->x[0] == input + 0x200 || c->x[0] == input + 0x4028 ||
                   c->x[0] == c->sp + 0x10);
            if (c->x[0] == c->sp + 0x10) {
                assert(LD64(c->x[0]) == manager + 0x10);
                assert(!strcmp((char *)(uintptr_t)LD64(c->x[0] + 8), replace_incoming));
            }
            c->x[0] = chest_capacity;
        }
    } else if (address == 0x7001070ce0ull) {
        memcpy((void *)(uintptr_t)c->x[0], (void *)(uintptr_t)c->x[1], 0x18);
        if (weak_ref) ST8(c->x[0], 0);
    } else if (address == 0x70007b4ee0ull) {
        uint64_t actor = LD64(c->x[0] + 8);
        c->x[0] = actor ? actor + 0xf98 : 0;
    } else if (address == 0x7001126464ull) {
        releases++; memset((void *)(uintptr_t)c->x[0], 0, 0x18);
    } else if (address == 0x700112649cull) {
        ST8(c->x[0], !weak_ref); ST64(c->x[0] + 8, c->x[1]); owner_refs++; c->x[0] = 1;
    } else if (address == 0x7001517e60ull) {
        uint64_t out = c->x[0], actor = c->x[1];
        assert(c->x[2] == 1);
        if (LD8(out + 0xc)) { ST32(LD64(out) + 0x50, LD32(LD64(out) + 0x50) - 1); index_releases++; }
        ST8(out + 0xc, 0); ST32(out + 8, UINT32_MAX);
        if (index_capture_fails || LD8(actor + 0x64) == 3) c->x[0] = 0;
        else {
            uint64_t block = LD64(actor + 0x68);
            if (!block) {
                block = manager + 0x30000 + index_blocks++ * 0x60;
                ST64(actor + 0x68, block); ST32(actor + 0x60, index_blocks);
                ST32(block + 0x40, index_blocks); ST64(block + 0x48, actor);
            }
            ST64(out, block); ST32(out + 8, LD32(actor + 0x60)); ST8(out + 0xc, 1);
            ST32(block + 0x50, LD32(block + 0x50) + 1); index_retains++; c->x[0] = 1;
        }
    } else if (address == 0x7001517abcull) {
        resolve_calls++;
        uint64_t index = c->x[0], ordinary = c->x[1], block = LD64(index);
        assert(ordinary && c->x[2] == 0);
        if (resolve_refused || LD32(index + 8) == UINT32_MAX || LD32(index + 8) != LD32(block + 0x40)) c->x[0] = 0;
        else {
            uint64_t actor = LD64(block + 0x48);
            ST8(ordinary, !weak_ref); ST64(ordinary + 8, actor); c->x[0] = actor;
        }
    } else if (address == 0x7001517f8cull) {
        uint64_t index = c->x[0];
        if (LD8(index + 0xc)) { ST32(LD64(index) + 0x50, LD32(LD64(index) + 0x50) - 1); index_releases++; }
        ST8(index + 0xc, 0); ST32(index + 8, UINT32_MAX);
    } else if (address == 0x70010d544cull) {
        ST8(c->x[1], 1);
        ST64(c->x[1] + 8, ground_actor);
    } else if (address == 0x7000e780f0ull) c->x[0] = ground_result;
    else if (address == 0x7000eaff08ull || address == 0x7000eaff18ull) {
        assert(c->x[0] == manager + 0x28);
    } else if (address == 0x7000d970a8ull) c->x[0] = 1;
    else if (address == 0x7000d992acull) c->x[0] = input + 0x2000;
    else assert(0);
    if (clobber_release_stack && (address == 0x7001126464ull || address == 0x7001517f8cull))
        memset((void *)(uintptr_t)(c->sp - 0x100), 0xa5, 0x100);
    c->x[4] = 0xdead; c->nzcv = 0x12340000;
}
uint64_t input_buttons_now(void) { return buttons; }
void input_consume_buttons(uint64_t mask) { consumed |= mask; }
static void tick_original(Cpu *c) {
    quick_drop_action_tick(c);
    ticks++;
    if (inspect_input) {
        assert(LD32(input + 0x114) == expected_held);
        assert(LD32(input + 8) == expected_edge);
    }
    if (emulate_direction_latch) {
        uint32_t direction = LD32(input + 0x114) & 0xf0000u;
        uint32_t previous = LD32(manager + 0x64c58);
        ST32(manager + 0x64c58, direction);
        // Native stores held input before availability, including on failure.
        if (direction && !previous && ++open_attempts == 2) {
            ST32(manager + 0x64c24, 3);
            ST32(manager + 0x64c2c, 1);
        }
    }
    c->x[5] = 0x12345678;
}
static void fill_original(Cpu *c) { (void)c; fills++; }
static Cpu capacity_result;
static void capacity_original(Cpu *c) { capacities++; *c = capacity_result; }
static void popup_original(Cpu *c) { (void)c; native_popups++; }
static void item_popup_original(Cpu *c) { (void)c; item_popup_calls++; }
// Model MessageGet's direct tail-call: no dispatch lookup of the base entry,
// so overriding only the base cannot intercept a natural derived-card draw.
static void item_card_thunk_original(Cpu *c) { replace_ui_draw_original(c); }
GuestFn dispatch_lookup(uint64_t addr) {
    switch (addr) {
    case 0x7000e68168ull: return tick_original;
    case 0x7000e27c2cull: return fill_original;
    case 0x7000e78a30ull: return popup_original;
    case 0x7000e7a7f0ull: return item_popup_original;
    case 0x7000be21e0ull: return capacity_original;
    case 0x7000a9d1bcull: return chest_original;
    case 0x7000b445c0ull: return query_original;
    case 0x70005e6e48ull: return full_info_original;
    case 0x7000be1e30ull: return receive_original;
    case 0x7000709ba0ull: return card_action_original;
    case 0x70013ed264ull: return close_gate_original;
    case 0x7000e2b838ull: return item_card_thunk_original;
    case 0x7000d91414ull:
    case 0x7000573698ull:
    case 0x7000eaff08ull:
    case 0x7000eaff18ull: return popup_original;
    case 0x70013e942cull:
    case 0x7000f6e334ull:
    case 0x7000025e40ull:
    case 0x70013e99c4ull:
    case 0x70013ee084ull:
    case 0x7000daa3fcull:
    case 0x7000da9a0cull:
    case 0x70013eb788ull: return popup_original;
    case 0x7000027d30ull: return pane_original;
    case 0x7000f6c9b4ull: return anim_original;
    case 0x7000f60944ull:
    case 0x7000025c64ull:
    case 0x7000025cd0ull: return popup_original;
    case 0x7001537800ull:
    case 0x7001070ce0ull:
    case 0x70007b4ee0ull:
    case 0x7001126464ull:
    case 0x700112649cull:
    case 0x7001517e60ull:
    case 0x7001517abcull:
    case 0x7001517f8cull:
    case 0x70010d544cull:
    case 0x7000e780f0ull: return popup_original;
    default: return popup_original;
    }
}

static void tick_with(uint32_t held, uint32_t edge) {
    ST32(input + 0x114, held);
    ST32(input + 8, edge);
    Cpu c = {0}; c.x[0] = manager; c.sp = input + 0x1000;
    unsigned before = ticks, before_actions = action_ticks;
    replace_tick(&c);
    assert(ticks == before + 1 && c.x[5] == 0x12345678);
    assert(action_ticks == before_actions + 1);
    assert(LD32(input + 0x114) == held && LD32(input + 8) == edge);
}

static void test_toast_scope(void) {
    uint64_t toast = manager + 0x50000, other = manager + 0x51000;
    Cpu c = {.sp = input + 0x1000}; c.x[0] = toast;
    Cpu before = c;
    replace_category = 0;
    for (unsigned saved = 0; saved < 256; saved += 127) {
        replace_toast = toast; ST8(toast + 0x1a8, saved); ST8(other + 0x1a8, 0x55);
        int token = rt_quick_replace_toast_begin(&c);
        assert(token == (int)saved && LD8(toast + 0x1a8) == 1);
        // A nested update may publish another toast. Restore the invocation's
        // original object, independently of the current global presentation.
        replace_toast = other;
        rt_quick_replace_toast_end(toast, token);
        assert(LD8(toast + 0x1a8) == saved && LD8(other + 0x1a8) == 0x55);
        assert(!memcmp(&c, &before, sizeof c));
    }
    replace_toast = 0; c.x[0] = GUEST_LO + 0x1000;
    assert(rt_quick_replace_toast_begin(&c) == -1);
    rt_quick_replace_toast_end(c.x[0], -1); // Inactive callbacks never dereference it.
    quick_replace_reset();
}

static void test_boot_disabled_live_enable(void) {
    assert(!feature_enabled && !g_settings.quick_drop);
    unsigned saved_ticks = ticks, saved_actions = action_ticks, saved_popups = native_popups, saved_fills = fills;
    uint64_t source = input + 0x1600, status = input + 0x1700, actor = input + 0x1a00;
    ST8(source, 1); ST64(source + 8, actor); ST8(actor + 0x64, 1);
    ST64(actor + 0x10, input + 0x300);
    strcpy((char *)(uintptr_t)(input + 0x300), "Weapon_Bow_001");
    ST32(status, 7); ST32(actor + 0xf98, 0);
    Cpu popup = {.sp = input + 0x1000}; popup.x[0] = source; popup.x[1] = status;
    Cpu original = popup;
    replace_popup(&popup);
    assert(!memcmp(&popup, &original, sizeof popup) && native_popups == saved_popups + 1);
    assert(replace_category == -1 && !replace_handle);
    feature_enabled = true;
    popup = original; replace_popup(&popup);
    assert(replace_category == 2 && replace_handle && LD64(replace_handle + 8) == actor);
    Cpu fill = {.sp = input + 0x1000}; fill.x[0] = screen; fill.x[30] = g_site.replace_fill_call + 4;
    replace_fill(&fill);
    assert(replace_screen == screen && LD32(screen + 0x3738) == 2);
    assert(!g_settings.quick_drop); // Replacement is independent of ordinary Y.
    quick_replace_reset(); feature_enabled = false;
    ticks = saved_ticks; action_ticks = saved_actions; native_popups = saved_popups; fills = saved_fills;
}

static void async_menu(Cpu *menu, uint64_t actor, uint64_t ai, bool chest) {
    quick_replace_reset(); inspect_input = false; buttons = consumed = 0;
    defer_drop = true; drop_result = QUICK_DROP_REJECTED;
    chest_capacity = false; ground_result = 7; ground_actor = actor;
    ST64(actor + 0x10, input + 0x300);
    ST8(actor + 0x64, 1);
    strcpy((char *)(uintptr_t)(input + 0x300), "Weapon_Bow_001");
    ST32(actor + 0xf98, 0x40); ST32(actor + 0xf9c, 750);
    if (chest) {
        ST64(ai + 8, manager + 0x1000); ST64(ai + 0x70, input + 0x600);
        ST64(input + 0x608, input + 0x300); ST8(ai + 0xb0, 1);
        ST32(ai + 0xc8, 0x10); ST32(ai + 0xcc, 17);
        Cpu chest_cpu = *menu; chest_cpu.x[0] = ai;
        replace_chest_update(&chest_cpu);
        assert(replace_chest == ai && replace_chest_owner_live(menu));
        replace_chest_wait = false; replace_chest_previewed = true;
    } else {
        replace_handle = (uint64_t)(uintptr_t)hle_alloc(0x18);
        ST8(replace_handle, 1); ST64(replace_handle + 8, actor);
        assert(replace_index_capture(menu, &replace_ground_index, actor));
        strcpy(replace_incoming, "Weapon_Bow_001");
        replace_capture_known_ground_preview(menu);
    }
    replace_start(2); replace_screen = screen;
    ST64(screen + 0x3640, manager); ST32(screen + 0x3738, 2);
    ST32(screen + 0x3880, 1); ST32(screen + 0x387c, 0); ST32(screen + 0x3878, 0);
    ST32(manager + 0x44498, 1); ST64(screen + 0x37c8, input + 0x2000);
    ST64(input + 0x2030, input + 0x2100);
    strcpy((char *)(uintptr_t)(input + 0x2100), "Weapon_Bow_002");
    ST8(screen + 0x38e4, 0);
    ST32(manager + 0x64c24, 3); ST32(manager + 0x64c2c, 2);
    replace_card = manager + 0x65000; replace_card_open = replace_card_paused = true;
    replace_card_pause_saved = 0; ST8(replace_card + 0x103, 1);
    menu->x[0] = screen;
    buttons = 1;
    unsigned old_closes = card_closes, old_drops = drops;
    assert(quick_replace_menu(menu));
    assert(replace_outgoing_pending && replace_closing && !replace_confirmed && drops == old_drops + 1);
    assert(LD8(screen + 0x38e4) == 2 && !replace_card_open && card_closes == old_closes + 1);
    assert(!LD8(manager + 0x65103) && !replace_retry_until);
    unsigned old_opens = card_opens;
    replace_card_show(menu); assert(card_opens == old_opens && !replace_card_open);
    buttons = 2; quick_replace_menu(menu); // Closing cannot cancel native work already requested.
    assert(replace_outgoing_pending && !replace_confirmed);
}

static void test_sequential_chests(uint64_t actor, uint64_t ai) {
    Cpu menu = {.sp = input + 0x1000};
    async_menu(&menu, actor, ai, true);
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
    chest_capacity = true; drop_result = QUICK_DROP_COMPLETE;
    tick_with(0, 0);
    assert(replace_confirmed && replace_resume_chest == ai && replace_closing);
    Cpu chest = {.sp = input + 0x1000}; chest.x[0] = ai;
    chest_emits_receive = true; replace_chest_update(&chest); chest_emits_receive = false;
    assert(!replace_resume_chest && replace_category == -1 && !replace_chest);
    unsigned received = receive_calls, requested = drops;
    // No reset between the completed chest and these two fresh captures. The
    // second iteration also starts immediately after a canceled chest.
    for (unsigned next = 0; next < 2; next++) {
        buttons = 0; chest_capacity = false; ST8(ai + 0xb0, 1);
        replace_chest_update(&chest);
        assert(replace_chest_wait && !replace_closing && !replace_source_lost && !replace_confirmed);
        unsigned opened = card_opens;
        ST8(manager + 0x650fe, 2);
        tick_with(0, 0);
        assert(card_opens == opened + 1 && replace_card_open && replace_chest_previewed);
        assert(!replace_chest_wait && replace_category == 2);
        Cpu fill = {.sp = input + 0x1000}; fill.x[0] = screen; fill.x[30] = 0x7000e2853cull;
        replace_fill(&fill);
        assert(replace_screen == screen && !replace_closing);
        buttons = 2; menu.x[0] = screen;
        assert(quick_replace_menu(&menu) && replace_closing && !replace_confirmed);
        ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
        tick_with(0, 0);
        assert(replace_resume_chest == ai && replace_category == -1);
        chest_emits_receive = true; replace_chest_update(&chest); chest_emits_receive = false;
        assert(!replace_resume_chest && !replace_chest && !replace_chest_wait);
        assert(receive_calls == received && drops == requested);
    }
}

static void test_ground_retry_handoff(void) {
    // Release helpers may use the stack below their supplied SP. The fresh
    // capture's temporary link must remain in its caller's reserved frame.
    clobber_release_stack = true;
    const uint64_t old_actor = manager + 0x6c000, actor = input + 0x1a00;
    const uint64_t source = input + 0x1600, status = input + 0x1700;
    const uint64_t event_manager = manager + 0x10000, event = input + 0x3000;
    Cpu popup = {.sp = input + 0x1000}; popup.x[0] = source; popup.x[1] = status;
    Cpu item = {.sp = input + 0x1000}; item.x[0] = input + 0x200; item.x[30] = 0x7000cb0ce8ull;
    Cpu query = {.sp = input + 0x1000}; query.x[0] = input + 0x4000;
    Cpu fill = {.sp = input + 0x1000}; fill.x[0] = screen; fill.x[30] = 0x7000e2853cull;
    unsigned old_drops = drops;
    inspect_input = false; emulate_direction_latch = false; defer_drop = false;
    g_site.event_mgr = 0x7001d66008ull; ST64(g_site.event_mgr, event_manager);
    strcpy((char *)(uintptr_t)(input + 0x300), "Weapon_Bow_001");
    ST64(input + 0x208, input + 0x300); ST64(input + 0x4030, input + 0x300);
    ST32(status, 7); ST8(source, 1);
    ST64(event + 0x68, event + 0x80);
    strcpy((char *)(uintptr_t)(event + 0x80), "Demo002_0");
    for (unsigned route = 0; route < 3; route++) {
        quick_replace_reset(); buttons = 0; chest_capacity = false; ground_result = 7;
        ST64(event_manager + 0x1d2b8, 0);
        ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
        ST64(source + 8, old_actor); ST64(old_actor + 0x10, input + 0x300);
        ST8(old_actor + 0x64, 1); ST32(old_actor + 0xf98, 0);
        ground_actor = old_actor;
        replace_popup(&popup);
        // Finish the preceding receive with its real retained source still
        // alive, leaving automatic A armed until native pickup consumes it.
        ground_result = 0; replace_confirmed = true;
        replace_finish_close(&popup);
        uint64_t deadline = replace_retry_until, old_block = LD64(replace_ground_index);
        assert(deadline > presents && LD32(old_block + 0x50) == 1);
        assert(LD64(replace_handle + 8) == old_actor && quick_replace_buttons(0) == 1);
        ST64(source + 8, actor); ST64(actor + 0x10, input + 0x300);
        ST8(actor + 0x64, 1); ST32(actor + 0xf98, 0x40); ST32(actor + 0xf9c, 750);
        ground_actor = actor; ground_result = 7;
        uint64_t existing_block = LD64(actor + 0x68);
        uint32_t existing_refs = existing_block ? LD32(existing_block + 0x50) : 0;
        if (route == 0) replace_item_popup(&item);
        else if (route == 1) replace_popup(&popup);
        else {
            ST64(event_manager + 0x1d2b8, event);
            query.x[0] = input + 0x4000;
            replace_query(&query);
            assert(replace_event == event);
        }
        assert(LD32(old_block + 0x50) == 0);
        uint64_t handoff_buttons = quick_replace_buttons(0x40);
        presents = deadline + 1;
        tick_with(0, 0);
        assert(replace_ground_actor(&popup) == actor && LD8(replace_ground_index + 0xc));
        assert(replace_ground_shop == (route == 1));
        assert(handoff_buttons == 0x40);
        uint64_t new_block = LD64(replace_ground_index);
        assert(new_block != old_block && LD32(new_block + 0x50) == existing_refs + 1);
        if (route < 2) {
            assert(replace_source_valid(&popup, true));
            ST8(screen + 0x38e4, 0);
            replace_fill(&fill);
            buttons = 2;
            assert(quick_replace_menu(&fill) && replace_closing && !replace_confirmed);
            tick_with(0, 0);
            assert(replace_category == -1);
        } else {
            // Cancel discovery before its native full-pouch notification;
            // retaining the new actor never authorizes a synthetic pickup.
            ST64(event_manager + 0x1d2b8, 0);
            tick_with(0, 0);
            assert(!replace_event && replace_category == -1);
        }
        assert(!LD64(replace_handle + 8) && !LD8(replace_ground_index + 0xc));
        assert(LD32(new_block + 0x50) == existing_refs && quick_replace_buttons(0) == 0 && drops == old_drops);
    }
    quick_replace_reset(); buttons = 0; clobber_release_stack = false;
}

static uint64_t blur_expected;
static uint8_t corner_colors[4][16];
static bool tint_expected;
static void visual_pane(uint64_t pane, const char *name) {
    memset((void *)(uintptr_t)pane, 0, 0x100);
    strcpy((char *)(uintptr_t)(pane + 0xb8), name);
    ST64(pane + 0x20, pane + 0x20);
    ST64(pane + 0x28, pane + 0x20);
}
static void visual_child(uint64_t parent, uint64_t child) {
    uint64_t end = parent + 0x20, first = LD64(end);
    ST64(child + 8, first); ST64(end, child + 8);
}
static void blur_selector_original(Cpu *c) {
    assert(replace_selector_blur_pane == blur_expected);
    for (unsigned i = 0; i < 4; i++) for (unsigned v = 0; v < 4; v++) {
        uint64_t rgba = manager + 0x826e8 + i * 0x100 + v * 4;
        const uint8_t *original = corner_colors[i] + v * 4;
        bool tinted = tint_expected && i != 3; // Fourth is not a native Picture.
        assert(LD8(rgba) == (tinted ? 255 : original[0]));
        assert(LD8(rgba + 1) == (tinted ? 218 : original[1]));
        assert(LD8(rgba + 2) == (tinted ? 68 : original[2]));
        assert(LD8(rgba + 3) == original[3]);
    }
    assert(LD32(manager + 0x835e8) == 0x43211234); // Unrelated same-name Picture.
    assert(LD32(manager + 0x836e8) == 0xff0048ff); // Equipped blue untouched.
    unsigned before = pane_calls;
    Cpu pane = *c;
    pane.x[0] = manager + 0x82100; replace_pane_draw(&pane);
    assert(pane_calls == before + (blur_expected ? 0 : 1));
    // Identical name in another layout must not be excluded.
    pane.x[0] = manager + 0x82200; replace_pane_draw(&pane);
    // Original-world capture must continue drawing.
    pane.x[0] = manager + 0x82300; replace_pane_draw(&pane);
    assert(pane_calls == before + (blur_expected ? 2 : 3));
    c->x[7] = 0x5eed; // Preserve effects from the actual original draw.
}
static uint64_t card_style_test_pane(uint64_t parent, const char *name, uint64_t *next) {
    uint64_t pane = *next; *next += 0x100;
    visual_pane(pane, name); ST8(pane + 0x58, 0x91);
    visual_child(parent, pane);
    return pane;
}

static void visual_set_x(uint64_t pane, float x) {
    memcpy((void *)(uintptr_t)(pane + 0x30), &x, 4);
}
static float visual_get_x(uint64_t pane) {
    float x; memcpy(&x, (void *)(uintptr_t)(pane + 0x30), 4); return x;
}

static const uint64_t *initial_hidden, *initial_colors, *initial_modifiers;
static unsigned initial_hidden_count, initial_draws;
static bool initial_styled;
static void initial_card_original(Cpu *c) {
    bool styled = initial_styled && c->x[0] == replace_card;
    for (unsigned i = 0; i < initial_hidden_count; i++)
        assert(!!(LD8(initial_hidden[i] + 0x58) & 1) == !styled);
    for (unsigned i = 0; i < 2; i++)
        assert(visual_get_x(initial_colors[i]) == (styled ? -54 - 6 * (int)i : 7 * (int)i));
    for (unsigned i = 0; i < 2; i++)
        assert(visual_get_x(initial_modifiers[i]) == (styled ? 91 - 12 * (int)i : 145 + (int)i));
    initial_draws++;
    c->x[7] = 0xcafe;
}

static void test_replacement_card_style(void) {
    bool saved_feature = feature_enabled;
    quick_replace_reset(); feature_enabled = true;
    uint64_t root = manager + 0x84000, next = root + 0x100;
    visual_pane(root, "RootPane");
    uint64_t hidden[12], kept[12], colors[2], modifiers[2]; unsigned n = 0, k = 0;
    uint64_t capture = card_style_test_pane(root, "N_Capture_00", &next);
    uint64_t ordinary = card_style_test_pane(root, "N_InOut_00", &next);
    uint64_t glow = card_style_test_pane(ordinary, "N_Sp3_00", &next);
    ordinary = card_style_test_pane(glow, "N_Sp_00", &next);
    hidden[n++] = card_style_test_pane(ordinary, "N_State_00", &next);
    hidden[n++] = card_style_test_pane(capture, "N_State_01", &next);
    uint64_t body = card_style_test_pane(ordinary, "W_Base_00", &next);
    uint64_t icons[] = {card_style_test_pane(ordinary, "N_Icon_00", &next),
                        card_style_test_pane(capture, "N_Icon_01", &next)};
    uint64_t foreground_icon = card_style_test_pane(icons[0], "P_Icon_00", &next);
    const char *old_stat[] = {"T_OldNum_00", "W_Base_00", "W_BaseLine_00",
                              "P_NumberIconOld_00", "P_ArrowS_01"};
    for (unsigned i = 0; i < 2; i++) {
        uint64_t gear = card_style_test_pane(icons[i], i ? "Pa_Gear_01" : "Pa_Gear_00", &next);
        uint64_t parameter = card_style_test_pane(gear, "Pa_Param_00", &next);
        uint64_t all = card_style_test_pane(parameter, "N_All_00", &next);
        colors[i] = card_style_test_pane(all, "N_Color_00", &next);
        visual_set_x(colors[i], 7 * i);
        for (unsigned j = 0; j < 5; j++)
            hidden[n++] = card_style_test_pane(j == 4 ? colors[i] : all, old_stat[j], &next);
        visual_set_x(hidden[2 + i * 5], 39 + 2 * i);
        kept[k++] = card_style_test_pane(colors[i], "T_Num_00", &next);
        visual_set_x(kept[k - 1], 93 + 8 * i);
        kept[k++] = card_style_test_pane(colors[i], "W_Base_01", &next);
        kept[k++] = card_style_test_pane(colors[i], "W_BaseLine_01", &next);
        kept[k++] = card_style_test_pane(all, "P_Icon_00", &next);
        visual_set_x(kept[k - 1], 654 + i);
        kept[k++] = card_style_test_pane(gear, "Pa_Param_01", &next);
        modifiers[i] = kept[k - 1];
        visual_set_x(kept[k - 1], 145 + i);
        kept[k++] = card_style_test_pane(gear, "T_OldNum_00", &next); // Outside stat part.
    }
    replace_category = 2; replace_screen = screen; replace_card_open = true;
    for (unsigned category = 0; category < 3; category++) for (unsigned chest = 0; chest < 2; chest++) {
        replace_category = category;
        replace_chest = chest ? input + 0x400 : 0;
        ReplaceCardStyle style = replace_card_style_begin(root);
        assert(style.count == 12);
        assert(LD8(body + 0x58) == 0x91 && LD8(foreground_icon + 0x58) == 0x91);
        assert(style.shift_count == 4);
        for (unsigned i = 0; i < 2; i++) {
            assert(visual_get_x(colors[i]) == -54 - 6 * (int)i);
            assert(visual_get_x(colors[i]) + visual_get_x(kept[i * 6]) == 39 + 2 * i);
            assert(visual_get_x(kept[i * 6 + 3]) == 654 + i); // Category icon unchanged.
            assert(visual_get_x(modifiers[i]) == 91 - 12 * (int)i); // Same displacement.
            assert(visual_get_x(modifiers[i]) - visual_get_x(colors[i]) ==
                   (145 + (int)i) - 7 * (int)i); // Native spacing preserved.
        }
        for (unsigned i = 0; i < n; i++) assert(LD8(hidden[i] + 0x58) == 0x90);
        for (unsigned i = 0; i < k; i++) assert(LD8(kept[i] + 0x58) == 0x91);
        replace_card_style_end(&style);
        for (unsigned i = 0; i < 2; i++) assert(visual_get_x(colors[i]) == 7 * i);
        for (unsigned i = 0; i < 2; i++) assert(visual_get_x(modifiers[i]) == 145 + i);
        for (unsigned i = 0; i < n; i++) assert(LD8(hidden[i] + 0x58) == 0x91);
    }
    feature_enabled = false; assert(!replace_card_style_begin(root).count);
    feature_enabled = true; replace_card_open = false;
    assert(!replace_card_style_begin(root).count);
    replace_card_open = true; replace_closing = true;
    assert(!replace_card_style_begin(root).count);
    replace_closing = false; replace_category = -1; // Ordinary discovery card.
    assert(!replace_card_style_begin(root).count);
    for (unsigned i = 0; i < n; i++) assert(LD8(hidden[i] + 0x58) == 0x91);
    // The first owned chest card updates internally, without drawing into
    // the world image that the selector will capture.
    GuestFn saved_draw = replace_ui_draw_original;
    unsigned saved_calcs = toast_calcs;
    uint64_t saved_root = LD64(manager + 0x54018), saved_context = LD64(manager + 0x530f8);
    uint64_t card = manager + 0x65000;
    uint64_t saved_layout = LD64(card + 0x30), saved_info = LD64(card + 0x68);
    ST64(card + 0x30, manager + 0x54000); ST64(card + 0x68, manager + 0x53000);
    ST64(manager + 0x54018, root); ST64(manager + 0x530f8, 0x1234);
    replace_screen = 0; replace_card = card; replace_chest_wait = true;
    replace_chest = input + 0x400; replace_card_open = true;
    initial_hidden = hidden; initial_colors = colors; initial_modifiers = modifiers; initial_hidden_count = n;
    replace_ui_draw_original = initial_card_original;
    Cpu draw = {.sp = input + 0x1000}; draw.x[0] = card; draw.x[1] = manager + 0x55000;
    Cpu expected = draw;
    initial_styled = false;
    assert(installed_ui_draw && installed_card_draw);
    unsigned direct_draws = initial_draws;
    Cpu direct = draw;
    item_card_thunk_original(&direct);
    assert(initial_draws == direct_draws + 1 && direct.x[7] == 0xcafe);
    // Base override existed above but was bypassed. Catch the derived dispatch.
    unsigned old_draws = initial_draws;
    for (unsigned phase = 0; phase < 3; phase++) {
        ST8(card + 0xfe, phase);
        installed_card_draw(&draw);
        assert(LD8(card + 0xfe) == phase);
    }
    assert(initial_draws == old_draws && !memcmp(&draw, &expected, sizeof draw));
    assert(toast_calcs == saved_calcs && LD64(manager + 0x530f8) == 0x1234);
    for (unsigned i = 0; i < n; i++) assert(LD8(hidden[i] + 0x58) == 0x91);
    for (unsigned i = 0; i < 2; i++) assert(visual_get_x(colors[i]) == 7 * i);
    initial_styled = false;
    draw.x[0] = card + 0x100; installed_card_draw(&draw); // Unrelated native card.
    draw.x[0] = card; feature_enabled = false;
    Cpu disabled_expected = draw; item_card_thunk_original(&disabled_expected);
    installed_card_draw(&draw);
    assert(!memcmp(&draw, &disabled_expected, sizeof draw));
    feature_enabled = true; replace_chest_wait = false; replace_ui_draw(&draw); // Discovery.
    replace_chest_wait = true; replace_card_open = false; replace_ui_draw(&draw);
    replace_card_open = true; replace_chest = 0; replace_ui_draw(&draw);
    assert(initial_draws == old_draws + 6 && toast_calcs == saved_calcs);
    // Once the selector exists, its callback draws exactly one styled card.
    replace_chest = input + 0x400; replace_chest_wait = false;
    replace_category = 0; replace_screen = screen; initial_styled = true;
    draw.x[0] = card; installed_card_draw(&draw);
    assert(initial_draws == old_draws + 6); // Native owned-card draw still omitted.
    draw.x[0] = screen; replace_ui_draw(&draw);
    assert(initial_draws == old_draws + 8 && toast_calcs == saved_calcs + 2);
    assert(LD64(manager + 0x530f8) == 0x1234);
    for (unsigned i = 0; i < n; i++) assert(LD8(hidden[i] + 0x58) == 0x91);
    replace_ui_draw_original = saved_draw; toast_calcs = saved_calcs;
    ST64(manager + 0x54018, saved_root); ST64(manager + 0x530f8, saved_context);
    ST64(card + 0x30, saved_layout); ST64(card + 0x68, saved_info);
    quick_replace_reset(); feature_enabled = saved_feature;
}

static void test_selector_blur_scope(void) {
    GuestFn saved_draw = replace_ui_draw_original;
    unsigned saved_calls = pane_calls, saved_calcs = toast_calcs;
    bool saved_feature = feature_enabled; feature_enabled = true;
    uint64_t saved_layout = LD64(screen + 0x30), saved_info = LD64(screen + 0x68);
    uint64_t saved_root = LD64(manager + 0x54018);
    quick_replace_reset();
    uint64_t root = manager + 0x82000, filtered = manager + 0x82100;
    ST64(screen + 0x30, manager + 0x54000);
    ST64(screen + 0x68, manager + 0x53000);
    ST64(manager + 0x54018, root);
    visual_pane(root, "RootPane");
    visual_pane(filtered, "P_Capture_01"); visual_child(root, filtered);
    uint64_t focus = manager + 0x82400;
    visual_pane(focus, "N_FocusL_00"); visual_child(root, focus);
    const char *corners[] = {"N_LT_00", "N_RT_00", "N_LD_00", "N_RD_00"};
    const char *arrows[] = {"P_ArrowS_00", "P_ArrowS_01", "P_ArrowS_02", "P_ArrowS_03"};
    for (unsigned i = 0; i < 4; i++) {
        uint64_t corner = manager + 0x82a00 + i * 0x100;
        uint64_t picture = manager + 0x82600 + i * 0x100;
        visual_pane(corner, corners[i]); visual_child(focus, corner);
        visual_pane(picture, arrows[i]); visual_child(corner, picture);
        ST64(picture, i == 3 ? 0x7001b7ccd0ull : i == 1 ? 0x7001d0e408ull : 0x7001b7cce0ull);
        for (unsigned k = 0; k < 16; k++) corner_colors[i][k] = 30 + i * 16 + k;
        memcpy((void *)(uintptr_t)(picture + 0xe8), corner_colors[i], 16);
    }
    visual_pane(manager + 0x83500, "P_ArrowS_00");
    ST64(manager + 0x83500, 0x7001b7cce0ull); ST32(manager + 0x835e8, 0x43211234);
    visual_pane(manager + 0x83600, "W_Equip_00"); ST32(manager + 0x836e8, 0xff0048ff);
    strcpy((char *)(uintptr_t)(manager + 0x822b8), "P_Capture_01");
    strcpy((char *)(uintptr_t)(manager + 0x823b8), "P_Capture_00");
    replace_ui_draw_original = blur_selector_original;
    replace_category = 2; replace_screen = screen; replace_card_open = true;
    Cpu c = {0}; c.x[0] = screen; c.sp = input + 0x1000;
    blur_expected = filtered; tint_expected = true;
    Cpu before = c; test_selector_draw(&c);
    before.x[7] = 0x5eed;
    assert(!memcmp(&before, &c, sizeof c));
    assert(tinted_calcs == 1); // Tint reaches native render-data calculation.
    for (unsigned i = 0; i < 4; i++)
        assert(!memcmp((void *)(uintptr_t)(manager + 0x826e8 + i * 0x100), corner_colors[i], 16));
    assert(!replace_selector_blur_pane);
    // Restores the enclosing scope even when called recursively.
    replace_selector_blur_pane = manager + 0x82400;
    test_selector_draw(&c);
    assert(replace_selector_blur_pane == manager + 0x82400);
    replace_selector_blur_pane = 0;
    blur_expected = 0; tint_expected = false;
    feature_enabled = false; test_selector_draw(&c); feature_enabled = true;
    c.x[0] = screen + 0x100; test_selector_draw(&c);
    c.x[0] = screen; replace_card_open = false; test_selector_draw(&c);
    replace_card_open = true; replace_closing = true; test_selector_draw(&c);
    replace_closing = false; ST64(screen + 0x30, 0); test_selector_draw(&c);
    assert(!replace_selector_blur_pane);
    replace_selector_blur_pane = filtered;
    quick_replace_reset(); assert(!replace_selector_blur_pane);
    ST64(screen + 0x30, saved_layout); ST64(screen + 0x68, saved_info);
    ST64(manager + 0x54018, saved_root);
    replace_ui_draw_original = saved_draw; pane_calls = saved_calls; toast_calcs = saved_calcs;
    feature_enabled = saved_feature;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    void *mem = mmap((void *)(uintptr_t)manager, 0x90000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mem == (void *)(uintptr_t)manager);
    void *globals = mmap((void *)(uintptr_t)0x7001d66000ull, 0x2000,
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
                         MAP_FIXED_NOREPLACE, -1, 0);
    assert(globals == (void *)(uintptr_t)0x7001d66000ull);
    void *clock_globals = mmap((void *)(uintptr_t)0x7001e37000ull, 0x1000,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(clock_globals == (void *)(uintptr_t)0x7001e37000ull);
    ST64(0x7001d66260ull, manager);
    ST8(manager + 0x1064, 1); // Live native chest owner.
    feature_enabled = false;
    g_site.actor_index_assign = 0x7001517e60ull;
    g_site.actor_index_release = 0x7001517f8cull;
    g_site.actor_index_resolve = 0x7001517abcull;
    g_site.actor_link_assign = 0x700112649cull;
    g_site.actor_link_copy = 0x7001070ce0ull;
    g_site.actor_link_release = 0x7001126464ull;
    g_site.event_mgr = 0x7001e3a638ull;
    g_site.pouch_can_receive = 0x7000be21e0ull;
    g_site.pouch_item_type = 0x7000d91414ull;
    g_site.pouch_lock = 0x7000eaff08ull;
    g_site.pouch_page_count = 0x7000d970a8ull;
    g_site.pouch_page_item = 0x7000d992acull;
    g_site.pouch_unlock = 0x7000eaff18ull;
    g_site.replace_ai_child_matches = 0x7001537800ull;
    g_site.replace_anim_advance = 0x7000f6c9b4ull;
    g_site.replace_card_close = 0x70013e99c4ull;
    g_site.replace_card_manager_slot = 0x7001d67960ull;
    g_site.replace_card_prepare = 0x70013e942cull;
    g_site.replace_chest_closed_wait_name = 0x700164f739ull;
    g_site.replace_component_update = 0x70013eb788ull;
    g_site.replace_draw_info_reset = 0x7000f60944ull;
    g_site.replace_fill_call = 0x7000e28538ull;
    g_site.replace_game_picture_vtable = 0x7001d0e408ull;
    g_site.replace_interaction_manager_slot = 0x7001d68838ull;
    g_site.replace_interaction_target = 0x70010d544cull;
    g_site.replace_item_popup = 0x7000e7a7f0ull;
    g_site.replace_layout_calculate = 0x7000025c64ull;
    g_site.replace_pickup_call_a = 0x7000cb0ce4ull;
    g_site.replace_pickup_call_b = 0x7000ca4440ull;
    g_site.replace_pickup_call_c = 0x7000cbc270ull;
    g_site.replace_pickup_status = 0x7000e780f0ull;
    g_site.replace_picture_vtable_base = 0x7001b7ccd0ull;
    g_site.replace_popup = 0x7000e78a30ull;
    g_site.replace_pouch_manager_slot = 0x7001d67af0ull;
    g_site.replace_safe_string_vtable_slot = 0x7001d66260ull;
    g_site.replace_string_assign = 0x7000573698ull;
    g_site.replace_toast_start = 0x7000da9a0cull;
    g_site.replace_toast_vtable = 0x7001cde098ull;
    g_site.replace_weapon_modifier = 0x70007b4ee0ull;
    g_site.screen_mgr = 0x7001e374b8ull;
    g_site.event_mgr = 0x7001d66008ull;
    installed_tick = replace_tick; installed_popup = replace_popup; installed_fill = replace_fill;
    installed_ui_draw = installed_card_draw = replace_ui_draw;
    replace_ui_draw_original = popup_original;
    quick_replace_init();
    assert(quick_replace_init());
    test_toast_scope();
    test_boot_disabled_live_enable();
    test_selector_blur_scope();
    test_replacement_card_style();
    ST64(manager + 0x649f0, input);
    tick_with(0x42, 0x24);
    assert(replace_category == -1 && !replace_screen);
    feature_enabled = true;
    inspect_input = true;
    const uint32_t held = 0xf0025, edge = 0x40;
    for (int category = 0; category < 3; category++) {
        quick_replace_reset(); buttons = 0; replace_start(category);
        uint32_t direction = 1u << (category == 1 ? 18 : 19);
        expected_held = (held & ~0xf0000u) | direction;
        expected_edge = edge | direction;
        tick_with(held, edge);
        // Unrelated fill callers must retain their own category and screen.
        Cpu fill = {0}; fill.x[0] = screen; fill.x[1] = 4;
        fill.x[30] = 0x7000e28540ull;
        ST32(screen + 0x3738, 4); ST32(screen + 0x373c, 4);
        replace_fill(&fill);
        assert(fill.x[1] == 4 && replace_screen == 0);
        assert(LD32(screen + 0x3738) == 4 && LD32(screen + 0x373c) == 4);
        fill.x[30] = 0x7000e2853cull;
        replace_fill(&fill);
        assert(replace_screen == screen && fill.x[1] == (uint64_t)category);
        assert(LD32(screen + 0x3738) == (unsigned)category);
        assert(LD32(screen + 0x373c) == (unsigned)category);
        // Once open, holding must not fabricate another opening edge.
        expected_edge = edge;
        tick_with(held, edge);
    }
    assert(fills == 6);
    // An initially unavailable native menu must receive another held edge.
    quick_replace_reset(); buttons = 0; replace_start(0);
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
    ST32(manager + 0x64c58, 0);
    emulate_direction_latch = true; open_attempts = 0;
    expected_held = (held & ~0xf0000u) | (1u << 19);
    expected_edge = edge | (1u << 19);
    tick_with(held, edge);
    assert(open_attempts == 1 && LD32(manager + 0x64c24) == 0);
    expected_held = held & ~0xf0000u; expected_edge = edge;
    tick_with(held, edge);
    assert(open_attempts == 1 && !LD32(manager + 0x64c58));
    expected_held |= 1u << 19; expected_edge |= 1u << 19;
    tick_with(held, edge);
    assert(open_attempts == 2 && LD32(manager + 0x64c24) == 3);
    // Even tick must keep holding during an accepted opening transition.
    tick_with(held, edge);
    assert(open_attempts == 2 && LD32(manager + 0x64c58) == (1u << 19));
    emulate_direction_latch = false;
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
    // Both A and B close without committing the highlighted equipment slot.
    for (uint64_t close_button = 1; close_button <= 2; close_button++) {
        quick_replace_reset(); buttons = 0; consumed = 0; replace_start(0);
        Cpu c = {0}; c.x[0] = screen;
        c.x[30] = 0x7000e2853cull;
        replace_fill(&c);
        ST32(screen + 0x3878, 2); ST32(screen + 0x387c, 5);
        ST8(screen + 0x38e4, 0);
        buttons = close_button | (1ull << 14);
        assert(quick_replace_menu(&c));
        assert(replace_closing && LD8(screen + 0x38e4) == 2);
        assert(LD32(screen + 0x387c) == 2);
        assert(consumed == buttons);
        expected_held = held & ~0xf0000u; expected_edge = edge;
        ST32(manager + 0x64c24, 3); ST32(manager + 0x64c2c, 2);
        tick_with(held, edge);
        assert(replace_screen == screen); // Wait for native close completion.
        ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
        tick_with(held, edge);
        assert(replace_category == -1 && replace_screen == 0);
        assert(!quick_replace_menu(&c));
    }
    // Reset must invalidate guest pointers and stale button edges.
    replace_start(1); replace_screen = screen; replace_buttons = 0xff;
    replace_closing = true;
    quick_replace_reset();
    assert(replace_category == -1 && !replace_screen && !replace_buttons);
    assert(!replace_frames && !replace_closing);
    uint64_t string = input + 0x200, name = input + 0x300;
    ST64(string + 8, name);
    strcpy((char *)(uintptr_t)name, "Weapon_Sword_001");
    // Full chest suspends before native pending/context consumption.
    quick_replace_reset(); buttons = 0; inspect_input = false;
    const uint64_t ai = input + 0x400, chest_string = input + 0x600;
    ST64(ai + 0x70, chest_string); ST64(chest_string + 8, name);
    ST64(ai + 8, manager + 0x1000); ST8(ai + 0xb0, 1);
    strcpy((char *)(uintptr_t)name, "Weapon_Bow_001");
    Cpu chest_cpu = {0}; chest_cpu.x[0] = ai; chest_cpu.sp = input + 0x1000;
    Cpu before_chest = chest_cpu;
    unsigned char context[0xd8]; memcpy(context, (void *)(uintptr_t)ai, sizeof context);
    replace_chest_update(&chest_cpu);
    assert(!memcmp(&chest_cpu, &before_chest, sizeof chest_cpu));
    assert(!memcmp(context, (void *)(uintptr_t)ai, sizeof context));
    assert(chest_calls == 0 && predicate_calls == 1);
    assert(replace_chest == ai && replace_owner == manager + 0x1000);
    assert(replace_category == -1 && replace_chest_wait && !strcmp(replace_incoming, "Weapon_Bow_001"));
    replace_chest_update(&chest_cpu);
    assert(chest_calls == 0 && predicate_calls == 1 && LD8(ai + 0xb0));
    replace_chest_wait = false; replace_chest_previewed = true; replace_start(2);
    Cpu menu_cpu = {0}; menu_cpu.x[0] = screen; menu_cpu.x[30] = 0x7000e2853cull;
    replace_fill(&menu_cpu);
    buttons = 2; assert(quick_replace_menu(&menu_cpu));
    tick_with(0, 0);
    assert(replace_resume_chest == ai && !replace_chest && replace_category == -1);
    replace_chest_update(&chest_cpu);
    assert(chest_calls == 1 && !replace_resume_chest && !LD8(ai + 0xb0));
    replace_chest_update(&chest_cpu);
    assert(chest_calls == 2 && replace_category == -1 && predicate_calls == 1);
    // A successful capacity query must retain the native handler's behavior.
    quick_replace_reset(); ST8(ai + 0xb0, 1); chest_capacity = true;
    replace_chest_update(&chest_cpu);
    assert(chest_calls == 3 && replace_category == -1 && !LD8(ai + 0xb0));
    // Non-equipment and an unfinished native child never launch replacement.
    quick_replace_reset(); ST8(ai + 0xb0, 1); chest_capacity = false;
    strcpy((char *)(uintptr_t)name, "Item_Fruit_A");
    unsigned old_predicates = predicate_calls;
    replace_chest_update(&chest_cpu);
    assert(chest_calls == 4 && replace_category == -1 && predicate_calls == old_predicates);
    strcpy((char *)(uintptr_t)name, "Weapon_Bow_001");
    ST8(ai + 0xb0, 1); chest_ready = false;
    replace_chest_update(&chest_cpu);
    assert(chest_calls == 5 && replace_category == -1);
    // Recycled identity or changed modifier context releases the old request.
    chest_ready = true;
    for (int changed = 0; changed < 5; changed++) {
        quick_replace_reset(); buttons = 0;
        ST8(ai + 0xb0, 1); ST64(ai + 8, manager + 0x1000);
        ST32(ai + 0xc8, 0x10); ST32(ai + 0xcc, 17);
        strcpy((char *)(uintptr_t)name, "Weapon_Bow_001");
        replace_chest_update(&chest_cpu);
        assert(replace_chest == ai);
        unsigned old_calls = chest_calls;
        if (changed == 0) ST64(ai + 8, manager + 0x2000);
        else if (changed == 1) strcpy((char *)(uintptr_t)name, "Weapon_Bow_002");
        else if (changed == 2) ST8(ai + 0xb0, 0);
        else if (changed == 3) ST32(ai + 0xc8, 1);
        else ST32(ai + 0xcc, 18);
        replace_chest_update(&chest_cpu);
        assert(!replace_source_valid(&chest_cpu, true));
        assert(chest_calls == old_calls + 1 && !replace_chest && replace_closing);
    }
    ST32(ai + 0xc8, 0); ST32(ai + 0xcc, 0);
    // Reset also forgets pending/resume actor identities and incoming names.
    replace_chest = replace_resume_chest = ai; replace_owner = manager;
    quick_replace_reset();
    assert(!replace_chest && !replace_resume_chest && !replace_owner && !replace_incoming[0]);
    // Retain the ground actor handle and reject A when the target changed.
    void *target_globals = mmap((void *)(uintptr_t)0x7001d68000ull, 0x1000,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(target_globals == (void *)(uintptr_t)0x7001d68000ull);
    ST64(0x7001d68838ull, input + 0x1900); ST64(input + 0x1900, manager);
    quick_replace_reset(); buttons = 0;
    uint64_t source = input + 0x1600, status = input + 0x1700, actor = input + 0x1a00;
    ST64(source + 8, actor); ST64(actor + 0x10, name);
    ST8(source, 1); ST8(actor + 0x64, 1);
    strcpy((char *)(uintptr_t)name, "Weapon_Bow_001"); ST32(status, 7);
    // Alternate native full-pickup notification only owns proven A callers; accepted notifications belong to the overlay.
    Cpu item_popup = {0}; item_popup.x[0] = string; item_popup.sp = input + 0x1000;
    item_popup.x[30] = 0x7000e78aa8ull; ground_actor = actor; chest_capacity = false;
    replace_item_popup(&item_popup);
    assert(item_popup_calls == 1 && replace_category == -1 && !replace_handle);
    item_popup.x[30] = 0x7000cb0ce8ull;
    // A stale target and a different current actor name must pass through.
    ground_actor = 0; replace_item_popup(&item_popup);
    assert(item_popup_calls == 2 && replace_category == -1);
    ground_actor = actor + 0x100;
    ST64(ground_actor + 0x10, input + 0x1c00);
    strcpy((char *)(uintptr_t)(input + 0x1c00), "Weapon_Bow_003");
    replace_item_popup(&item_popup);
    assert(item_popup_calls == 3 && replace_category == -1);
    ground_actor = actor; chest_capacity = true;
    replace_item_popup(&item_popup);
    assert(item_popup_calls == 4 && replace_category == -1);
    chest_capacity = false;
    ST32(actor + 0xf98, 0x40); ST32(actor + 0xf9c, 750);
    // A new full-pouch request can arrive before the preceding pickup retry
    // expires. That retry must not release the new selector's shared links.
    replace_retry_until = presents + 3;
    Cpu before_item_popup = item_popup;
    replace_item_popup(&item_popup);
    assert(item_popup_calls == 5 && replace_category == 2 && LD64(replace_handle + 8) == actor);
    assert(!replace_retry_until);
    assert(!memcmp(&item_popup, &before_item_popup, sizeof item_popup));
    assert(!strcmp(replace_incoming, "Weapon_Bow_001"));
    assert(replace_ground_preview && replace_ground_flags == 0x40 && replace_ground_modifier == 750);
    replace_call(&item_popup, 0x7001126464ull, replace_handle, 0, 0);
    quick_replace_reset();
    const uint64_t other_pickup_callers[] = {0x7000ca4444ull, 0x7000cbc274ull};
    for (unsigned i = 0; i < 2; i++) {
        item_popup.x[30] = other_pickup_callers[i];
        replace_item_popup(&item_popup);
        assert(item_popup_calls == 6 + i && replace_category == 2);
        assert(LD64(replace_handle + 8) == actor);
        replace_call(&item_popup, 0x7001126464ull, replace_handle, 0, 0);
        quick_replace_reset();
    }
    Cpu popup = {0}; popup.x[0] = source; popup.x[1] = status; popup.sp = input + 0x1000;
    replace_retry_until = presents + 3;
    replace_popup(&popup);
    assert(replace_handle && LD64(replace_handle + 8) == actor && replace_category == 2);
    assert(!replace_retry_until);
    assert(replace_ground_preview && replace_ground_flags == 0x40 && replace_ground_modifier == 750);
    menu_cpu.sp = input + 0x1000; replace_fill(&menu_cpu);
    ST8(screen + 0x38e4, 0); ST32(screen + 0x387c, 1);
    ground_actor = actor + 0x100; buttons = 1;
    assert(quick_replace_menu(&menu_cpu));
    assert(!replace_closing && !replace_confirmed && !drops);
    assert(LD8(screen + 0x38e4) == 0 && LD32(screen + 0x387c) == 1);
    // A dropped weapon can fail the shop's price check despite being a valid
    // ground pickup. Capacity still changes only after the outgoing discard.
    ground_actor = actor; ground_result = 2;
    assert(replace_ground_shop && !replace_source_valid(&menu_cpu, true));
    replace_ground_release(&menu_cpu);
    quick_replace_reset();
    item_popup.x[30] = 0x7000cb0ce8ull;
    replace_item_popup(&item_popup);
    assert(!replace_ground_shop && replace_category == 2);
    replace_fill(&menu_cpu);
    ST8(screen + 0x38e4, 0); ST32(screen + 0x387c, 1);
    Cpu before_capacity = menu_cpu;
    assert(replace_source_valid(&menu_cpu, true));
    assert(!replace_source_valid(&menu_cpu, false));
    assert(!memcmp(&menu_cpu, &before_capacity, sizeof menu_cpu));
    ground_actor = 0;
    assert(!replace_source_valid(&menu_cpu, true));
    ground_actor = actor; ground_result = 0;
    assert(!replace_source_valid(&menu_cpu, true));
    assert(replace_source_valid(&menu_cpu, false));
    ground_result = 2;
    // Restoring the exact source permits ordinary native confirmation.
    buttons = 0; quick_replace_menu(&menu_cpu); ground_actor = actor; buttons = 1;
    {
        ST64(screen + 0x3640, manager); ST32(screen + 0x3880, 2);
        ST32(manager + 0x44498, 1); ST64(screen + 0x37c8 + 8, input + 0x2000);
        ST64(input + 0x2000 + 0x30, input + 0x2100);
        strcpy((char *)(uintptr_t)(input + 0x2100), "Weapon_Bow_002");
        assert(quick_replace_menu(&menu_cpu) && replace_closing && replace_confirmed && drops == 1);
        tick_with(0, 0);
        assert(replace_retry_until == presents + 3 && LD64(replace_handle + 8) == actor);
        assert(quick_replace_buttons(0x40) == 0x41);
        presents += 3;
        assert(quick_replace_buttons(0x40) == 0x40);
        unsigned old_releases = releases; tick_with(0, 0);
        assert(!replace_retry_until && !LD64(replace_handle + 8) && releases == old_releases + 1);
        // Capture another handle to verify cancel release below.
        buttons = 0; replace_popup(&popup); replace_fill(&menu_cpu);
    }
    buttons = 2; assert(quick_replace_menu(&menu_cpu));
    unsigned old_releases = releases; tick_with(0, 0);
    assert(!replace_retry_until && !LD64(replace_handle + 8) && releases == old_releases + 1);
    quick_replace_reset();
    assert(!replace_handle && !replace_retry_until && !replace_confirmed);
    // First discovery: preserve description and hand matching notification to overlay,
    // wait for native event cleanup, then show notification and selector together.
    quick_replace_reset(); inspect_input = false;
    ground_actor = actor; ground_result = 7; chest_capacity = false; buttons = 0;
    uint64_t event_manager = manager + 0x10000, event = input + 0x3000;
    uint64_t query = input + 0x4000, action = input + 0x5000, message = input + 0x6000;
    g_site.event_mgr = 0x7001d66008ull; ST64(g_site.event_mgr, event_manager);
    ST64(event_manager + 0x1d2b8, event); ST64(event + 0x68, event + 0x80);
    strcpy((char *)(uintptr_t)(event + 0x80), "Demo002_0");
    ST64(query + 0x30, name); ST64(action + 0x28, message);
    Cpu query_cpu = {0}; query_cpu.x[0] = query; query_cpu.sp = input + 0x1000;
    replace_query(&query_cpu);
    assert(query_cpu.x[0] == 0 && replace_event == event && replace_category == -1);
    Cpu full_info_cpu = {0}; full_info_cpu.x[0] = action;
    strcpy((char *)(uintptr_t)message, "Weapon_Bow_003");
    replace_full_info(&full_info_cpu);
    assert(full_info_calls == 1 && !replace_event_ready);
    strcpy((char *)(uintptr_t)message, "Weapon_Bow_001");
    replace_full_info(&full_info_cpu);
    assert(full_info_calls == 2 && replace_event_ready);
    unsigned before_notification = item_popup_calls;
    tick_with(0, 0);
    assert(replace_category == -1 && item_popup_calls == before_notification);
    ST64(event_manager + 0x1d2b8, 0);
    tick_with(0, 0);
    assert(!replace_event && replace_category == 2 && item_popup_calls == before_notification);
    replace_call(&query_cpu, 0x7001126464ull, replace_handle, 0, 0);
    quick_replace_reset();
    // A false query without a matching full notification never starts a selector.
    ST64(event_manager + 0x1d2b8, event); query_cpu.x[0] = query;
    replace_query(&query_cpu); assert(replace_event == event);
    ST64(event_manager + 0x1d2b8, 0); tick_with(0, 0);
    assert(!replace_event && replace_category == -1 && !LD64(replace_handle + 8));
    // A target change while the event closes cancels the retained request.
    ST64(event_manager + 0x1d2b8, event); query_cpu.x[0] = query;
    replace_query(&query_cpu); full_info_cpu.x[0] = action; replace_full_info(&full_info_cpu);
    ground_actor = actor + 0x100; ST64(event_manager + 0x1d2b8, 0);
    for (unsigned i = 0; i < 181; i++) tick_with(0, 0);
    assert(!replace_event && replace_category == -1 && !LD64(replace_handle + 8));
    // Native toast draw: only its Pane is rendered after the owned selector.
    // DrawInfo and the caller CPU must survive all native nested calls unchanged.
    quick_replace_reset();
    g_site.screen_mgr = 0x7001d66010ull; ST64(g_site.screen_mgr, manager + 0x20000);
    ST32(manager + 0x20028, 0x26); ST64(manager + 0x20030, manager + 0x21000);
    ST64(manager + 0x21128, manager + 0x30000);
    ST64(manager + 0x30068, manager + 0x53000); ST64(manager + 0x30030, manager + 0x54000);
    replace_toast = manager + 0x40000; ST64(replace_toast + 0x20, manager + 0x50000);
    ST64(manager + 0x50018, manager + 0x51000); ST64(manager + 0x51000, manager + 0x52000);
    ST64(manager + 0x520a8, 0x7000025cd0ull);
    ST64(manager + 0x55020, manager + 0x56000);
    uint64_t info = manager + 0x53000;
    ST64(info + 0xf8, 0x1234); ST64(info + 0xc0, 0x5678);
    ST16(info + 0xeb, 0xaabb); ST8(info + 0xed, 0xcc); ST8(info + 0xef, 0xdd);
    replace_category = 0; replace_screen = screen;
    Cpu draw_cpu = {0}; draw_cpu.x[0] = screen; draw_cpu.x[1] = manager + 0x55000;
    draw_cpu.sp = input + 0x1000; Cpu expected_draw_cpu = draw_cpu;
    replace_ui_draw(&draw_cpu);
    assert(toast_draws == 1 && toast_calcs == 1 && !memcmp(&draw_cpu, &expected_draw_cpu, sizeof draw_cpu));
    assert(LD64(info + 0xf8) == 0x1234 && LD64(info + 0xc0) == 0x5678);
    assert(LD16(info + 0xeb) == 0xaabb && LD8(info + 0xed) == 0xcc && LD8(info + 0xef) == 0xdd);
    draw_cpu.x[0] = screen + 0x100; replace_ui_draw(&draw_cpu);
    assert(toast_draws == 1); // Never add a toast to an unrelated native screen.
    // Suppress only the owned toast from background HUD rendering.
    Cpu pane_cpu = {0}; pane_cpu.x[0] = manager + 0x51000;
    replace_pane_draw(&pane_cpu); assert(pane_calls == 0);
    replace_toast_foreground = true;
    replace_pane_draw(&pane_cpu); assert(pane_calls == 1);
    replace_toast_foreground = false;
    pane_cpu.x[0]++; replace_pane_draw(&pane_cpu); assert(pane_calls == 2);
    // Apply then advance the existing animator; never restart it, and never
    // advance twice when the normal HUD and fallback both run in one present.
    ST64(g_site.screen_mgr, manager + 0x60000);
    ST32(manager + 0x60b20, 0x3f800000);
    uint64_t anim = manager + 0x61000, vt = manager + 0x62000;
    ST64(replace_toast + 0x110, anim); ST64(replace_toast + 0x118, 0);
    ST64(anim, vt); ST64(vt + 0x18, 0x7000f6c9b4ull); ST64(vt + 0x28, 0x7000022c60ull);
    replace_toast_animate(&draw_cpu, replace_toast);
    replace_toast_animate(&draw_cpu, replace_toast);
    assert(apply_calls == 1 && anim_calls == 1);
    Cpu anim_cpu = draw_cpu; anim_cpu.x[0] = anim; anim_cpu.v[0] = 0x3f800000;
    replace_anim_advance(&anim_cpu); assert(anim_calls == 1);
    presents++; replace_anim_advance(&anim_cpu);
    replace_toast_animate(&draw_cpu, replace_toast);
    assert(apply_calls == 1 && anim_calls == 2);
    // Direct native chest preview is visual only, with no receive/award call.
    quick_replace_reset();
    ST64(0x7001d67af0ull, manager + 0x62000);
    ST64(manager + 0x62000, manager);
    ST64(manager + 0x44570, manager + 0x44580);
    // A previous item may have left modifier data in the native UI record.
    ST32(manager + 0x445c0, 42);
    uint8_t preview_record_before[0xb8];
    memcpy(preview_record_before, (void *)(uintptr_t)(manager + 0x44540), sizeof preview_record_before);
    ST64(0x7001d67960ull, manager + 0x63000);
    ST64(manager + 0x63000, manager + 0x64000);
    ST32(manager + 0x60028, 0x26); ST64(manager + 0x60030, manager + 0x21000);
    ST64(manager + 0x21058, manager + 0x65000);
    ST64(ai + 0x70, chest_string); ST64(chest_string + 8, name);
    strcpy((char *)(uintptr_t)name, "Weapon_Bow_001");
    ST64(ai + 8, manager + 0x1000); ST8(ai + 0xb0, 1);
    replace_chest = ai; replace_owner = manager + 0x1000; replace_category = 2;
    strcpy(replace_incoming, "Weapon_Bow_001");
    Cpu preview_cpu = {0}; preview_cpu.sp = input + 0x1000;
    Cpu preview_before = preview_cpu;
    replace_card_show(&preview_cpu); replace_card_show(&preview_cpu);
    assert(card_opens == 1 && !receive_calls && replace_card_open);
    assert(LD8(manager + 0x44565) == 1 && LD32(manager + 0x44558) == 1);
    assert(LD32(manager + 0x445c0) == 0 && LD32(manager + 0x445c8) == 0);
    assert(!strcmp((char *)(uintptr_t)(manager + 0x44580), "Weapon_Bow_001"));
    assert(!memcmp(&preview_cpu, &preview_before, sizeof preview_cpu));
    assert(!quick_replace_card_drop_allowed());
    replace_card_drop = true; assert(quick_replace_card_drop_allowed());
    ST64(event_manager + 0x1d2b8, event);
    assert(!quick_replace_card_drop_allowed()); // Never bypass active-event guard.
    ST64(event_manager + 0x1d2b8, 0); replace_card_drop = false;
    replace_screen = manager + 0x80000;
    assert(quick_replace_bottom_layout());
    // An open replacement preview allows sorting before a discard is in
    // progress. The discard exception must remain restricted to its own scope.
    assert(quick_replace_card_sort_allowed(replace_screen));
    assert(!quick_replace_card_drop_allowed() && !replace_card_drop);
    assert(!quick_replace_card_sort_allowed(replace_screen + 8));
    replace_closing = true;
    assert(!quick_replace_card_sort_allowed(replace_screen));
    replace_closing = false;
    int sort_category_before = replace_category; replace_category = -1;
    assert(!quick_replace_card_sort_allowed(replace_screen));
    replace_category = sort_category_before;
    ST64(event_manager + 0x1d2b8, event);
    assert(!quick_replace_card_sort_allowed(replace_screen));
    ST64(event_manager + 0x1d2b8, 0);
    uint64_t sort_source_before = replace_chest; replace_chest = 0;
    assert(!quick_replace_card_sort_allowed(replace_screen));
    replace_chest = sort_source_before;
    Cpu close_cpu = preview_cpu; close_cpu.x[0] = replace_card;
    replace_card_close_gate(&close_cpu); assert(!close_gate_calls);
    close_cpu.x[0]++; replace_card_close_gate(&close_cpu); assert(close_gate_calls == 1);
    close_cpu.x[0]--; replace_card_open = false;
    assert(!quick_replace_card_sort_allowed(replace_screen));
    replace_card_close_gate(&close_cpu); assert(close_gate_calls == 2);
    replace_card_open = true;
    replace_card_close(&preview_cpu);
    assert(!quick_replace_bottom_layout());
    assert(card_closes == 1 && !replace_card_open && !replace_card);
    assert(!quick_replace_card_sort_allowed(replace_screen));
    assert(!memcmp(preview_record_before, (void *)(uintptr_t)(manager + 0x44540), sizeof preview_record_before));
    ST32(ai + 0xc8, 0x10); ST32(ai + 0xcc, 17);
    assert(replace_preview_stage(&preview_cpu));
    assert(LD32(manager + 0x445c8) == 0x10 && LD32(manager + 0x445c0) == 17);
    replace_card_close(&preview_cpu);
    assert(!memcmp(preview_record_before, (void *)(uintptr_t)(manager + 0x44540), sizeof preview_record_before));
    // Capture modifiers only from the visible discovery card for this actor,
    // then stage and restore the shared record without awarding an item.
    replace_chest = 0; replace_event = event; replace_ground_preview = false;
    replace_handle = input + 0x1800; ST64(replace_handle + 8, actor);
    assert(replace_index_capture(&preview_cpu, &replace_ground_index, actor));
    ST64(actor + 0x10, name); ST8(manager + 0x650fe, 2); ST8(manager + 0x65100, 1);
    ST8(manager + 0x44565, 1);
    strcpy((char *)(uintptr_t)(manager + 0x44580), "Weapon_Bow_002");
    replace_capture_discovery_preview(&preview_cpu); assert(!replace_ground_preview);
    strcpy((char *)(uintptr_t)(manager + 0x44580), "Weapon_Bow_001");
    ST32(manager + 0x445c8, 0x100); ST32(manager + 0x445c0, 19);
    replace_capture_discovery_preview(&preview_cpu);
    assert(replace_ground_preview && replace_ground_flags == 0x100 && replace_ground_modifier == 19);
    memcpy(preview_record_before, (void *)(uintptr_t)(manager + 0x44540), sizeof preview_record_before);
    replace_event = 0;
    replace_card_show(&preview_cpu);
    assert(replace_card_open && LD32(manager + 0x445c8) == 0x100 && LD32(manager + 0x445c0) == 19);
    assert(quick_replace_card_sort_allowed(replace_screen) && !replace_card_drop);
    replace_card_drop = true; assert(quick_replace_card_drop_allowed());
    replace_card_drop = false; replace_card_close(&preview_cpu);
    assert(!memcmp(preview_record_before, (void *)(uintptr_t)(manager + 0x44540), sizeof preview_record_before));
    // A known ordinary item after a modified one must not inherit its bonus.
    // Both previews must restore the shared native record on cancellation.
    for (int modified = 1; modified >= 0; modified--) {
        replace_ground_preview = false;
        ST32(actor + 0xf98, modified ? 0x40 : 0);
        ST32(actor + 0xf9c, 750); // Stale value is ignored without flags.
        replace_capture_known_ground_preview(&preview_cpu);
        assert(replace_ground_preview);
        assert(replace_ground_flags == (modified ? 0x40 : 0));
        assert(replace_ground_modifier == (modified ? 750 : 0));
        replace_card_show(&preview_cpu);
        assert(replace_card_open);
        assert(LD32(manager + 0x445c8) == (modified ? 0x40 : 0));
        assert(LD32(manager + 0x445c0) == (modified ? 750 : 0));
        replace_card_close(&preview_cpu);
        assert(!memcmp(preview_record_before, (void *)(uintptr_t)(manager + 0x44540), sizeof preview_record_before));
    }
    replace_ground_preview = false; replace_handle = 0;
    // Gameplay action prompts are absent even from the first preview capture,
    // while other HUD panes and the normal gameplay prompt still render.
    ST64(manager + 0x21060, manager + 0x66000);
    ST64(manager + 0x66030, manager + 0x67000);
    ST64(manager + 0x67018, manager + 0x68000);
    unsigned pane_before = pane_calls;
    Cpu action_draw = {0}; action_draw.x[0] = manager + 0x68000;
    replace_category = -1; replace_chest_wait = true;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before);
    replace_chest_wait = false; replace_category = 0;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before);
    action_draw.x[0]++;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before + 1);
    action_draw.x[0]--; replace_category = -1;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before + 2);
    // The world item's 3D name/arrow must not overlap selected-item text.
    // Scope to screen 2's exact direct-child part, including first capture.
    ST64(manager + 0x21010, manager + 0x69000);
    ST64(manager + 0x69030, manager + 0x70000);
    ST64(manager + 0x70018, manager + 0x71000);
    visual_pane(manager + 0x71000, "RootPane");
    visual_pane(manager + 0x71200, "Pa_NoticeItem_00");
    visual_child(manager + 0x71000, manager + 0x71200);
    visual_pane(manager + 0x71300, "Pa_NoticeItem_00");
    pane_before = pane_calls;
    action_draw.x[0] = manager + 0x71200; replace_chest_wait = true;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before);
    replace_chest_wait = false; replace_category = 2;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before);
    action_draw.x[0] = manager + 0x71300;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before + 1);
    action_draw.x[0] = manager + 0x71200; feature_enabled = false;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before + 2);
    feature_enabled = true; replace_category = -1;
    replace_pane_draw(&action_draw); assert(pane_calls == pane_before + 3);
    replace_category = 2;
    // Cancel suppresses just the expected receive event, retaining native AI cleanup.
    replace_chest_handle = (uint64_t)(uintptr_t)hle_alloc(0x18);
    replace_owner = LD64(ai + 8); ST8(replace_chest_handle, 1);
    ST64(replace_chest_handle + 8, replace_owner);
    assert(replace_index_capture(&preview_cpu, &replace_chest_index, replace_owner));
    replace_receive_ai = ai; replace_receive_cancel = true;
    Cpu receive_cpu = preview_cpu;
    receive_cpu.x[0] = LD64(ai + 8); receive_cpu.x[1] = chest_string; receive_cpu.x[4] = ai + 0xc8;
    replace_receive(&receive_cpu); assert(!receive_calls);
    receive_cpu.x[4]++; replace_receive(&receive_cpu); assert(receive_calls == 1);
    receive_cpu.x[4]--; replace_receive_cancel = false;
    replace_confirmed = replace_chest_previewed = true;
    replace_receive(&receive_cpu); assert(receive_calls == 2 && replace_award_until);
    // Skip only the matching repeated visual action inside the native award event.
    uint64_t card_action = manager + 0x66000;
    ST64(card_action + 0x40, name); ST8(card_action + 0x1b, 0xa0);
    ST64(event_manager + 0x1d2b8, event);
    Cpu action_cpu = preview_cpu; action_cpu.x[0] = card_action;
    replace_card_action(&action_cpu);
    assert(!card_action_calls && LD8(card_action + 0x1b) == 0xa5 && !replace_award_until);
    replace_card_action(&action_cpu); assert(card_action_calls == 1);
    ST64(event_manager + 0x1d2b8, 0);
    // Equipped outgoing work must finish after the owned UI closes, before any
    // native incoming receive or synthetic ground pickup can run.
    for (int chest = 0; chest < 2; chest++) {
        Cpu pending_menu = {.sp = input + 0x1000};
        async_menu(&pending_menu, actor, ai, chest);
        unsigned old_receives = receive_calls, old_chests = chest_calls;
        uint64_t retained = chest ? replace_chest_handle : replace_handle;
        uint64_t retained_actor = LD64(retained + 8);
        tick_with(0xf0000, 0xf0000);
        assert(replace_outgoing_pending && !replace_confirmed && !replace_resume_chest);
        assert(LD64(retained + 8) == retained_actor && receive_calls == old_receives);
        if (chest) { chest_cpu.x[0] = ai; replace_chest_update(&chest_cpu); assert(chest_calls == old_chests); }
        // Capacity becoming available alone is not outgoing completion.
        chest_capacity = true; ground_result = 0;
        ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
        for (unsigned i = 0; i < 240; i++) tick_with(0, 0);
        assert(replace_outgoing_pending && !replace_confirmed && !replace_retry_until);
        assert(receive_calls == old_receives && LD64(retained + 8) == retained_actor);
        drop_result = QUICK_DROP_COMPLETE;
        // Completion also waits for the native selector to finish closing.
        ST32(manager + 0x64c24, 3); tick_with(0, 0);
        assert(replace_outgoing_pending && !replace_confirmed);
        ST32(manager + 0x64c24, 0); tick_with(0, 0);
        assert(!replace_outgoing_pending && replace_confirmed && replace_category == -1);
        if (chest) {
            assert(replace_resume_chest == ai && LD64(retained + 8) == retained_actor);
            chest_defer_receive = true;
            replace_chest_update(&chest_cpu);
            chest_defer_receive = false;
            assert(replace_resume_chest == ai && LD64(retained + 8) == retained_actor);
            assert(receive_calls == old_receives && LD8(ai + 0xb0));
            chest_emits_receive = true;
            replace_chest_update(&chest_cpu);
            chest_emits_receive = false;
            assert(receive_calls == old_receives + 1 && !replace_resume_chest && !LD64(retained + 8));
        } else {
            assert(replace_retry_until == presents + 3 && quick_replace_buttons(0) == 1);
            uint64_t retry_block = LD64(replace_ground_index);
            ST32(retry_block + 0x40, LD32(retry_block + 0x40) + 1);
            uint64_t retry_actor_page = actor & ~0xfffull;
            assert(!mprotect((void *)(uintptr_t)retry_actor_page, 0x1000, PROT_NONE));
            assert(quick_replace_buttons(0) == 0); // No stale A before the next guest poll.
            assert(!mprotect((void *)(uintptr_t)retry_actor_page, 0x1000, PROT_READ | PROT_WRITE));
            ST32(retry_block + 0x40, LD32(retry_block + 0x40) - 1);
            presents += 3; tick_with(0, 0);
            assert(!replace_retry_until && !LD64(retained + 8));
        }
    }
    // Ground target/name/modifier and unrelated-event changes permanently
    // cancel only the incoming award, even if the source later matches again.
    for (int changed = 0; changed < 6; changed++) {
        Cpu pending_menu = {.sp = input + 0x1000};
        async_menu(&pending_menu, actor, ai, false);
        uint64_t retained = replace_handle;
        ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
        if (changed == 0) ground_actor = actor + 0x100;
        else if (changed == 1) strcpy((char *)(uintptr_t)(input + 0x300), "Weapon_Bow_003");
        else if (changed == 2) ST32(actor + 0xf9c, 751);
        else if (changed == 3) ST64(event_manager + 0x1d2b8, event);
        else if (changed == 4) ST32(replace_ground_index + 8, LD32(replace_ground_index + 8) + 1);
        else ST8(actor + 0x64, 3);
        tick_with(0, 0);
        assert(replace_source_lost && replace_outgoing_pending);
        ground_actor = actor; ST32(actor + 0xf9c, 750);
        ST8(retained, 1); ST8(actor + 0x64, 1);
        if (changed == 4) ST32(replace_ground_index + 8, LD32(replace_ground_index + 8) - 1);
        strcpy((char *)(uintptr_t)(input + 0x300), "Weapon_Bow_001");
        ST64(event_manager + 0x1d2b8, 0);
        drop_result = QUICK_DROP_COMPLETE; ground_result = 0; chest_capacity = true;
        tick_with(0, 0);
        assert(!replace_outgoing_pending && !replace_confirmed && !replace_retry_until && !LD64(retained + 8));
    }
    // A changed chest reward remains suspended until outgoing completion;
    // cleanup consumes its native context while suppressing the changed award.
    Cpu changed_menu = {.sp = input + 0x1000};
    async_menu(&changed_menu, actor, ai, true);
    uint64_t changed_owner = replace_chest_handle;
    ST32(ai + 0xcc, 18); chest_capacity = true;
    unsigned changed_chests = chest_calls, changed_receives = receive_calls;
    chest_emits_receive = true;
    replace_chest_update(&chest_cpu);
    assert(replace_source_lost && replace_chest == ai && chest_calls == changed_chests);
    assert(receive_calls == changed_receives && LD8(ai + 0xb0));
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
    drop_result = QUICK_DROP_COMPLETE; tick_with(0, 0);
    assert(!replace_confirmed && replace_resume_chest == ai);
    replace_chest_update(&chest_cpu); chest_emits_receive = false;
    assert(chest_calls == changed_chests + 1 && receive_calls == changed_receives);
    assert(!LD8(ai + 0xb0) && !LD64(changed_owner + 8));
    // Lost chest ownership is checked before a retained AI pointer is read.
    Cpu pending_menu = {.sp = input + 0x1000};
    async_menu(&pending_menu, actor, ai, true);
    uint64_t retained = replace_chest_handle;
    ST64(LD64(replace_chest_index) + 0x48, 0);
    replace_chest = GUEST_LO + 0x1000; // Intentionally unmapped.
    tick_with(0, 0); assert(replace_source_lost && replace_outgoing_pending);
    drop_result = QUICK_DROP_COMPLETE;
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0); tick_with(0, 0);
    assert(!replace_outgoing_pending && !replace_confirmed && !replace_resume_chest);
    ST64(LD64(replace_chest_index) + 0x48, manager + 0x1000); // Restore the reused fixture actor.
    // The retained owner can remain live after its AI child becomes unreadable.
    // UI polling, capacity checks and close finalization must use host context
    // and an owned SafeString; only a current native AI callback may read AI.
    const uint64_t protected_ai = manager + 0x69000;
    Cpu protected_menu = {.sp = input + 0x1000};
    async_menu(&protected_menu, actor, protected_ai, true);
    uint64_t protected_owner = replace_chest_handle;
    assert(!mprotect((void *)(uintptr_t)protected_ai, 0x1000, PROT_NONE));
    tick_with(0, 0);
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
    for (unsigned i = 0; i < 4; i++) tick_with(0, 0);
    assert(replace_outgoing_pending && !replace_source_lost && replace_chest_owner_live(&protected_menu));
    chest_capacity = true; drop_result = QUICK_DROP_COMPLETE;
    tick_with(0, 0);
    assert(replace_confirmed && replace_resume_chest == protected_ai && !replace_outgoing_pending);
    tick_with(0, 0); // A missing native callback does not authorize an AI read.
    assert(replace_resume_chest == protected_ai && LD64(protected_owner + 8));
    assert(!mprotect((void *)(uintptr_t)protected_ai, 0x1000, PROT_READ | PROT_WRITE));
    Cpu protected_chest = {.sp = input + 0x1000}; protected_chest.x[0] = protected_ai;
    unsigned protected_receives = receive_calls;
    // Native update may replace/free its AI child before returning. Keep only
    // opaque resume identity afterward, even when the own-job resolver refuses.
    chest_defer_receive = chest_poison_on_defer = resolve_refused = true;
    unsigned deferred_resolves = resolve_calls;
    replace_chest_update(&protected_chest);
    assert(replace_resume_chest == protected_ai && resolve_calls == deferred_resolves);
    chest_defer_receive = chest_poison_on_defer = resolve_refused = false;
    assert(!mprotect((void *)(uintptr_t)protected_ai, 0x1000, PROT_READ | PROT_WRITE));
    chest_emits_receive = true; replace_chest_update(&protected_chest); chest_emits_receive = false;
    assert(receive_calls == protected_receives + 1 && !replace_resume_chest && !LD64(protected_owner + 8));
    // A native rejection before mutation permits no incoming award either.
    async_menu(&pending_menu, actor, ai, true);
    retained = replace_chest_handle;
    drop_result = QUICK_DROP_REJECTED;
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0); tick_with(0, 0);
    assert(!replace_outgoing_pending && !replace_confirmed && replace_resume_chest == ai);
    unsigned old_receives = receive_calls;
    chest_emits_receive = true; replace_chest_update(&chest_cpu); chest_emits_receive = false;
    assert(receive_calls == old_receives && !LD64(retained + 8));
    test_sequential_chests(actor, ai);
    // Native acquisition may return a non-retaining thread-local reference;
    // the separate retained generation link establishes cross-frame ownership.
    quick_replace_reset(); ST8(ai + 0xb0, 1); chest_capacity = false;
    weak_ref = true;
    unsigned weak_chests = chest_calls;
    replace_chest_update(&chest_cpu);
    weak_ref = false;
    assert(replace_chest == ai && replace_chest_wait && replace_chest_owner_live(&chest_cpu));
    assert(chest_calls == weak_chests);
    // The native resolver's own-job policy can refuse an otherwise live,
    // retained chest. A current native AI callback supplies its own lifetime;
    // UI polling still requires a successful fresh native resolution.
    resolve_refused = true;
    unsigned own_job_resolves = resolve_calls;
    replace_chest_update(&chest_cpu);
    assert(replace_chest == ai && replace_chest_wait && chest_calls == weak_chests);
    assert(resolve_calls == own_job_resolves && atomic_load(&replace_chest_context_current));
    assert(!replace_source_valid(&chest_cpu, true));
    resolve_refused = false;
    assert(replace_source_valid(&chest_cpu, true));
    replace_chest_release(&chest_cpu);
    // Actor jobs legitimately produce ordinary weak views. Retained indexed
    // ownership must permit the request, and generation loss must reject the
    // source before touching even an inaccessible old actor address.
    const uint64_t weak_actor = manager + 0x6a000;
    weak_ref = true;
    unsigned weak_drops = drops;
    Cpu weak_menu = {.sp = input + 0x1000};
    async_menu(&weak_menu, weak_actor, ai, false);
    assert(drops == weak_drops + 1 && replace_outgoing_pending && !LD8(replace_handle));
    assert(LD8(replace_ground_index + 0xc) && replace_source_valid(&weak_menu, true));
    uint64_t weak_block = LD64(replace_ground_index);
    ST32(weak_block + 0x40, LD32(weak_block + 0x40) + 1);
    assert(!mprotect((void *)(uintptr_t)weak_actor, 0x2000, PROT_NONE));
    tick_with(0, 0);
    assert(replace_source_lost && replace_outgoing_pending && drops == weak_drops + 1);
    ST32(manager + 0x64c24, 0); ST32(manager + 0x64c2c, 0);
    drop_result = QUICK_DROP_COMPLETE; tick_with(0, 0);
    assert(!replace_confirmed && !replace_retry_until && !LD8(replace_ground_index + 0xc));
    assert(!mprotect((void *)(uintptr_t)weak_actor, 0x2000, PROT_READ | PROT_WRITE));
    weak_ref = false;
    // Failure to acquire a retained link never starts a selector or a drop.
    quick_replace_reset(); index_capture_fails = true; ground_actor = actor;
    Cpu failed_source = {.sp = input + 0x1000};
    assert(!replace_capture_ground(&failed_source, input + 0x600));
    assert(replace_category == -1 && drops == weak_drops + 1 && !LD64(replace_handle + 8));
    index_capture_fails = false;
    test_ground_retry_handoff();
    assert(index_retains && index_releases);
    defer_drop = false;
    munmap(clock_globals, 0x1000);
    munmap(target_globals, 0x1000);
    munmap(globals, 0x2000);
    munmap(mem, 0x90000);
    puts("ok: selector/CPU guards, chest ownership, async completion, UI release and source-change cancellation");
    return 0;
}
