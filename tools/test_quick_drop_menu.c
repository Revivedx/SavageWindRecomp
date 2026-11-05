// Selector edges and exact locked identity lookup; no game files or window.
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/quick_drop.c"

GameSites g_site;
static bool enabled = true, available = true, duplicate;
static uint64_t buttons, consumed;
static int depth, locks, unlocks, requests;
static QuickDropResult result;
static const uint64_t mgr = 0x7240000000ull, screen = 0x7240050000ull;
static const uint64_t item = 0x7240048000ull, name = 0x7240055000ull;
bool quick_drop_action_enabled(void) { return enabled; }
bool quick_drop_action_init(void) { return available; }
QuickDropResult quick_drop_action_result(void) { return result; }
uint64_t input_buttons_now(void) { return buttons; }
void input_consume_buttons(uint64_t mask) { consumed |= mask; }
void quick_drop_action_close(Cpu *c) { (void)c; }
void quick_drop_action_request(Cpu *c, uint64_t s, uint64_t m, uint64_t p, int tab, int slot, const char *n) {
    (void)c;
    assert(s == screen && m == mgr && p == item && tab == 0 && slot == 1 && !depth);
    assert(!strcmp(n, "Weapon_Sword_001"));
    requests++; result = QUICK_DROP_PENDING;
}
void guest_call(Cpu *c, uint64_t entry) {
    if (entry == g_site.pouch_lock) { assert(c->x[0] == mgr + 0x28 && !depth); depth++; locks++; }
    else if (entry == g_site.pouch_unlock) { assert(c->x[0] == mgr + 0x28 && depth == 1); depth--; unlocks++; }
    else if (entry == g_site.pouch_page_count) { assert(depth == 1 && c->x[0] == mgr); c->x[0] = c->x[1] == 0 || duplicate ? 2 : 0; }
    else if (entry == g_site.pouch_page_item) { assert(depth == 1 && c->x[0] == mgr); c->x[0] = c->x[2] == 1 ? item : 0; }
    else assert(0);
    c->x[4] = 0xdead; c->pc = 0xbeef; c->nzcv = 0x10000000; c->v[0] = 0x1234;
}
static void edge(Cpu *c, uint64_t b) {
    buttons = 0; rt_quick_drop_menu(c);
    buttons = b; rt_quick_drop_menu(c);
}
int main(void) {
    void *memory = mmap((void *)(uintptr_t)mgr, 0x60000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory == (void *)(uintptr_t)mgr);
    g_site.pouch_lock = 1; g_site.pouch_unlock = 2;
    g_site.pouch_page_count = 3; g_site.pouch_page_item = 4;
    ST64(screen + 0x3640, mgr); ST32(screen + 0x3738, 0);
    ST32(screen + 0x3878, 0); ST32(screen + 0x387c, 1); ST32(screen + 0x3880, 2);
    ST64(screen + 0x37d0, item); ST32(mgr + 0x44498, 2);
    ST8(item + 0x25, 1); ST32(item + 0x20, 2000); ST64(item + 0x30, name);
    strcpy((char *)(uintptr_t)name, "Weapon_Sword_001");
    Cpu c = {.sp = mgr + 0x5f000, .nzcv = 0xa0000000}; c.x[0] = screen; c.x[4] = 44; c.v[0] = 99;
    Cpu before = c;
    buttons = QUICK_DROP_BUTTON; rt_quick_drop_begin(&c); rt_quick_drop_menu(&c);
    assert(!requests && quick_drop_hint_state() == 2);
    edge(&c, QUICK_DROP_BUTTON);
    assert(requests == 1 && (consumed & QUICK_DROP_BUTTON));
    rt_quick_drop_menu(&c); assert(requests == 1);
    assert(!memcmp(&c, &before, sizeof c) && !depth && locks == unlocks);
    duplicate = true; edge(&c, QUICK_DROP_BUTTON); assert(requests == 1); duplicate = false;
    ST32(screen + 0x3880, 0); edge(&c, QUICK_DROP_BUTTON); assert(requests == 1);
    ST32(screen + 0x3880, 2); consumed = 0;
    edge(&c, QUICK_BACK_BUTTON | QUICK_DROP_BUTTON | (1 << 14));
    assert(requests == 1 && LD8(screen + 0x38e4) == 2);
    assert(LD32(screen + 0x387c) == LD32(screen + 0x3878));
    assert(consumed == (QUICK_BACK_BUTTON | (1 << 14)));
    assert(!memcmp(&c, &before, sizeof c));
    enabled = false; ST8(screen + 0x38e4, 0);
    edge(&c, QUICK_DROP_BUTTON); assert(requests == 1 && !quick_drop_hint_state());
    enabled = true; available = false;
    edge(&c, QUICK_DROP_BUTTON); assert(requests == 1);
    quick_drop_reset(); assert(!menu_screen && !menu_buttons && !atomic_load(&drop_hint));
    puts("ok: held-Y entry, one request per edge, exact locked identity, CPU preservation, Back priority and disabled guards");
}
