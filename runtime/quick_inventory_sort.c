// Native pouch sorting for the verified BOTW 1.9.0 quick selector.
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "hle.h"
#include "game_sites.h"
#include "quick_inventory_sort.h"
#include "settings.h"

extern uint64_t input_buttons_now(void);
extern void input_consume_buttons(uint64_t);

enum { SORT_X = 1 << 2, SORT_OTHER_ACTIONS = 1 | 2 | 8 };
static bool sort_ready;
static uint64_t sort_screen, sort_buttons;
static _Atomic uint64_t sort_hint_time;
static const int sort_categories[] = {0, 2, 1}; // weapon, shield, bow
static const int sort_types[] = {0, 3, 1};

static uint64_t sort_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static bool sort_range(uint64_t p, uint64_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}
static uint64_t sort_call(Cpu *source, uint64_t entry, uint64_t a, uint64_t b, uint64_t d) {
    Cpu call = *source;
    call.x[0] = a; call.x[1] = b; call.x[2] = d; call.x[3] = 0;
    guest_call(&call, entry);
    return call.x[0];
}
bool quick_inventory_sort_enabled(void) {
    return g_settings.quick_sort && !strcmp(g_game_version, "1.9.0");
}

static bool sort_shape(uint64_t screen) {
    if (!sort_ready || !quick_inventory_sort_enabled() || !sort_range(screen, 0x38e5) ||
        LD8(screen + 0x38e4) == 2) return false;
    int category = (int32_t)LD32(screen + 0x3738);
    int count = (int32_t)LD32(screen + 0x3880);
    int selected = (int32_t)LD32(screen + 0x387c);
    return category >= 0 && category <= 2 && count > 0 && count <= 20 &&
        selected >= -1 && selected < count && sort_range(LD64(screen + 0x3640), 0x45000) &&
        sort_range(LD64(screen + 0x37a8), 0x40);
}

RT_ON_RESET(quick_inventory_sort_reset) {
    sort_ready = false;
    sort_screen = sort_buttons = 0;
    atomic_store(&sort_hint_time, 0);
}
void rt_quick_inventory_sort_begin(Cpu *c) {
    atomic_store(&sort_hint_time, 0);
    if (!quick_inventory_sort_enabled()) {
        sort_ready = false;
        sort_screen = sort_buttons = 0;
        return;
    }
    sort_ready = quick_inventory_sort_init();
    sort_screen = c->x[0]; sort_buttons = input_buttons_now();
}
void quick_inventory_sort_publish(uint64_t screen) {
    atomic_store(&sort_hint_time, sort_shape(screen) ? sort_now() : 0);
}
bool quick_inventory_sort_hint(void) {
    uint64_t t = atomic_load(&sort_hint_time);
    return g_settings.quick_sort && t && sort_now() - t <= 150;
}

// Every retained identity is pinned by the native recursive inventory mutex.
static bool sort_snapshot(Cpu *c, uint64_t screen, uint64_t mgr, int category,
                          uint64_t items[20], int count) {
    int matches[20] = {0};
    for (int i = 0; i < count; i++) {
        uint64_t item = items[i] = LD64(screen + 0x37c8 + i * 8);
        if (!sort_range(item, 0x90)) return false;
        for (int j = 0; j < i; j++) if (items[j] == item) return false;
    }
    int tabs = (int32_t)LD32(mgr + 0x44498);
    if (tabs < 1 || tabs > 50) return false;
    for (int tab = 0; tab < tabs; tab++) {
        int n = (int32_t)sort_call(c, g_site.pouch_page_count, mgr, tab, 0);
        if (n < 0 || n > 20) return false;
        for (int slot = 0; slot < n; slot++) {
            uint64_t item = sort_call(c, g_site.pouch_page_item, mgr, tab, slot);
            for (int i = 0; i < count; i++) matches[i] += item == items[i];
        }
    }
    for (int i = 0; i < count; i++) {
        // Prove live membership before dereferencing a potentially stale UI pointer.
        if (matches[i] != 1) return false;
        uint64_t item = items[i];
        if (!LD8(item + 0x25) || (int32_t)LD32(item + 0x18) != sort_types[category] ||
            (int32_t)LD32(item + 0x20) <= 0) return false;
        uint64_t name = LD64(item + 0x30);
        if (!sort_range(name, 97) || !LD8(name) ||
            strnlen((const char *)(uintptr_t)name, 97) >= 97 ||
            (int32_t)sort_call(c, g_site.pouch_item_type, item + 0x28, 0, 0) != sort_types[category]) return false;
    }
    return true;
}

static void sort_execute(Cpu *original) {
    uint64_t screen = original->x[0];
    if (!sort_shape(screen) || (original->sp & 15) || original->sp < GUEST_LO + 0x2000 ||
        !sort_range(original->sp - 0x2000, 0x2000)) return;
    Cpu scratch = *original;
    scratch.sp -= 0x100;
    // Honor the native event, modal and equipment restrictions.
    if (sort_call(&scratch, g_site.quick_sort_event, 0, 0, 0) ||
        sort_call(&scratch, g_site.quick_sort_player_busy, 0, 0, 0) ||
        sort_call(&scratch, g_site.quick_sort_modal, 0, 0, 0)) return;
    uint64_t mgr = LD64(screen + 0x3640), items[20], equipped[4];
    int category = (int32_t)LD32(screen + 0x3738);
    int count = (int32_t)LD32(screen + 0x3880);
    int selected = (int32_t)LD32(screen + 0x387c);
    sort_call(&scratch, g_site.pouch_lock, mgr + 0x28, 0, 0);
    if (!sort_snapshot(&scratch, screen, mgr, category, items, count)) goto done;
    // An unequip cell must exist in the source mapping as well as the rebuilt one.
    if (selected == -1 && (int32_t)LD32(screen + 0x3884) != -1) goto done;
    for (int i = 0; i < 4; i++) equipped[i] = LD64(mgr + 0x447e0 + i * 8);
    uint64_t selected_item = selected < 0 ? 0 : items[selected];
    sort_call(&scratch, g_site.pouch_sort, mgr, sort_categories[category], 0);
    // Pure sort preserves PouchItem nodes. No cleanup/discard sweep is involved.
    sort_call(&scratch, g_site.quick_sort_refill, screen, category, 0);
    int rebuilt = (int32_t)LD32(screen + 0x3880), restored = -1;
    if (rebuilt < 0 || rebuilt > 20) {
        LOG("[quick-inventory-sort] invalid rebuilt count=%d; refresh stopped\n", rebuilt);
        goto done;
    }
    bool intact = rebuilt == count;
    for (int i = 0; intact && i < count; i++) {
        int matches = 0;
        for (int j = 0; j < rebuilt; j++)
            matches += items[i] == LD64(screen + 0x37c8 + j * 8);
        intact = matches == 1;
    }
    for (int i = 0; i < 4; i++) intact &= equipped[i] == LD64(mgr + 0x447e0 + i * 8);
    if (intact) {
        for (int i = 0; selected_item && i < rebuilt; i++)
            if (LD64(screen + 0x37c8 + i * 8) == selected_item) restored = i;
        ST32(screen + 0x387c, restored);
    }
    // Keep +3878 from the refill: it identifies the actual equipped instance.
    sort_call(&scratch, g_site.quick_sort_mapping, screen, 0, 0);
    int highlighted = (int32_t)LD32(screen + 0x387c), cursor = -1;
    for (int i = 0; i < 21; i++)
        if ((int32_t)LD32(screen + 0x3884 + i * 4) == highlighted) { cursor = i; break; }
    if (rebuilt >= 0 && rebuilt <= 20 && cursor >= 0) {
        int extra = (int32_t)LD32(screen + 0x3884) == -1 || highlighted == -1;
        sort_call(&scratch, g_site.quick_sort_cursor, LD64(screen + 0x37a8), cursor, rebuilt + extra);
        sort_call(&scratch, g_site.quick_sort_labels, screen, 0, 0);
    }
    LOG("[quick-inventory-sort] category=%d native=%d item=%" PRIx64
        " selected=%d->%d count=%d->%d intact=%d\n", category, sort_categories[category],
        selected_item, selected, highlighted, count, rebuilt, intact);
done:
    sort_call(&scratch, g_site.pouch_unlock, mgr + 0x28, 0, 0);
}

void rt_quick_inventory_sort_menu(Cpu *c) {
    if (!quick_inventory_sort_enabled()) {
        sort_screen = sort_buttons = 0;
        atomic_store(&sort_hint_time, 0);
        return;
    }
    if (!sort_ready) sort_ready = quick_inventory_sort_init();
    quick_inventory_sort_publish(c->x[0]);
    uint64_t buttons = input_buttons_now(), pressed = buttons & ~sort_buttons;
    sort_buttons = buttons;
    if (c->x[0] != sort_screen) { sort_screen = c->x[0]; return; }
    if (!(pressed & SORT_X) || !sort_ready || !quick_inventory_sort_enabled()) return;
    // Cancel and other mutations win even when X has the newest edge.
    if (buttons & SORT_OTHER_ACTIONS) return;
    if (!sort_shape(c->x[0])) return;
    input_consume_buttons(SORT_X);
    sort_execute(c);
}

bool quick_inventory_sort_init(void) {
    if (strcmp(g_game_version, "1.9.0")) return false;
    const uint64_t helpers[] = {g_site.pouch_lock, g_site.pouch_unlock, g_site.pouch_page_count,
        g_site.pouch_page_item, g_site.pouch_item_type, g_site.quick_sort_event, g_site.quick_sort_player_busy,
        g_site.quick_sort_modal, g_site.pouch_sort, g_site.quick_sort_refill, g_site.quick_sort_mapping,
        g_site.quick_sort_cursor, g_site.quick_sort_labels};
    for (size_t i = 0; i < sizeof helpers / sizeof *helpers; i++)
        if (!helpers[i] || !dispatch_lookup(helpers[i])) return false;
    return true;
}
