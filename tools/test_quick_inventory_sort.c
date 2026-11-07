// Native sort boundaries: instance identity, category mapping, input priority,
// unchanged guest registers/equipment, and strict pre-mutation rejection.
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/quick_inventory_sort.c"

const char *g_game_version = "1.9.0";
static const uint64_t mgr = 0x7240000000ull, screen = 0x7240050000ull;
static const uint64_t item_base = 0x7240048000ull, widget = 0x7240054000ull;
static uint64_t buttons, consumed, live[20];
static bool unknown, duplicate, event, modal, missing_helper;
static int depth, locks, unlocks, sorts, fill_calls, widget_calls, labels, category, count = 3;
static int expected_cursor, expected_cells;

Settings g_settings = {.quick_sort = true};
GameSites g_site;
uint64_t input_buttons_now(void) { return buttons; }
void input_consume_buttons(uint64_t b) { consumed |= b; }
static void noop(Cpu *c) { (void)c; }
GuestFn dispatch_lookup(uint64_t entry) { (void)entry; return missing_helper ? NULL : noop; }

static void fill(void) {
    int equipped = -1;
    for (int i = 0; i < count; i++) {
        ST64(screen + 0x37c8 + i * 8, live[i]);
        if (LD8(live[i] + 0x24)) equipped = i;
    }
    ST32(screen + 0x3880, count);
    ST32(screen + 0x3878, equipped); ST32(screen + 0x387c, equipped);
}
static void mapping(void) {
    // Exercise both widget-map shapes; production derives the extra cell from
    // the native mapping rather than assuming a category-specific count.
    int extra = category != 2;
    for (int i = 0; i < 21; i++) ST32(screen + 0x3884 + i * 4, -1);
    for (int i = 0; i < count; i++) ST32(screen + 0x3884 + (i + extra) * 4, i);
}
void guest_call(Cpu *c, uint64_t entry) {
    if (entry == 0x7000eaff08ull) {
        assert(c->x[0] == mgr + 0x28 && depth == 0); depth++; locks++;
    } else if (entry == 0x7000eaff18ull) {
        assert(c->x[0] == mgr + 0x28 && depth == 1); depth--; unlocks++;
    } else if (entry == 0x7000d1499cull) c->x[0] = event;
    else if (entry == 0x7000d35b80ull) c->x[0] = 0;
    else if (entry == 0x7000e7e750ull) c->x[0] = modal;
    else if (entry == 0x7000d91414ull) {
        assert(depth == 1); c->x[0] = unknown ? -1 : sort_types[category];
    } else if (entry == 0x7000d970a8ull) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 1); c->x[0] = count;
    } else if (entry == 0x7000d992acull) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] == 0 && c->x[2] < (unsigned)count);
        c->x[0] = duplicate ? live[0] : live[c->x[2]];
    } else if (entry == 0x7000d980a0ull) {
        static const int native[] = {0, 2, 1};
        assert(depth == 1 && c->x[0] == mgr && c->x[1] == (unsigned)native[category] && c->x[2] == 0);
        sorts++;
        // Rotate instances, including duplicates with distinct modifiers.
        uint64_t last = live[count - 1];
        for (int i = count - 1; i > 0; i--) live[i] = live[i - 1];
        live[0] = last;
    } else if (entry == 0x7000e27c2cull) {
        assert(depth == 1 && c->x[0] == screen && c->x[1] == (unsigned)category); fill_calls++; fill();
    } else if (entry == 0x7000e28070ull) {
        assert(depth == 1 && c->x[0] == screen); mapping();
    } else if (entry == 0x7000d665a4ull) {
        assert(depth == 1 && c->x[0] == widget && c->x[1] == (unsigned)expected_cursor &&
               c->x[2] == (unsigned)expected_cells); widget_calls++;
    } else if (entry == 0x7000e278c0ull) { assert(depth == 1 && c->x[0] == screen); labels++; }
    else { fprintf(stderr, "unexpected sort guest call: %" PRIx64 "\n", entry); abort(); }
    c->x[10] = 0xdead; c->pc = 0xbeef; c->v[0] = 0x1234; c->nzcv = 0xa0000000;
}
static void setup(int cat) {
    category = cat;
    ST64(screen + 0x3640, mgr); ST64(screen + 0x37a8, widget);
    ST32(screen + 0x3738, cat); ST8(screen + 0x38e4, 0); ST32(mgr + 0x44498, 1);
    for (int i = 0; i < 4; i++) ST64(mgr + 0x447e0 + i * 8, 0);
    for (int i = 0; i < count; i++) {
        uint64_t item = live[i] = item_base + i * 0x100;
        ST32(item + 0x18, sort_types[cat]); ST32(item + 0x20, 100 - i);
        ST8(item + 0x24, i == 0); ST8(item + 0x25, 1);
        ST64(item + 0x30, item_base + 0x3000); ST32(item + 0x80, i + 7);
    }
    strcpy((char *)(uintptr_t)(item_base + 0x3000), "Weapon_SameName");
    ST64(mgr + 0x447e0 + sort_types[cat] * 8, item_base);
    fill(); mapping(); ST32(screen + 0x387c, 1);
}
static void edge(Cpu *c, uint64_t b) {
    buttons = 0; rt_quick_inventory_sort_menu(c);
    buttons = b; rt_quick_inventory_sort_menu(c);
}
int main(void) {
    void *mem = mmap((void *)(uintptr_t)mgr, 0x60000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mem == (void *)(uintptr_t)mgr);
    g_site.pouch_lock = 0x7000eaff08ull;
    g_site.pouch_unlock = 0x7000eaff18ull;
    g_site.pouch_page_count = 0x7000d970a8ull;
    g_site.pouch_page_item = 0x7000d992acull;
    g_site.pouch_item_type = 0x7000d91414ull;
    g_site.quick_sort_event = 0x7000d1499cull;
    g_site.quick_sort_player_busy = 0x7000d35b80ull;
    g_site.quick_sort_modal = 0x7000e7e750ull;
    g_site.pouch_sort = 0x7000d980a0ull;
    g_site.quick_sort_refill = 0x7000e27c2cull;
    g_site.quick_sort_mapping = 0x7000e28070ull;
    g_site.quick_sort_cursor = 0x7000d665a4ull;
    g_site.quick_sort_labels = 0x7000e278c0ull;
    missing_helper = true; assert(!quick_inventory_sort_init());
    missing_helper = false; g_game_version = "other"; assert(!quick_inventory_sort_init());
    g_game_version = "1.9.0"; assert(quick_inventory_sort_init());
    Cpu c = {.sp = mgr + 0x46000, .nzcv = 0xc0000000};
    c.x[0] = screen; c.x[3] = 31; c.v[0] = 99; c.pc = 0x7000123456;
    Cpu original = c;
    for (int cat = 0; cat < 3; cat++) {
        setup(cat); buttons = SORT_X; rt_quick_inventory_sort_begin(&c);
        int before = sorts; rt_quick_inventory_sort_menu(&c); assert(sorts == before);
        unsigned char saved_items[0x300]; memcpy(saved_items, (void *)(uintptr_t)item_base, sizeof saved_items);
        uint64_t selected_item = live[1]; expected_cursor = 2 + (cat != 2); expected_cells = count + (cat != 2);
        edge(&c, SORT_X);
        assert(sorts == before + 1 && LD64(screen + 0x37c8 + LD32(screen + 0x387c) * 8) == selected_item);
        assert(LD64(screen + 0x37c8 + LD32(screen + 0x3878) * 8) == item_base);
        assert(LD64(mgr + 0x447e0 + sort_types[cat] * 8) == item_base);
        assert(!memcmp(saved_items, (void *)(uintptr_t)item_base, sizeof saved_items));
        rt_quick_inventory_sort_menu(&c); assert(sorts == before + 1); // held: no repeat
        expected_cursor = cat != 2; edge(&c, SORT_X);
        assert(sorts == before + 2 && LD64(screen + 0x37c8 + LD32(screen + 0x387c) * 8) == selected_item);
        assert(!memcmp(&c, &original, sizeof c) && depth == 0 && locks == unlocks);
    }
    setup(0); buttons = 0; rt_quick_inventory_sort_begin(&c);
    int before = sorts;
    edge(&c, SORT_X | 2); edge(&c, SORT_X | 1); edge(&c, SORT_X | 8); assert(sorts == before);
    unknown = true; edge(&c, SORT_X); unknown = false; assert(sorts == before);
    duplicate = true; edge(&c, SORT_X); duplicate = false; assert(sorts == before);
    event = true; edge(&c, SORT_X); event = false; assert(sorts == before);
    modal = true; edge(&c, SORT_X); modal = false; assert(sorts == before);
    g_settings.quick_sort = false; edge(&c, SORT_X); g_settings.quick_sort = true; assert(sorts == before);
    ST8(live[1] + 0x25, 0); edge(&c, SORT_X); ST8(live[1] + 0x25, 1); assert(sorts == before);
    ST32(live[1] + 0x18, 3); edge(&c, SORT_X); ST32(live[1] + 0x18, 0); assert(sorts == before);
    ST32(screen + 0x3738, 3); edge(&c, SORT_X); ST32(screen + 0x3738, 0); assert(sorts == before);
    ST8(screen + 0x38e4, 2); edge(&c, SORT_X); ST8(screen + 0x38e4, 0); assert(sorts == before);
    c.sp++; edge(&c, SORT_X); c = original; assert(sorts == before);
    ST32(screen + 0x3880, 21); edge(&c, SORT_X); ST32(screen + 0x3880, 3); assert(sorts == before);
    ST64(screen + 0x37d0, GUEST_LO + 0x1000); // unmapped stale UI pointer: never dereference it
    edge(&c, SORT_X); ST64(screen + 0x37d0, live[1]); assert(sorts == before);
    c.x[0] += 0x100; edge(&c, SORT_X); c = original;
    rt_quick_inventory_sort_menu(&c); assert(sorts == before); // new screen suppresses old edge
    // Synthetic unequip survives; source selected/equipped identities stay distinct.
    ST32(screen + 0x387c, -1); expected_cursor = 0; expected_cells = 4;
    edge(&c, SORT_X); assert(sorts == before + 1 && (int32_t)LD32(screen + 0x387c) == -1);
    // Full-capacity gear with an unequip cell has 21 widget positions. The
    // selected last instance moves to index zero and remains visible there.
    count = 20; setup(0); ST32(screen + 0x387c, 19);
    uint64_t last = live[19]; expected_cursor = 1; expected_cells = 21;
    edge(&c, SORT_X);
    assert(sorts == before + 2 && LD32(screen + 0x387c) == 0 && LD64(screen + 0x37c8) == last);
    quick_inventory_sort_publish(screen); assert(quick_inventory_sort_hint());
    atomic_store(&sort_hint_time, sort_now() - 200); assert(!quick_inventory_sort_hint());
    quick_inventory_sort_reset(); assert(!quick_inventory_sort_hint());
    assert(!memcmp(&c, &original, sizeof c) && depth == 0 && locks == unlocks);
    assert(fill_calls == sorts && labels == sorts && widget_calls == sorts && consumed == SORT_X);
    munmap(mem, 0x60000);
    puts("ok: native sort categories, duplicate instance identity, equipment, CPU/locks, edges, cancel priority and guards");
}
