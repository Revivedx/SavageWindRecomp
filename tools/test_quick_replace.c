// Host invariants for the non-mutating native-selector prototype.
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/quick_replace.c"

static const uint64_t manager = 0x7240000000ull;
static const uint64_t input = 0x7240070000ull;
static const uint64_t screen = 0x7240080000ull;
static uint64_t buttons, consumed;
static uint32_t expected_held, expected_edge;
static unsigned ticks, fills, capacities;
static bool inspect_input, chest_ready = true, chest_capacity;
static bool emulate_direction_latch;
static unsigned open_attempts;
static unsigned chest_calls, predicate_calls, releases, drops;
GameSites g_site;
static unsigned item_popup_calls, full_info_calls, toast_draws, toast_calcs;
static unsigned pane_calls, anim_calls, apply_calls;
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
    assert(size == 0x18);
    void *p = (void *)(uintptr_t)(input + 0x1800);
    memset(p, 0, size); return p;
}
bool quick_drop_action_enabled(void) { return true; }
void quick_drop_action_request(Cpu *c, uint64_t scr, uint64_t mgr, uint64_t item,
                               int tab, int slot, const char *name) {
    (void)c; assert(scr == screen && mgr == manager && item == input + 0x2000);
    assert(tab == 0 && slot == 0 && !strcmp(name, "Weapon_Bow_002")); drops++;
}
void quick_drop_action_close(Cpu *c) { (void)c; chest_capacity = true; ground_result = 0; }
static void chest_original(Cpu *c) {
    chest_calls++;
    ST8(c->x[0] + 0xb0, 0); // Native handler consumes pending receive context.
}
void guest_call(Cpu *c, uint64_t address) {
    if (address == 0x70013e942cull) {
        assert(c->x[0] == manager + 0x64000 && c->x[1] == input + 0x600); card_opens++;
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
    } else if (address == 0x7000025cd0ull) {
        uint64_t info = manager + 0x53000;
        assert(c->x[0] == manager + 0x51000 && c->x[1] == info && c->x[2] == manager + 0x56008);
        assert(LD64(info + 0xf8) == manager + 0x55000 && LD64(info + 0xc0) == manager + 0x54000);
        assert(LD16(info + 0xeb) == 6 && LD8(info + 0xed) == 0 && LD8(info + 0xef) == 1); toast_draws++;
    } else if (address == 0x7000f60944ull) {
        assert(c->x[0] == manager + 0x53000);
    } else if (address == 0x7001537800ull) {
        predicate_calls++;
        assert(c->x[0] == input + 0x400);
        assert(LD64(c->x[1] + 8) == 0x700164f739ull);
        c->x[0] = chest_ready;
    } else if (address == 0x7000be21e0ull) {
        assert(c->x[0] == input + 0x600 || c->x[0] == input + 0x200 || c->x[0] == input + 0x4028);
        c->x[0] = chest_capacity;
    } else if (address == 0x7001070ce0ull) {
        memcpy((void *)(uintptr_t)c->x[0], (void *)(uintptr_t)c->x[1], 0x18);
    } else if (address == 0x7001126464ull) {
        releases++; memset((void *)(uintptr_t)c->x[0], 0, 0x18);
    } else if (address == 0x70010d544cull) {
        ST64(c->x[1] + 8, ground_actor);
    } else if (address == 0x7000e780f0ull) c->x[0] = ground_result;
    else if (address == 0x7000eaff08ull || address == 0x7000eaff18ull) {
        assert(c->x[0] == manager + 0x28);
    } else if (address == 0x7000d970a8ull) c->x[0] = 1;
    else if (address == 0x7000d992acull) c->x[0] = input + 0x2000;
    else assert(0);
    c->x[4] = 0xdead; c->nzcv = 0x12340000;
}
uint64_t input_buttons_now(void) { return buttons; }
void input_consume_buttons(uint64_t mask) { consumed |= mask; }
static void tick_original(Cpu *c) {
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
static void popup_original(Cpu *c) { (void)c; }
static void item_popup_original(Cpu *c) { (void)c; item_popup_calls++; }
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
    case 0x7001126464ull:
    case 0x70010d544cull:
    case 0x7000e780f0ull: return popup_original;
    default: assert(0); return NULL;
    }
}
void hle_override(uint64_t addr, HostFn fn, const char *name) {
    (void)addr; (void)fn; (void)name;
}
static void tick_with(uint32_t held, uint32_t edge) {
    ST32(input + 0x114, held);
    ST32(input + 8, edge);
    Cpu c = {0}; c.x[0] = manager; c.sp = input + 0x1000;
    unsigned before = ticks;
    replace_tick(&c);
    assert(ticks == before + 1 && c.x[5] == 0x12345678);
    assert(LD32(input + 0x114) == held && LD32(input + 8) == edge);
}
int main(int argc, char **argv) {
    bool execute = argc > 1 && !strcmp(argv[1], "execute");
    if (execute) setenv("SAVAGEWIND_QUICK_REPLACE_EXECUTE", "1", 1);
    else unsetenv("SAVAGEWIND_QUICK_REPLACE_EXECUTE");
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
    setenv("SAVAGEWIND_QUICK_REPLACE", "1", 1);
    unsetenv("SAVAGEWIND_QUICK_REPLACE_TEST_CATEGORY");
    quick_replace_init();
    ST64(manager + 0x649f0, input);
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
    replace_closing = replace_test_started = true;
    atomic_store(&replace_trace_count, 42);
    quick_replace_reset();
    assert(replace_category == -1 && !replace_screen && !replace_buttons);
    assert(!replace_frames && !replace_closing && !replace_test_started);
    assert(atomic_load(&replace_trace_count) == 0);
    // Trace must preserve the complete original callback result, including SIMD.
    uint64_t string = input + 0x200, name = input + 0x300;
    ST64(string + 8, name);
    strcpy((char *)(uintptr_t)name, "Weapon_Sword_001");
    memset(&capacity_result, 0xa5, sizeof capacity_result);
    capacity_result.x[0] = 0;
    Cpu traced = {0}; traced.x[0] = string; traced.x[30] = 0x7000123456ull;
    replace_capacity_trace(&traced);
    assert(capacities == 1 && !memcmp(&traced, &capacity_result, sizeof traced));
    assert(atomic_load(&replace_trace_count) == 1);
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
    // Recycled owner/name or cleared pending flag releases the old request.
    chest_ready = true;
    for (int changed = 0; changed < 3; changed++) {
        quick_replace_reset(); buttons = 0;
        ST8(ai + 0xb0, 1); ST64(ai + 8, manager + 0x1000);
        strcpy((char *)(uintptr_t)name, "Weapon_Bow_001");
        replace_chest_update(&chest_cpu);
        assert(replace_chest == ai);
        unsigned old_calls = chest_calls;
        if (changed == 0) ST64(ai + 8, manager + 0x2000);
        else if (changed == 1) strcpy((char *)(uintptr_t)name, "Weapon_Bow_002");
        else ST8(ai + 0xb0, 0);
        replace_chest_update(&chest_cpu);
        assert(chest_calls == old_calls + 1 && !replace_chest && replace_closing);
    }
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
    Cpu before_item_popup = item_popup;
    replace_item_popup(&item_popup);
    assert(item_popup_calls == 5 && replace_category == 2 && LD64(replace_handle + 8) == actor);
    assert(!memcmp(&item_popup, &before_item_popup, sizeof item_popup));
    assert(!strcmp(replace_incoming, "Weapon_Bow_001"));
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
    replace_popup(&popup);
    assert(replace_handle && LD64(replace_handle + 8) == actor && replace_category == 2);
    menu_cpu.sp = input + 0x1000; replace_fill(&menu_cpu);
    ST8(screen + 0x38e4, 0); ST32(screen + 0x387c, 1);
    ground_actor = actor + 0x100; buttons = 1;
    assert(quick_replace_menu(&menu_cpu));
    assert(!replace_closing && !replace_confirmed && !drops);
    assert(LD8(screen + 0x38e4) == 0 && LD32(screen + 0x387c) == 1);
    // With execution disabled even a valid source must leave the menu/inventory unchanged.
    buttons = 0; quick_replace_menu(&menu_cpu); ground_actor = actor; buttons = 1;
    if (!execute) {
        assert(quick_replace_menu(&menu_cpu) && !replace_closing && !drops);
    } else {
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
    ST64(0x7001e374b8ull, manager + 0x60000);
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
    assert(!strcmp((char *)(uintptr_t)(manager + 0x44580), "Weapon_Bow_001"));
    assert(!memcmp(&preview_cpu, &preview_before, sizeof preview_cpu));
    assert(!quick_replace_card_drop_allowed());
    replace_card_drop = true; assert(quick_replace_card_drop_allowed());
    ST64(event_manager + 0x1d2b8, event);
    assert(!quick_replace_card_drop_allowed()); // Never bypass active-event guard.
    ST64(event_manager + 0x1d2b8, 0); replace_card_drop = false;
    replace_screen = manager + 0x80000;
    assert(quick_replace_bottom_layout());
    Cpu close_cpu = preview_cpu; close_cpu.x[0] = replace_card;
    replace_card_close_gate(&close_cpu); assert(!close_gate_calls);
    close_cpu.x[0]++; replace_card_close_gate(&close_cpu); assert(close_gate_calls == 1);
    close_cpu.x[0]--; replace_card_open = false;
    replace_card_close_gate(&close_cpu); assert(close_gate_calls == 2);
    replace_card_open = true;
    replace_card_close(&preview_cpu);
    assert(!quick_replace_bottom_layout());
    assert(card_closes == 1 && !replace_card_open && !replace_card);
    assert(!memcmp(preview_record_before, (void *)(uintptr_t)(manager + 0x44540), sizeof preview_record_before));
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
    replace_category = 2;
    // Cancel suppresses just the expected receive event, retaining native AI cleanup.
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
    munmap(clock_globals, 0x1000);
    munmap(target_globals, 0x1000);
    munmap(globals, 0x2000);
    munmap(mem, 0x90000);
    puts("ok: temporary direction injection, category isolation, A/B close, reset, trace CPU result, chest suspension/resume/identity");
    return 0;
}
