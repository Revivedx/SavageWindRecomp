// Mock native functions: verify in-menu deletion, refresh, identity checks
// and exactly-once dispatch while the D-pad remains held.
#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include "../runtime/quick_drop_action.c"

Settings g_settings;
GameSites g_site;
const char *g_game_version = "1.9.0";
static uint64_t buttons;
static int depth, discarded, original_calls, tag_calls, lookup_calls, missing;
static int actor_drops, schedules, snapshots;
static uint64_t saved_cache[4];
static uint64_t native_equipment, current_player, frame, consumed;
static bool owner_attached, weak_links, retain_failure, interleave_drop, protect_after_discard, consume_scheduler_immediately, snapshot_failure;
static int sweeps;
static int widget_count, widget_cursor;
static bool sword_special;
static bool lookup_ok = true, tag_important, tag_notdrop, trash, equip_ban, event, popup, player_ban;
static uint64_t inventory[2][2];
static int counts[2];
static const uint64_t screen = GUEST_LO + 0x2100000, mgr = GUEST_LO + 0x2200000;
static const uint64_t item = GUEST_LO + 0x2280000, stack = GUEST_LO + 0x2300000;
static const uint64_t actor = GUEST_LO + 0x2310000, bg = GUEST_LO + 0x2320000;
static const uint64_t info = GUEST_LO + 0x2400000, player_info = GUEST_LO + 0x2410000;
static const uint64_t player = GUEST_LO + 0x2420000;
static const uint64_t links = GUEST_LO + 0x2430000, equipment = GUEST_LO + 0x2440000, equipment_vt = GUEST_LO + 0x2450000;
static const uint64_t equipment_block = GUEST_LO + 0x2460000, player_block = GUEST_LO + 0x2470000;
static const uint64_t ui_manager = GUEST_LO + 0x2500000, ui_input = GUEST_LO + 0x2570000;

void *hle_alloc(uint64_t size) { assert(size == 0x50); return (void *)(uintptr_t)links; }
void input_consume_buttons(uint64_t mask) { consumed |= mask; }
uint64_t renderer_present_count(void) { return frame; }
static void actor_original(Cpu *c) {
    assert(c->x[0] == equipment); actor_drops++;
    ST8(equipment + 0x920, 2); ST8(equipment + 0x934, 1);
    c->x[0] = 1; c->pc = 0x1234; c->v[0] = 0x4567;
}

static void observed_actor(Cpu *c) {
    uint64_t actor = c->x[0];
    uint64_t token = rt_quick_drop_actor_begin(actor, g_site.quick_drop_actor_method);
    actor_original(c);
    rt_quick_drop_actor_end(token, actor, c->x[0]);
}

static void consume_native_drop(Cpu *context) {
    assert(LD8(player + drop_pending_offset(equipped_drop.actor_category)));
    Cpu native = *context; native.x[0] = equipment; native.x[30] = 0x7001223f54ull;
    observed_actor(&native);
    native_equipment = 0;
    ST8(player + drop_pending_offset(equipped_drop.actor_category), 0);
    ST8(equipment + 0x920, 0xff); ST8(equipment + 0x934, 0);
    owner_attached = false;
}
uint64_t input_buttons_now(void) { return buttons; }
static void original(Cpu *c) { (void)c; original_calls++; }
GuestFn dispatch_lookup(uint64_t address) {
    if (missing && address == 0x700108e7a0ull) return NULL;
    return address == 0x70007acaa0ull ? actor_original : original;
}
void guest_call(Cpu *c, uint64_t fn) {
    assert(c->x[3] == 0);
    assert(c->sp == stack - 0x100);
    if (fn == g_site.pouch_lock) { assert(c->x[0] == mgr + 0x28 || c->x[0] == player + 0xeb8 || c->x[0] == player + 0x13a0 || c->x[0] == player + 0x13e8 || c->x[0] == player + 0x1430); depth++; c->x[0] = 0; }
    else if (fn == g_site.pouch_unlock) { assert(c->x[0] == mgr + 0x28 || c->x[0] == player + 0xeb8 || c->x[0] == player + 0x13a0 || c->x[0] == player + 0x13e8 || c->x[0] == player + 0x1430); assert(depth > 0); depth--; c->x[0] = 0; }
    else if (fn == g_site.pouch_page_count) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 2);
        c->x[0] = counts[c->x[1]];
    } else if (fn == g_site.pouch_page_item) {
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 2 && c->x[2] < 2);
        c->x[0] = inventory[c->x[1]][c->x[2]];
    } else if (fn == 0x7000d388b0ull) { assert(c->x[0] == 0); c->x[0] = trash; }
    else if (fn == 0x7000d35b80ull) { assert(c->x[0] == 0); c->x[0] = equip_ban; }
    else if (fn == 0x7000d1499cull) c->x[0] = event;
    else if (fn == 0x7000e7e750ull) c->x[0] = popup;
    else if (fn == 0x7000cc6fd0ull) { assert(c->x[0] == player_info); c->x[0] = current_player; }
    else if (fn == 0x7000d940c4ull) { assert(c->x[0] == mgr); c->x[0] = equipment_vt + 0x600; }
    else if (fn == 0x7000d9f4a8ull) {
        assert(depth == 1 && c->x[0] == mgr && LD8(item + 0x25));
        snapshots++;
        for (unsigned i = 0; i < 4; i++) { saved_cache[i] = LD64(mgr + 0x447e0 + i * 8); ST64(mgr + 0x447e0 + i * 8, 0); }
        // Native snapshot is derived from flags in the locked live pouch,
        // independently of the cache values that happened to exist on entry.
        for (unsigned tab = 0; tab < 2; tab++) for (int slot = 0; slot < counts[tab]; slot++) {
            uint64_t candidate = inventory[tab][slot];
            if (!candidate) continue;
            unsigned type = LD32(candidate + 0x18);
            if (type < 4 && LD8(candidate + 0x24) && !snapshot_failure)
                ST64(mgr + 0x447e0 + type * 8, candidate);
        }
    }
    else if (fn == 0x7000cbe480ull) {
        assert(discarded == 1 && quick_drop_action_busy() && snapshots == 1);
        for (unsigned i = 0; i < 4; i++) assert(LD64(mgr + 0x447e0 + i * 8) == saved_cache[i]);
        assert(LD8(player + drop_pending_offset(equipped_drop.actor_category)) || actor_drops == 1);
        assert(quick_drop_action_filter_buttons(UINT64_MAX) == UINT64_C(0xff0000));
        assert(c->x[0] == player && c->x[1] == equipment_vt + 0x600 && c->x[2] == 0xffffffffu);
        schedules++; ST8(player + 0x1120 + equipped_drop.actor_category, 1);
        ST32(player + 0xc98, LD32(player + 0xc98) | 0x20);
        if (consume_scheduler_immediately) {
            consume_native_drop(c);
            ST8(player + 0x1120 + equipped_drop.actor_category, 0);
        }
    }
    else if (fn == 0x7000570128ull) { assert(c->x[0] == player && c->x[1] <= 2); c->x[0] = native_equipment; }
    else if (fn == 0x7001517e60ull) {
        uint64_t out = c->x[0], target = c->x[1], block = LD64(target + 0x68);
        assert((out == links || out == links + 0x10) && c->x[2] == 1);
        if (retain_failure) { c->x[0] = 0; return; }
        ST64(out, block); ST32(out + 8, LD32(target + 0x60)); ST8(out + 0xc, 1);
        ST32(block + 0x50, LD32(block + 0x50) + 1); c->x[0] = 1;
    }
    else if (fn == 0x7001517abcull) {
        uint64_t index = c->x[0], out = c->x[1], block = LD64(index);
        assert((index == links || index == links + 0x10) && c->x[2] == 0);
        assert(out == links + 0x20 || out == links + 0x38);
        uint64_t target = LD32(index + 8) == LD32(block + 0x40) ? LD64(block + 0x48) : 0;
        ST8(out, target && !weak_links); ST64(out + 8, target); c->x[0] = target;
    }
    else if (fn == 0x7001517f8cull) {
        uint64_t index = c->x[0]; assert(index == links || index == links + 0x10);
        if (LD8(index + 0xc)) { uint64_t block = LD64(index); assert(LD32(block + 0x50)); ST32(block + 0x50, LD32(block + 0x50) - 1); }
        ST8(index + 0xc, 0); ST32(index + 8, UINT32_MAX);
    }
    else if (fn == 0x7001126464ull) { ST8(c->x[0], 0); ST64(c->x[0] + 8, 0); }
    else if (fn == 0x7001517e0cull) { assert(c->x[0] == equipment + 0x938 || c->x[0] == equipment + 0x958); c->x[0] = owner_attached; }
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
        if (native_equipment) {
            assert(quick_drop_action_busy() && schedules == 0 && snapshots == 1);
            assert(LD64(mgr + 0x447e0 + (uint64_t)LD32(item + 0x18) * 8) == item);
            assert(quick_drop_action_filter_buttons(UINT64_MAX) == UINT64_C(0xff0000));
        }
        discarded++;
        if (native_equipment) ST8(player + drop_pending_offset(equipped_drop.actor_category), 1);
        ST8(item + 0x25, 0);
        ST32(item + 0x20, 0);
        if (interleave_drop) {
            // Another guest worker consumes the established drop before the
            // scheduler runs. Reversed ordering would unequip without intent.
            assert(schedules == 0 && LD8(player + drop_pending_offset(equipped_drop.actor_category)));
            consume_native_drop(c);
        }
        if (protect_after_discard) assert(!mprotect((void *)(uintptr_t)item, 0x1000, PROT_NONE));
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
    quick_drop_action_reset();
    memset((void *)(uintptr_t)screen, 0, 0x3900);
    memset((void *)(uintptr_t)mgr, 0, 0x45000);
    memset((void *)(uintptr_t)item, 0, 0x100);
    memset((void *)(uintptr_t)player, 0, 0x2000);
    memset((void *)(uintptr_t)equipment, 0, 0x1000);
    memset((void *)(uintptr_t)links, 0, 0x100);
    memset((void *)(uintptr_t)equipment_block, 0, 0x100);
    memset((void *)(uintptr_t)player_block, 0, 0x100);
    ST64(equipment + 0x68, equipment_block); ST32(equipment + 0x60, 42);
    ST32(equipment_block + 0x40, 42); ST64(equipment_block + 0x48, equipment);
    ST64(player + 0x68, player_block); ST32(player + 0x60, 73);
    ST32(player_block + 0x40, 73); ST64(player_block + 0x48, player);
    ST8(player + 0x64, 1);
    ST8(equipment + 0x64, 1); ST8(equipment + 0x920, 0);
    ST64(equipment + 0x190, equipment + 0x1000);
    ST64(equipment, equipment_vt); ST64(equipment_vt + 0x588, 0x70007acaa0ull);
    ST64(equipment + 0x10, actor);
    memset((void *)(uintptr_t)ui_manager, 0, 0x65000);
    memset((void *)(uintptr_t)ui_input, 0, 0x120);
    ST64(ui_manager + 0x649f0, ui_input);
    current_player = player; native_equipment = 0; frame = 1; consumed = 0;
    owner_attached = true; weak_links = retain_failure = interleave_drop = protect_after_discard = consume_scheduler_immediately = snapshot_failure = false; actor_drops = schedules = snapshots = 0;
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
    sweeps = 0; widget_count = widget_cursor = -1; sword_special = false;
    trash = equip_ban = event = popup = player_ban = tag_important = tag_notdrop = false;
    lookup_ok = true;
    assert(quick_drop_action_init());
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
    g_settings.quick_drop = true;
    g_site.pouch_page_count = 0x7000d970a8ull;
    g_site.pouch_page_item = 0x7000d992acull;
    g_site.quick_sort_modal = 0x7000e7e750ull;
    g_site.player_drop_forbidden = 0x7000cc0334ull;
    g_site.actor_info_lookup = 0x700108dc0cull;
    g_site.actor_info_has_tag = 0x700108e7a0ull;
    g_site.pouch_discard = 0x7000d9784cull;
    g_site.player_equipment = 0x7000570128ull;
    g_site.pouch_default_name = 0x7000d940c4ull;
    g_site.player_schedule_equip = 0x7000cbe480ull;
    g_site.pouch_equipped_snapshot = 0x7000d9f4a8ull;
    g_site.actor_index_assign = 0x7001517e60ull;
    g_site.actor_index_resolve = 0x7001517abcull;
    g_site.actor_index_release = 0x7001517f8cull;
    g_site.actor_link_release = 0x7001126464ull;
    g_site.actor_link_alive = 0x7001517e0cull;
    g_site.pouch_cleanup = 0x7000d97f68ull;
    g_site.quick_sort_refill = 0x7000e27c2cull;
    g_site.quick_sort_mapping = 0x7000e28070ull;
    g_site.quick_sort_cursor = 0x7000d665a4ull;
    g_site.quick_sort_labels = 0x7000e278c0ull;
    g_site.pouch_lock = 0x7000eaff08ull;
    g_site.pouch_unlock = 0x7000eaff18ull;
    g_site.player_actor = 0x7000cc6fd0ull;
    g_site.quick_sort_event = 0x7000d1499cull;
    g_site.quick_sort_player_busy = 0x7000d35b80ull;
    g_site.quick_drop_trash_prohibited = 0x7000d388b0ull;
    g_site.quick_drop_actor_method = 0x70007acaa0ull;
    g_site.actor_info = 0x7001e39450ull;
    missing = 1; assert(!quick_drop_action_init());
    missing = 0; assert(quick_drop_action_init() && drop_ready);
    assert(quick_drop_action_init());
    setup(); Cpu c = cpu();
    request(&c);
    assert(pending_drop.item == item && !discarded && tag_calls == 2 && lookup_calls == 1);
    assert(LD8(screen + 0x38e4) != 2);
    // Held D-pad and visible background no longer block the action.
    ST32(bg + 0x20, 0x3f800000); buttons = 1 << 14;
    Cpu before = c; quick_drop_action_close(&c);
    assert(discarded == 1 && sweeps == 1 && !snapshots && !pending_drop.item && depth == 0 && !memcmp(&c, &before, sizeof c));
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
    // The equipment cache can change after preflight, independently of the
    // item's flag. Staged unequipped requests cannot become equipped drops.
    setup(); request(&c); ST64(mgr + 0x447e0, item); hidden(); quick_drop_action_close(&c);
    assert(!discarded && !pending_drop.item);
    setup(); request(&c); trash = true; hidden(); quick_drop_action_close(&c); assert(!discarded);
    // Enqueue guards, including every native restriction and actor lookup failure.
    setup(); ST8(item + 0x24, 1); request(&c); quick_drop_action_close(&c);
    assert(!discarded && !quick_drop_action_busy()); // empty snapshot does not prove a live equipped actor
    setup(); ST8(item + 0x25, 0); reject_case(&c);
    setup(); ST32(item + 0x20, 0); reject_case(&c);
    setup(); ST32(item + 0x18, 2); reject_case(&c);
    setup(); strcpy((char *)(uintptr_t)actor, "Weapon_Sword_070"); reject_case(&c);
    setup(); strcpy((char *)(uintptr_t)actor, "Weapon_Sword_502"); reject_case(&c);
    setup(); trash = true; reject_case(&c);
    setup(); equip_ban = true; reject_case(&c);
    setup(); event = true; reject_case(&c);
    setup(); popup = true; reject_case(&c);
    setup(); popup = true; event = true; reject_case(&c);
    setup(); player_ban = true; reject_case(&c);
    setup(); ST8(player + 0xcf8, 4); reject_case(&c);
    setup(); lookup_ok = false; reject_case(&c); assert(!tag_calls);
    setup(); tag_important = true; reject_case(&c);
    setup(); tag_notdrop = true; reject_case(&c);
    setup(); request(&c); Cpu other = c; other.x[0] = screen + 0x10000;
    hidden(); quick_drop_action_close(&other); assert(!discarded && pending_drop.item);
    quick_drop_action_close(&other); assert(LD8(other.x[0] + 0x38e4) == 0);
    quick_drop_action_reset(); assert(!pending_drop.item);
    // An interrupted menu must not execute an old request on a later visit.
    setup(); request(&c); ST8(screen + 0x38e4, 2);
    quick_drop_action_close(&c); assert(!pending_drop.item && !discarded);
    ST8(screen + 0x38e4, 0); quick_drop_action_close(&c); assert(!discarded);
    setup(); request(&c); ST32(screen + 0x3738, 3);
    quick_drop_action_close(&c); assert(!pending_drop.item && !discarded);
    setup(); request(&c); ST32(screen + 0x3738, 1);
    quick_drop_action_close(&c); assert(!pending_drop.item && !discarded);
    setup(); request(&c); Cpu bad_stack = c; bad_stack.sp |= 1;
    quick_drop_action_close(&bad_stack); assert(!pending_drop.item && !discarded);
    setup(); request(&c); assert(pending_drop.item == item);
    quick_drop_action_close(&c); assert(discarded == 1);
    // Turning the switch off also cancels an already queued request.
    setup(); request(&c); assert(pending_drop.item == item);
    g_settings.quick_drop = false;
    quick_drop_action_close(&c); assert(!pending_drop.item && !discarded);
    request(&c); assert(!pending_drop.item && !discarded);

    g_settings.quick_drop = true;
    // Native consumer cannot advance under the selector. Each category closes
    // first, issues one discard and waits for actual actor detach/placement.
    for (int job_context = 0; job_context < 2; job_context++) for (int category = 0; category < 3; category++) {
        setup();
        weak_links = job_context;
        int type = category == 0 ? 0 : category == 1 ? 3 : 1;
        strcpy((char *)(uintptr_t)actor, category == 0 ? "Weapon_Sword_001" : category == 1 ? "Weapon_Shield_001" : "Weapon_Bow_001");
        ST32(item + 0x18, type); ST32(screen + 0x3738, category);
        ST8(item + 0x24, 1);
        for (unsigned i = 0; i < 4; i++) if (i != (unsigned)type) ST64(mgr + 0x447e0 + i * 8, equipment + 0x100 + i * 8);
        assert(!LD64(mgr + 0x447e0 + type * 8)); // native quick menu leaves this snapshot empty
        native_equipment = equipment;
        buttons = 1 | (1 << 3) | (1 << 14);
        request(&c); assert(quick_drop_action_result() == QUICK_DROP_PENDING);
        before = c; quick_drop_action_close(&c);
        assert(!memcmp(&c, &before, sizeof c) && discarded == 1 && !sweeps);
        assert(quick_drop_action_busy() && LD8(screen + 0x38e4) == 2 && schedules == 1 && snapshots == 1);
        for (unsigned i = 0; i < 4; i++) assert(LD64(mgr + 0x447e0 + i * 8) == saved_cache[i]);
        assert(LD32(player + 0xc98) & 0x20);
        assert(LD32(screen + 0x387c) == LD32(screen + 0x3878));
        assert((consumed & buttons) == buttons);
        assert(LD8(player + drop_pending_offset(category)));
        quick_drop_action_close(&c); request(&c); assert(discarded == 1);
        // No runtime item dereference is permitted after native invalidation.
        assert(!mprotect((void *)(uintptr_t)item, 0x1000, PROT_NONE));
        quick_drop_action_tick(&c); assert(!sweeps && quick_drop_action_busy());
        // A cleared flag alone does not prove consumer work or world placement.
        ST8(player + drop_pending_offset(category), 0);
        quick_drop_action_tick(&c); assert(!sweeps && quick_drop_action_busy()); // getter still holds outgoing actor
        native_equipment = 0;
        ST8(equipment + 0x920, 0xff);
        quick_drop_action_tick(&c); assert(!sweeps && quick_drop_action_busy());
        // A foreign actor, method, result or transaction cannot acknowledge
        // this drop merely because the same virtual callback ran.
        uint64_t token = rt_quick_drop_actor_begin(equipment, g_site.quick_drop_actor_method);
        assert(token && !rt_quick_drop_actor_begin(equipment, g_site.quick_drop_actor_method + 4));
        rt_quick_drop_actor_end(token + 1, equipment, 1);
        rt_quick_drop_actor_end(token, equipment + 0x100, 1);
        rt_quick_drop_actor_end(token, equipment, 0);
        assert(!atomic_load(&drop_actor_consumed));
        // Only the real matching virtual call records consumption.
        Cpu native = cpu(); native.x[0] = equipment; native.x[30] = 0x7001223f54ull;
        Cpu native_expected = native; actor_original(&native_expected); actor_drops = 0;
        observed_actor(&native); assert(actor_drops == 1 && !memcmp(&native, &native_expected, sizeof native));
        quick_drop_action_tick(&c); assert(!sweeps && quick_drop_action_busy());
        ST8(equipment + 0x934, 0); ST8(equipment + 0x920, 0xff);
        quick_drop_action_tick(&c); assert(!sweeps); // owner links still attached
        owner_attached = false;
        quick_drop_action_tick(&c); assert(!sweeps); // queued default update still pending
        ST8(player + 0x1120 + category, 0);
        assert(!mprotect((void *)(uintptr_t)item, 0x1000, PROT_READ | PROT_WRITE));
        quick_drop_action_tick(&c);
        assert(sweeps == 1 && discarded == 1 && !quick_drop_action_busy());
        assert(quick_drop_action_result() == QUICK_DROP_COMPLETE);
        quick_drop_action_tick(&c); assert(sweeps == 1 && actor_drops == 1);
    }
    // A conflicting populated category snapshot and duplicate equipped flags
    // reject rather than using a rebuilt snapshot to hide ambiguous identity.
    setup(); ST8(item + 0x24, 1); native_equipment = equipment;
    ST64(mgr + 0x447e0, item + 0x100); reject_case(&c); assert(!snapshots);
    setup(); ST8(item + 0x24, 1); native_equipment = equipment;
    inventory[0][1] = item + 0x100; counts[0] = 2;
    ST32(item + 0x118, 0); ST8(item + 0x124, 1); ST8(item + 0x125, 1);
    request(&c); quick_drop_action_close(&c);
    assert(!discarded && !schedules && !snapshots && !quick_drop_action_busy());
    // Snapshot preparation can fail without changing any of its four prior
    // words, item validity, actor ownership, input gate, or inventory.
    setup(); ST8(item + 0x24, 1); native_equipment = equipment; snapshot_failure = true;
    for (unsigned i = 1; i < 4; i++) ST64(mgr + 0x447e0 + i * 8, equipment + i * 8);
    request(&c); quick_drop_action_close(&c);
    assert(snapshots == 1 && !discarded && !schedules && !quick_drop_action_busy());
    assert(LD8(item + 0x25) && LD8(item + 0x24) && !LD8(links + 0xc) && LD32(links + 8) == UINT32_MAX);
    for (unsigned i = 0; i < 4; i++) assert(LD64(mgr + 0x447e0 + i * 8) == saved_cache[i]);
    // Every cached native action/edge is cleared on admission and each UI
    // update, while a worker may interleave between discard and scheduling.
    setup(); ST8(item + 0x24, 1); ST64(mgr + 0x447e0, item); native_equipment = equipment;
    Cpu ui = cpu(); ui.x[0] = ui_manager;
    ST32(ui_input + 0x114, UINT32_MAX); ST32(ui_input + 8, UINT32_MAX);
    quick_drop_action_tick(&ui); // capture manager input before Menu::execute
    assert(LD32(ui_input + 0x114) == UINT32_MAX); // inactive driver leaves input alone
    interleave_drop = protect_after_discard = true;
    request(&c); quick_drop_action_close(&c);
    assert(discarded == 1 && schedules == 1 && actor_drops == 1 && !sweeps);
    assert(!LD32(ui_input + 0x114) && !LD32(ui_input + 8));
    assert(quick_drop_action_result() == QUICK_DROP_PENDING && quick_drop_action_busy());
    for (int poll = 0; poll < 7; poll++) {
        ST32(ui_input + 0x114, UINT32_MAX); ST32(ui_input + 8, UINT32_MAX);
        before = ui; quick_drop_action_tick(&ui);
        assert(!memcmp(&ui, &before, sizeof ui));
        assert(!LD32(ui_input + 0x114) && !LD32(ui_input + 8) && !sweeps);
        assert(quick_drop_action_filter_buttons(UINT64_MAX) == UINT64_C(0xff0000));
    }
    g_settings.quick_drop = false; frame += 700;
    quick_drop_action_tick(&ui);
    assert(quick_drop_action_busy() && quick_drop_action_result() == QUICK_DROP_PENDING && !sweeps);
    assert(quick_drop_action_filter_buttons(UINT64_MAX) == UINT64_C(0xff0000));
    ST8(player + 0x1120, 0);
    assert(!mprotect((void *)(uintptr_t)item, 0x1000, PROT_READ | PROT_WRITE));
    quick_drop_action_tick(&ui);
    assert(!quick_drop_action_busy() && quick_drop_action_result() == QUICK_DROP_COMPLETE);
    assert(sweeps == 1 && discarded == 1 && schedules == 1 && actor_drops == 1);
    assert(quick_drop_action_filter_buttons(UINT64_MAX) == UINT64_MAX);
    g_settings.quick_drop = true;
    // Scheduler may execute immediately too, with the source item page
    // inaccessible already. Completion still awaits the guest driver/sweep.
    setup(); ST8(item + 0x24, 1); ST64(mgr + 0x447e0, item); native_equipment = equipment;
    protect_after_discard = consume_scheduler_immediately = true;
    request(&c); quick_drop_action_close(&c);
    assert(discarded == 1 && schedules == 1 && actor_drops == 1 && !sweeps);
    assert(quick_drop_action_busy() && quick_drop_action_result() == QUICK_DROP_PENDING);
    assert(!mprotect((void *)(uintptr_t)item, 0x1000, PROT_READ | PROT_WRITE));
    quick_drop_action_tick(&ui);
    assert(!quick_drop_action_busy() && quick_drop_action_result() == QUICK_DROP_COMPLETE);
    assert(sweeps == 1 && discarded == 1 && actor_drops == 1);
    setup(); ST8(item + 0x24, 1); ST64(mgr + 0x447e0, item); native_equipment = equipment;
    weak_links = true; request(&c); quick_drop_action_close(&c);
    assert(discarded == 1 && schedules == 1 && quick_drop_action_busy());
    assert(LD32(equipment_block + 0x50) == 1 && LD32(player_block + 0x50) == 1);
    assert(!LD64(links + 0x28) && !LD64(links + 0x40)); // callback links released
    consume_native_drop(&c); ST8(player + 0x1120, 0); quick_drop_action_tick(&c);
    assert(!quick_drop_action_busy() && quick_drop_action_result() == QUICK_DROP_COMPLETE);
    assert(!LD32(equipment_block + 0x50) && !LD32(player_block + 0x50));
    setup(); ST8(item + 0x24, 1); native_equipment = equipment;
    retain_failure = true; request(&c); quick_drop_action_close(&c);
    assert(!discarded && !schedules && !quick_drop_action_busy());
    assert(quick_drop_action_result() == QUICK_DROP_REJECTED && !LD8(links + 0xc));
    // Generation mismatch must stop before stale actor fields are read.
    setup(); ST8(item + 0x24, 1); native_equipment = equipment;
    weak_links = true; request(&c); quick_drop_action_close(&c);
    ST32(equipment_block + 0x40, 43);
    assert(!mprotect((void *)(uintptr_t)equipment, 0x1000, PROT_NONE));
    quick_drop_action_tick(&c);
    assert(quick_drop_action_busy() && equipped_drop.recovery && !sweeps);
    assert(!mprotect((void *)(uintptr_t)equipment, 0x1000, PROT_READ | PROT_WRITE));
    setup(); ST8(item + 0x24, 1); ST64(mgr + 0x447e0, item); native_equipment = equipment;
    ST8(player + 0x1120, 1); request(&c); quick_drop_action_close(&c);
    assert(!discarded && !quick_drop_action_busy() && quick_drop_action_result() == QUICK_DROP_REJECTED);
    setup(); ST8(item + 0x24, 1); ST64(mgr + 0x447e0, item); native_equipment = equipment;
    ST8(player + 0x13e0, 1); request(&c); quick_drop_action_close(&c);
    assert(!discarded && !quick_drop_action_busy() && quick_drop_action_result() == QUICK_DROP_REJECTED);
    setup(); ST8(item + 0x24, 1); ST64(mgr + 0x447e0, item); native_equipment = equipment;
    request(&c); quick_drop_action_close(&c); assert(quick_drop_action_busy());
    current_player = player + 0x2000; frame += 700; quick_drop_action_tick(&c);
    assert(equipped_drop.recovery && quick_drop_action_busy() && discarded == 1 && !sweeps);
    assert(quick_drop_action_result() == QUICK_DROP_PENDING);
    g_settings.quick_drop = false; quick_drop_action_close(&c); quick_drop_action_tick(&c);
    assert(quick_drop_action_busy()); // settings/timeout cannot roll back invalidation
    quick_drop_action_reset(); assert(!quick_drop_action_busy());

    puts("quick-drop action tests passed");
    return 0;
}
