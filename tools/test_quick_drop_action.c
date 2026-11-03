// Mock native functions: verify in-menu deletion, refresh, identity checks
// and exactly-once dispatch while the D-pad remains held.
#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include "../runtime/quick_drop_action.c"

static bool owned_card;
bool quick_replace_card_drop_allowed(void) { return owned_card; }
Settings g_settings;
GameSites g_site;
const char *g_game_version = "1.9.0";
static uint64_t buttons;
static int depth, discarded, original_calls, tag_calls, lookup_calls, missing;
static int spawned;
static int sweeps;
static int widget_count, widget_cursor;
static bool sword_special;
static bool lookup_ok = true, tag_important, tag_notdrop, trash, equip_ban, event, popup, player_ban;
static uint64_t inventory[2][2];
static int counts[2];
static HostFn installed;
static const uint64_t screen = GUEST_LO + 0x2100000, mgr = GUEST_LO + 0x2200000;
static const uint64_t item = GUEST_LO + 0x2280000, stack = GUEST_LO + 0x2300000;
static const uint64_t actor = GUEST_LO + 0x2310000, bg = GUEST_LO + 0x2320000;
static const uint64_t info = GUEST_LO + 0x2400000, player_info = GUEST_LO + 0x2410000;
static const uint64_t player = GUEST_LO + 0x2420000;

uint64_t input_buttons_now(void) { return buttons; }
static void original(Cpu *c) { (void)c; original_calls++; }
GuestFn dispatch_lookup(uint64_t address) {
    if (missing && address == 0x700108e7a0ull) return NULL;
    return original;
}
void hle_override(uint64_t addr, HostFn fn, const char *name) {
    assert(addr == 0x7000e2835cull && name);
    installed = fn;
}
void guest_call(Cpu *c, uint64_t fn) {
    assert(c->x[3] == 0);
    assert(c->sp == stack - 0x100);
    if (fn == DROP_LOCK) { assert(c->x[0] == mgr + 0x28); depth++; c->x[0] = 0; }
    else if (fn == DROP_UNLOCK) { assert(c->x[0] == mgr + 0x28); assert(depth == 1); depth--; c->x[0] = 0; }
    else if (fn == DROP_COUNT) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 2);
        c->x[0] = counts[c->x[1]];
    } else if (fn == DROP_GET) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 2 && c->x[2] < 2);
        c->x[0] = inventory[c->x[1]][c->x[2]];
    } else if (fn == 0x7000d388b0ull) { assert(c->x[0] == 0); c->x[0] = trash; }
    else if (fn == 0x7000d35b80ull) { assert(c->x[0] == 0); c->x[0] = equip_ban; }
    else if (fn == 0x7000d1499cull) c->x[0] = event;
    else if (fn == 0x7000e7e750ull) c->x[0] = popup;
    else if (fn == 0x7000cc6fd0ull) { assert(c->x[0] == player_info); c->x[0] = player; }
    else if (fn == 0x7000cc0334ull) { assert(c->x[0] == player); c->x[0] = player_ban; }
    else if (fn == 0x700108dc0cull) {
        assert(c->x[0] == info && c->x[1] == stack - 0xf0 && c->x[2] == stack - 0xd0);
        assert(!strcmp((char *)(uintptr_t)c->x[2], (char *)(uintptr_t)actor));
        assert(c->x[2] >= GUEST_LO && c->x[2] < GUEST_HI);
        lookup_calls++;
        c->x[0] = lookup_ok;
    } else if (fn == 0x700108e7a0ull) {
        assert(c->x[0] == info && c->x[1] == stack - 0xf0);
        assert(c->x[2] == 0x4850af43u || c->x[2] == 0x4873f474u);
        tag_calls++;
        c->x[0] = c->x[2] == 0x4850af43u ? tag_important : tag_notdrop;
    } else if (fn == 0x7000e890b0ull) {
        assert(depth == 1 && c->x[0] == item && c->x[1] == stack - 0xf0);
        assert((uint32_t)c->v[0] == 0x3f4ccccd && (uint32_t)c->v[1] == 0xbf4ccccd);
        assert(LD64(c->x[1]) == 0x7001bc32c8ull && LD64(c->x[1] + 8) == 0x7001639fc5ull);
        spawned++;
        c->x[0] = 1;
    } else if (fn == 0x7000d97f68ull) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] == 0);
        assert(!LD8(item + 0x25));
        sweeps++;
        for (int tab = 0; tab < 2; tab++) {
            int kept = 0;
            for (int slot = 0; slot < counts[tab]; slot++)
                if (inventory[tab][slot] != item) inventory[tab][kept++] = inventory[tab][slot];
            counts[tab] = kept;
            while (kept < 2) inventory[tab][kept++] = 0;
        }
        c->x[0] = 0;
    } else if (fn == 0x7000e27c2cull) {
        assert(depth == 1 && c->x[0] == screen);
        ST32(screen + 0x3880, counts[0]);
        ST64(screen + 0x37c8, inventory[0][0]);
        ST32(screen + 0x3878, -1);
    } else if (fn == 0x7000e28070ull) {
        // Native e28070: ordinary swords, shields and bows have an
        // unequip slot; special sword mode has no extra slot.
        int extra = LD32(screen + 0x3738) != 0 || !sword_special ||
            (int32_t)LD32(screen + 0x387c) == -1;
        for (int i = 0; i < 21; i++) {
            int index = i - extra;
            ST32(screen + 0x3884 + i * 4,
                 index >= 0 && index < (int)LD32(screen + 0x3880) ? index : -1);
        }
    } else if (fn == 0x7000d665a4ull) {
        assert(depth == 1);
        widget_cursor = c->x[1]; widget_count = c->x[2];
    } else if (fn == 0x7000e278c0ull) {
        assert(depth == 1);
    } else if (fn == 0x7000d9784cull) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 2 && c->x[2] < 2);
        assert(inventory[c->x[1]][c->x[2]] == item);
        discarded++;
        ST8(item + 0x25, 0);
        ST32(item + 0x20, 0);
        c->x[0] = 0;
    } else { fprintf(stderr, "unexpected native call %llx\n", (unsigned long long)fn); assert(0); }
    // Cloned Cpu must isolate all guest register clobbers from interrupted code.
    c->x[4] = 0xdead;
    c->nzcv = 0xbeef;
}

static Cpu cpu(void) {
    Cpu c = {0}; c.x[0] = screen; c.x[4] = 44; c.nzcv = 55; c.sp = stack; return c;
}
static void setup(void) {
    memset(&pending_drop, 0, sizeof pending_drop);
    memset((void *)(uintptr_t)screen, 0, 0x3900);
    memset((void *)(uintptr_t)mgr, 0, 0x45000);
    memset((void *)(uintptr_t)item, 0, 0x100);
    memset((void *)(uintptr_t)player, 0, 0x1000);
    g_site.player_info = 0x7001e13ed0ull;
    ST64(g_site.player_info, player_info);
    ST64(0x7001e39450ull, info);
    ST64(item + 0x30, actor);
    strcpy((char *)(uintptr_t)actor, "Weapon_Sword_001");
    ST32(item + 0x18, 0); ST32(item + 0x20, 15); ST8(item + 0x25, 1);
    ST64(screen + 0x3640, mgr); ST64(screen + 0x37b8, bg);
    ST32(screen + 0x3878, 1); ST32(screen + 0x387c, 0); ST32(screen + 0x3738, 0);
    ST32(bg + 0x20, 0); ST32(mgr + 0x44498, 2);
    memset(inventory, 0, sizeof inventory); inventory[0][0] = item;
    counts[0] = 1; counts[1] = 0;
    buttons = 0; depth = discarded = original_calls = tag_calls = lookup_calls = 0;
    spawned = 0;
    sweeps = 0; widget_count = widget_cursor = -1; sword_special = false;
    trash = equip_ban = event = popup = player_ban = tag_important = tag_notdrop = false;
    lookup_ok = true;
}
static void request(Cpu *c) {
    Cpu before = *c;
    quick_drop_action_request(c, screen, mgr, item, 0, 0, (char *)(uintptr_t)actor);
    assert(!memcmp(c, &before, sizeof before)); assert(depth == 0);
}
static void hidden(void) { /* Requests now execute in the open menu. */ }
static void reject_case(Cpu *c) { request(c); assert(!pending_drop.item && discarded == 0); }

int main(void) {
    void *mapped = mmap((void *)(uintptr_t)GUEST_LO, 0x4000000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mapped == (void *)(uintptr_t)GUEST_LO);
    g_settings.quick_inventory = true;
    setenv("SAVAGEWIND_QUICK_DROP", "1", 1);
    missing = 1; assert(!quick_drop_action_init() && !installed);
    missing = 0; assert(quick_drop_action_init() && drop_ready && !installed);
    assert(quick_drop_action_init());
    setup(); Cpu c = cpu(); request(&c);
    assert(!pending_drop.item && !discarded);
    assert(LD8(screen + 0x38e4) != 2);
    setenv("SAVAGEWIND_QUICK_DROP_EXECUTE", "1", 1);
    request(&c);
    assert(pending_drop.item == item && !discarded && tag_calls == 2 && lookup_calls == 1);
    assert(LD8(screen + 0x38e4) != 2);
    // Held D-pad and visible background no longer block the action.
    ST32(bg + 0x20, 0x3f800000); buttons = 1 << 14;
    Cpu before = c; quick_drop_action_close(&c);
    assert(discarded == 1 && sweeps == 1 && !pending_drop.item && depth == 0 && !memcmp(&c, &before, sizeof c));
    assert(counts[0] == 0 && inventory[0][0] == 0);
    quick_drop_action_close(&c); assert(discarded == 1);
    assert(LD32(screen + 0x3880) == 0 && (int32_t)LD32(screen + 0x387c) == -1);
    assert(LD8(screen + 0x38e4) != 2 && LD32(screen + 0x3738) == 0);
    // Removing the first of two entries retains a usable cursor on its neighbor.
    setup(); inventory[0][1] = item + 0x100; counts[0] = 2;
    request(&c); quick_drop_action_close(&c);
    assert(discarded == 1 && LD32(screen + 0x3880) == 1);
    assert(LD32(screen + 0x387c) == 0 && LD64(screen + 0x37c8) == item + 0x100);
    assert(LD8(screen + 0x38e4) != 2);
    // One remaining sword plus the unequip slot must both remain visible.
    assert(widget_count == 2 && widget_cursor == 1);
    for (int category = 0; category <= 2; category++) {
        setup(); ST32(screen + 0x3738, category);
        inventory[0][1] = item + 0x100; counts[0] = 2;
        request(&c); quick_drop_action_close(&c);
        assert(discarded == 1 && counts[0] == 1);
        assert(widget_count == 2 && widget_cursor == 1);
    }
    setup(); sword_special = true;
    inventory[0][1] = item + 0x100; counts[0] = 2;
    request(&c); quick_drop_action_close(&c);
    assert(widget_count == 1 && widget_cursor == 0);
    // Resolve by identity again if inventory ordering changed before execution.
    setup(); request(&c); inventory[0][0] = 0; counts[0] = 0;
    inventory[1][0] = item; counts[1] = 1; hidden(); quick_drop_action_close(&c); assert(discarded == 1);
    // Stale/mutated requests cancel without destructive fallback.
    setup(); request(&c); ST32(item + 0x20, 14); hidden(); quick_drop_action_close(&c); assert(!discarded && !pending_drop.item);
    setup(); request(&c); inventory[0][0] = 0; hidden(); quick_drop_action_close(&c); assert(!discarded && !pending_drop.item);
    setup(); request(&c); ST8(item + 0x24, 1); hidden(); quick_drop_action_close(&c); assert(!discarded);
    setup(); request(&c); trash = true; hidden(); quick_drop_action_close(&c); assert(!discarded);
    // Enqueue guards, including every native restriction and actor lookup failure.
    setup(); ST8(item + 0x24, 1); reject_case(&c);
    setup(); ST8(item + 0x25, 0); reject_case(&c);
    setup(); ST32(item + 0x20, 0); reject_case(&c);
    setup(); ST32(item + 0x18, 2); reject_case(&c);
    setup(); strcpy((char *)(uintptr_t)actor, "Weapon_Sword_070"); reject_case(&c);
    setup(); strcpy((char *)(uintptr_t)actor, "Weapon_Sword_502"); reject_case(&c);
    setup(); trash = true; reject_case(&c);
    setup(); equip_ban = true; reject_case(&c);
    setup(); event = true; reject_case(&c);
    setup(); popup = true; reject_case(&c);
    setup(); popup = true; owned_card = true;
    request(&c); quick_drop_action_close(&c); assert(discarded == 1);
    setup(); popup = true; event = true; reject_case(&c);
    owned_card = false;
    setup(); player_ban = true; reject_case(&c);
    setup(); ST8(player + 0xcf8, 4); reject_case(&c);
    setup(); lookup_ok = false; reject_case(&c); assert(!tag_calls);
    setup(); tag_important = true; reject_case(&c);
    setup(); tag_notdrop = true; reject_case(&c);
    setup(); request(&c); Cpu other = c; other.x[0] = screen + 0x10000;
    hidden(); quick_drop_action_close(&other); assert(!discarded && pending_drop.item);
    quick_drop_action_close(&other); assert(LD8(other.x[0] + 0x38e4) == 0);
    quick_drop_action_reset(); assert(!pending_drop.item);
    setup();
    setenv("SAVAGEWIND_QUICK_DROP_SPAWN_ONLY", "1", 1);
    request(&c); hidden(); quick_drop_action_close(&c);
    assert(spawned == 1 && discarded == 0 && sweeps == 0 && LD8(item + 0x25) == 1 && LD32(item + 0x20) == 15);
    quick_drop_action_close(&c); assert(spawned == 1);
    unsetenv("SAVAGEWIND_QUICK_DROP_SPAWN_ONLY");
    // The Gameplay switch executes without diagnostic environment variables.
    unsetenv("SAVAGEWIND_QUICK_DROP");
    unsetenv("SAVAGEWIND_QUICK_DROP_EXECUTE");
    unsetenv("SAVAGEWIND_QUICK_DROP_SPAWN_ONLY");
    setup(); request(&c); assert(pending_drop.item == item);
    quick_drop_action_close(&c); assert(discarded == 1);
    // Turning the switch off also cancels an already queued request.
    setup(); request(&c); assert(pending_drop.item == item);
    g_settings.quick_inventory = false;
    quick_drop_action_close(&c); assert(!pending_drop.item && !discarded);
    request(&c); assert(!pending_drop.item && !discarded);
    puts("quick-drop action tests passed");
    return 0;
}
