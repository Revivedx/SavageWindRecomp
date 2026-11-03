// Experimental receive replacement for the verified 1.9.0 build.
// TRACE=1 logs; QUICK_REPLACE=1 opens the selector; QUICK_REPLACE_EXECUTE=1
// also permits A to request the existing guarded native drop. Installed only
// after the quick-inventory NSO build check, before guest threads start.
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "hle.h"
#include "game_sites.h"
#include "ui/quick_inventory_layout.h"

static GuestFn replace_capacity_original;
static GuestFn replace_tick_original, replace_fill_original, replace_popup_original, replace_chest_original;
static GuestFn replace_item_popup_original;
static GuestFn replace_query_original, replace_full_info_original;
static GuestFn replace_ui_draw_original, replace_toast_update_original;
static GuestFn replace_pane_draw_original, replace_anim_advance_original;
static uint64_t replace_toast, replace_toast_tick;
static uint64_t replace_anim_present[2];
static bool replace_toast_foreground;
static bool replace_toast_started;
static unsigned replace_toast_draw_count;
static GuestFn replace_receive_original, replace_card_action_original, replace_card_close_original;
static uint64_t replace_card, replace_receive_ai, replace_award_until;
static bool replace_card_open, replace_card_requested, replace_card_drop, replace_receive_cancel;
static bool replace_chest_wait, replace_card_paused, replace_chest_previewed;
static uint8_t replace_card_pause_saved;
static uint64_t replace_preview_record, replace_preview_mgr;
static uint8_t replace_preview_saved[0xb8];
static unsigned replace_chest_wait_frames;
static char replace_award_name[97];
static uint64_t replace_event;
static bool replace_event_ready;
static unsigned replace_event_wait;
static _Atomic unsigned replace_trace_count;
static _Atomic unsigned replace_popup_count;
static int replace_category = -1, replace_frames;
static uint64_t replace_screen, replace_buttons;
static bool replace_closing, replace_test_started;
static uint64_t replace_chest, replace_resume_chest, replace_owner;
static char replace_incoming[97];
static uint64_t replace_handle, replace_retry_until;
static bool replace_confirmed;
extern uint64_t input_buttons_now(void);
extern void input_consume_buttons(uint64_t);
extern uint64_t renderer_present_count(void);
extern bool quick_drop_action_enabled(void);
extern void quick_drop_action_request(Cpu *, uint64_t, uint64_t, uint64_t, int, int, const char *);
extern void quick_drop_action_close(Cpu *);

bool quick_replace_active(void) { return replace_category >= 0 && replace_screen && !replace_closing; }
bool quick_replace_bottom_layout(void) { return quick_replace_active() && replace_card_open; }

RT_ON_RESET(quick_replace_reset) {
    atomic_store(&replace_trace_count, 0);
    atomic_store(&replace_popup_count, 0);
    replace_category = -1;
    replace_frames = 0;
    replace_screen = replace_buttons = 0;
    replace_closing = replace_test_started = false;
    replace_chest = replace_resume_chest = replace_owner = 0;
    memset(replace_incoming, 0, sizeof replace_incoming);
    replace_handle = replace_retry_until = 0;
    replace_confirmed = false;
    replace_card = replace_receive_ai = replace_award_until = 0;
    replace_card_open = replace_card_requested = replace_card_drop = replace_receive_cancel = false;
    replace_chest_wait = replace_card_paused = replace_chest_previewed = false;
    replace_chest_wait_frames = 0;
    replace_preview_record = replace_preview_mgr = 0;
    memset(replace_award_name, 0, sizeof replace_award_name);
    replace_event = 0;
    replace_event_ready = false;
    replace_event_wait = 0;
    replace_toast = replace_toast_tick = 0;
    memset(replace_anim_present, 0, sizeof replace_anim_present);
    replace_toast_foreground = false;
    replace_toast_started = false;
    replace_toast_draw_count = 0;
}

static bool replace_range(uint64_t address, uint64_t size) {
    return address >= GUEST_LO && address < GUEST_HI && size <= GUEST_HI - address;
}

static bool replace_experiment(void) {
    return quick_drop_action_enabled();
}

static int replace_name_category(const char *name) {
    if (!strncmp(name, "Weapon_Sword_", 13) || !strncmp(name, "Weapon_Lsword_", 14) ||
        !strncmp(name, "Weapon_Spear_", 13)) return 0;
    if (!strncmp(name, "Weapon_Shield_", 14)) return 1;
    if (!strncmp(name, "Weapon_Bow_", 11)) return 2;
    return -1;
}

static bool replace_string(uint64_t string, char name[97]) {
    if (!replace_range(string, 16)) return false;
    uint64_t address = LD64(string + 8);
    if (!replace_range(address, 97)) return false;
    for (int i = 0; i < 97; i++) {
        unsigned ch = LD8(address + i);
        if (!ch) { name[i] = 0; return i > 0; }
        if (ch < 32 || ch > 126) return false;
        name[i] = ch;
    }
    return false;
}

static uint64_t replace_call(Cpu *source, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t x2) {
    Cpu call = *source;
    call.x[0] = x0;
    call.x[1] = x1;
    call.x[2] = x2;
    guest_call(&call, fn);
    return call.x[0];
}

static bool replace_scratch(Cpu *c, Cpu *scratch) {
    if ((c->sp & 15) || c->sp < GUEST_LO + 0x100 || !replace_range(c->sp - 0x100, 0x100)) return false;
    *scratch = *c;
    scratch->sp -= 0x100;
    return true;
}

static void replace_start(int category) {
    replace_category = category;
    replace_frames = 0;
    replace_closing = false;
    replace_confirmed = false;
    replace_screen = 0;
    replace_toast = replace_toast_tick = 0;
    memset(replace_anim_present, 0, sizeof replace_anim_present);
    replace_toast_foreground = false;
    replace_toast_started = false;
    replace_toast_draw_count = 0;
    replace_buttons = input_buttons_now();
    input_consume_buttons(replace_buttons & 3);
    LOG("[quick-replace] selector request category=%d (no inventory mutation)\n", category);
}

static void replace_popup(Cpu *c) {
    unsigned trace = atomic_fetch_add(&replace_popup_count, 1);
    bool diagnostic = trace < 12;
    uint64_t actor = replace_range(c->x[0], 0x18) ? LD64(c->x[0] + 8) : 0;
    if (diagnostic)
        LOG("[quick-replace] popup entry x0=%" PRIx64 " x1=%" PRIx64 " error=%d actor=%" PRIx64 "\n",
            c->x[0], c->x[1], replace_range(c->x[1], 4) ? (int32_t)LD32(c->x[1]) : -1, actor);
    if (replace_experiment() && replace_category < 0 &&
        replace_range(c->x[1], 4) && LD32(c->x[1]) == 7 && replace_range(c->x[0], 0x18)) {
        char name[97];
        if (replace_range(actor, 0x18) && replace_string(actor + 8, name)) {
            int category = replace_name_category(name);
            LOG("[quick-replace] attempted pickup actor=%" PRIx64 " name=%s category=%d\n", actor, name, category);
            if (category >= 0) {
                if (!replace_handle) replace_handle = (uint64_t)(uintptr_t)hle_alloc(0x18);
                replace_call(c, 0x7001070ce0ull, replace_handle, c->x[0], 0);
                if (!LD64(replace_handle + 8)) { replace_popup_original(c); return; }
                memcpy(replace_incoming, name, sizeof name);
                replace_start(category);
                replace_popup_original(c);
                return;
            }
        } else if (diagnostic)
            LOG("[quick-replace] popup actor/name invalid actor=%" PRIx64 "\n", actor);
    }
    replace_popup_original(c);
}

static bool replace_capture_ground(Cpu *c, uint64_t string) {
    char incoming[97], target_name[97];
    Cpu scratch;
    if (!replace_string(string, incoming) || replace_name_category(incoming) < 0 ||
        !replace_scratch(c, &scratch)) return false;
    uint64_t slot = LD64(0x7001d68838ull);
    uint64_t mgr = replace_range(slot, 8) ? LD64(slot) : 0;
    if (!replace_range(mgr, 8)) return false;
    uint64_t handle = scratch.sp + 0x10;
    memset((void *)(uintptr_t)handle, 0, 0x18);
    replace_call(&scratch, 0x70010d544cull, mgr, handle, 0);
    uint64_t actor = LD64(handle + 8);
    bool accepted = replace_range(actor, 0x18) && replace_string(actor + 8, target_name) &&
        !strcmp(incoming, target_name) &&
        !(replace_call(&scratch, 0x7000be21e0ull, string, 0, 0) & 1);
    if (accepted) {
        if (!replace_handle) replace_handle = (uint64_t)(uintptr_t)hle_alloc(0x18);
        replace_call(&scratch, 0x7001070ce0ull, replace_handle, handle, 0);
        accepted = LD64(replace_handle + 8) == actor;
    }
    replace_call(&scratch, 0x7001126464ull, handle, 0, 0);
    if (!accepted) return false;
    memcpy(replace_incoming, incoming, sizeof incoming);
    return true;
}

// The ordinary full weapon pickup uses this item-name notification, before the
// older actor/error notification path. Restrict interception to proven A
// handlers, and retain only the matching current native interaction actor.
static void replace_item_popup(Cpu *c) {
    char incoming[97];
    Cpu scratch;
    bool pickup_caller = c->x[30] == 0x7000cb0ce8ull ||
        c->x[30] == 0x7000ca4444ull || c->x[30] == 0x7000cbc274ull;
    if (atomic_fetch_add(&replace_popup_count, 1) < 24)
        LOG("[quick-replace] item popup caller=%" PRIx64 " name_ptr=%" PRIx64 " pending=%d\n",
            c->x[30], c->x[0], replace_category);
    if (!replace_experiment() || replace_category >= 0 || !pickup_caller ||
        !replace_string(c->x[0], incoming) || replace_name_category(incoming) < 0 ||
        !replace_scratch(c, &scratch)) { replace_item_popup_original(c); return; }
    if (!replace_capture_ground(c, c->x[0])) {
        LOG("[quick-replace] item pickup source validation failed incoming=%s\n", incoming);
        replace_item_popup_original(c); return;
    }
    replace_start(replace_name_category(incoming));
    LOG("[quick-replace] attempted item pickup actor=%" PRIx64 " name=%s\n", LD64(replace_handle + 8), incoming);
    replace_item_popup_original(c); // Keep the native full-inventory notification.
}

// The first discovery uses Demo002_0, including the item description. Retain
// its failed query source, observe its matching full-pouch notification, and
// let native event cleanup finish before requesting the ordinary selector.
static uint64_t replace_active_event(void) {
    if (!replace_range(g_site.event_mgr, 8)) return 0;
    uint64_t mgr = LD64(g_site.event_mgr);
    return replace_range(mgr, 0x1d2c0) ? LD64(mgr + 0x1d2b8) : 0;
}

static bool replace_discovery_event(uint64_t event) {
    if (!replace_range(event, 0x70)) return false;
    uint64_t name = LD64(event + 0x68);
    return replace_range(name, 10) && !memcmp((void *)(uintptr_t)name, "Demo002_0", 10);
}

static void replace_query(Cpu *c) {
    uint64_t query = c->x[0];
    replace_query_original(c);
    uint64_t event = replace_active_event();
    if (!replace_experiment() || replace_category >= 0 || replace_event || c->x[0] != 0 ||
        !replace_discovery_event(event) || !replace_range(query, 0x38)) return;
    if (replace_capture_ground(c, query + 0x28)) {
        replace_event = event;
        replace_event_ready = false;
        replace_event_wait = 0;
        LOG("[quick-replace] discovery full source retained name=%s\n", replace_incoming);
    } else {
        char name[97] = {0};
        replace_string(query + 0x28, name);
        LOG("[quick-replace] discovery full source unavailable name=%s\n", name);
    }
}

static void replace_full_info(Cpu *c) {
    uint64_t action = c->x[0];
    char name[97];
    bool matching = replace_event && replace_active_event() == replace_event &&
        replace_range(action, 0x30) && replace_string(action + 0x20, name) &&
        !strcmp(name, replace_incoming);
    replace_full_info_original(c); // Preserve the native notification and result.
    if (matching) {
        replace_event_ready = true;
        LOG("[quick-replace] discovery full notification observed name=%s\n", name);
    }

}

// A fresh native target handle prevents retrying against a different nearby
// weapon. The retained handle participates in the game's actor lifetime rules.
static bool replace_ground_target(Cpu *c, bool full) {
    if (!replace_handle || !LD64(replace_handle + 8)) return false;
    Cpu scratch;
    if (!replace_scratch(c, &scratch)) return false;
    uint64_t slot = LD64(0x7001d68838ull);
    if (!replace_range(slot, 8)) return false;
    uint64_t mgr = LD64(slot), handle = scratch.sp + 0x10;
    if (!replace_range(mgr, 8)) return false;
    memset((void *)(uintptr_t)handle, 0, 0x18);
    replace_call(&scratch, 0x70010d544cull, mgr, handle, 0);
    bool same = LD64(handle + 8) && LD64(handle + 8) == LD64(replace_handle + 8);
    int result = same ? (int)replace_call(&scratch, 0x7000e780f0ull, handle, 0, 0) : -1;
    replace_call(&scratch, 0x7001126464ull, handle, 0, 0);
    return same && result == (full ? 7 : 0);
}

static bool replace_source_valid(Cpu *c, bool full) {
    if (replace_chest) {
        char current[97];
        uint64_t ai = replace_chest;
        if (!replace_range(ai, 0xd8) || LD64(ai + 8) != replace_owner || !LD8(ai + 0xb0) ||
            !replace_string(LD64(ai + 0x70), current) || strcmp(current, replace_incoming)) return false;
        bool available = replace_call(c, 0x7000be21e0ull, LD64(ai + 0x70), 0, 0) & 1;
        return available != full;
    }
    return replace_ground_target(c, full);
}

static bool replace_drop_selected(Cpu *c) {
    if (!quick_drop_action_enabled() || !replace_source_valid(c, true)) return false;
    uint64_t screen = replace_screen, mgr = LD64(screen + 0x3640);
    int selected = (int32_t)LD32(screen + 0x387c), count = (int32_t)LD32(screen + 0x3880);
    if (!replace_range(mgr, 0x45000) || count < 1 || count > 20 || selected < 0 || selected >= count) return false;
    Cpu scratch;
    if (!replace_scratch(c, &scratch)) return false;
    replace_call(&scratch, 0x7000eaff08ull, mgr + 0x28, 0, 0);
    uint64_t item = LD64(screen + 0x37c8 + selected * 8);
    int tab_found = -1, slot_found = -1, matches = 0, tabs = (int32_t)LD32(mgr + 0x44498);
    if (tabs > 0 && tabs <= 50) {
        for (int tab = 0; tab < tabs; tab++) {
            int n = (int32_t)replace_call(&scratch, 0x7000d970a8ull, mgr, tab, 0);
            if (n < 0 || n > 20) { matches = -100; break; }
            for (int slot = 0; slot < n; slot++)
                if (replace_call(&scratch, 0x7000d992acull, mgr, tab, slot) == item) {
                    matches++; tab_found = tab; slot_found = slot;
                }
        }
    }
    char name[97];
    if (matches == 1 && replace_range(item, 0x38) && replace_string(item + 0x28, name))
        { replace_card_drop = replace_card_open;
          quick_drop_action_request(c, screen, mgr, item, tab_found, slot_found, name); }
    replace_call(&scratch, 0x7000eaff18ull, mgr + 0x28, 0, 0);
    quick_drop_action_close(c);
    replace_card_drop = false;
    return replace_source_valid(c, false);
}

// A card we opened ourselves is the only screen-0xb exception for the
// guarded discard. Active events and every item/player restriction still apply.
bool quick_replace_card_drop_allowed(void) {
    return replace_card_drop && replace_card_open && replace_card && replace_chest &&
        replace_category >= 0 && !replace_active_event();
}

static uint64_t replace_card_screen(void) {
    uint64_t mgr = LD64(0x7001e374b8ull);
    if (!replace_range(mgr, 0x38) || LD32(mgr + 0x28) <= 0xb) return 0;
    uint64_t screens = LD64(mgr + 0x30);
    return replace_range(screens, 0x60) ? LD64(screens + 0x58) : 0;
}

// Stage only itemGet's UI record; never call its inventory-add/save tail.
static bool replace_preview_stage(Cpu *c) {
    if (replace_preview_record) return true;
    uint64_t site = LD64(0x7001d67af0ull);
    if (!replace_range(site, 8)) return false;
    uint64_t mgr = LD64(site), record = mgr + 0x44540;
    if (!replace_range(mgr, 0x44800) || !replace_range(replace_chest, 0xd0)) return false;
    uint64_t name = LD64(replace_chest + 0x70);
    int type = (int32_t)replace_call(c, 0x7000d91414ull, name, 0, 0);
    if (type != 0 && type != 1 && type != 3) return false;
    replace_call(c, 0x7000eaff08ull, mgr + 0x28, 0, 0);
    memcpy(replace_preview_saved, (void *)(uintptr_t)record, sizeof replace_preview_saved);
    ST32(record + 0x18, type); ST32(record + 0x20, 1);
    ST8(record + 0x24, 0); ST8(record + 0x25, 1);
    replace_call(c, 0x7000573698ull, record + 0x28, name, 0);
    uint32_t flags = LD32(replace_chest + 0xc8);
    ST32(record + 0x88, flags);
    if (flags) ST32(record + 0x80, LD32(replace_chest + 0xcc));
    replace_preview_record = record; replace_preview_mgr = mgr;
    replace_call(c, 0x7000eaff18ull, mgr + 0x28, 0, 0);
    return true;
}

static void replace_card_close(Cpu *c) {
    if (replace_card_paused && replace_range(replace_card, 0x104))
        ST8(replace_card + 0x103, replace_card_pause_saved);
    replace_card_paused = false;
    bool open = replace_card_open;
    replace_card_open = false;
    if (open) replace_call(c, 0x70013e99c4ull, 0, 0, 0);
    replace_card_requested = false;
    replace_card = 0;
    if (replace_preview_record) {
        replace_call(c, 0x7000eaff08ull, replace_preview_mgr + 0x28, 0, 0);
        memcpy((void *)(uintptr_t)replace_preview_record, replace_preview_saved, sizeof replace_preview_saved);
        replace_call(c, 0x7000eaff18ull, replace_preview_mgr + 0x28, 0, 0);
        replace_preview_record = replace_preview_mgr = 0;
    }
}

static void replace_card_close_gate(Cpu *c) {
    if (replace_card_open && (replace_category >= 0 || replace_chest_wait) && c->x[0] == replace_card) {
        LOG("[quick-replace] owned card close deferred caller=%" PRIx64 " mode=%d\n", c->x[30], (int32_t)c->x[1]);
        return;
    }
    replace_card_close_original(c);
}

static void replace_card_show(Cpu *c) {
    Cpu scratch;
    if (!replace_chest || replace_card_open || replace_active_event() || !replace_scratch(c, &scratch)) return;
    uint64_t site = LD64(0x7001d67960ull);
    if (!replace_range(site, 8) || !replace_range(LD64(site), 0x48)) return;
    if (!replace_card_requested) {
        if (!replace_preview_stage(&scratch)) return;
        // The native helper creates/loads screen 0xb before opening it. It may
        // not exist yet on a fresh load; do not require it before this call.
        replace_call(&scratch, 0x70013e942cull, LD64(site), LD64(replace_chest + 0x70), 0);
        replace_call(&scratch, 0x7000e7a7f0ull, LD64(replace_chest + 0x70), 0, 0);
        replace_card_requested = true;
        LOG("[quick-replace] native chest card requested item=%s\n", replace_incoming);
    }
    uint64_t card = replace_card_screen();
    if (!replace_range(card, 0x110)) return;
    replace_card = card;
    replace_card_open = true;
    LOG("[quick-replace] native chest card opened screen=%" PRIx64 " item=%s\n", card, replace_incoming);
}

static void replace_receive(Cpu *c) {
    uint64_t ai = replace_receive_ai;
    bool owned = ai && c->x[0] == LD64(ai + 8) && c->x[1] == LD64(ai + 0x70) &&
        c->x[4] == ai + 0xc8;
    if (owned && replace_receive_cancel) return; // Native cleanup still runs.
    if (owned && replace_confirmed && replace_chest_previewed) {
        memcpy(replace_award_name, replace_incoming, sizeof replace_award_name);
        replace_award_until = renderer_present_count() + 600;
    }
    replace_receive_original(c);
}

static void replace_card_action(Cpu *c) {
    uint64_t action = c->x[0], event = replace_active_event();
    char name[97];
    uint64_t event_name = replace_range(event, 0x70) ? LD64(event + 0x68) : 0;
    bool chest_event = replace_range(event_name, 10) &&
        (!memcmp((void *)(uintptr_t)event_name, "Demo000_0", 10) ||
         !memcmp((void *)(uintptr_t)event_name, "Demo002_0", 10));
    if (chest_event && replace_award_until && renderer_present_count() < replace_award_until && event &&
        replace_range(action, 0x50) && replace_string(action + 0x38, name) &&
        !strcmp(name, replace_award_name)) {
        // Complete only the repeated visual action. Native GetDemo keeps its
        // inventory award, equip decision, flags and modifier context.
        ST8(action + 0x1b, (LD8(action + 0x1b) & 0xfc) | 5);
        replace_award_until = 0;
        LOG("[quick-replace] repeated chest card skipped item=%s\n", name);
        return;
    }
    replace_card_action_original(c);
}

static void replace_chest_update(Cpu *c) {
    uint64_t ai = c->x[0];
    if (ai == replace_resume_chest) {
        replace_resume_chest = 0;
        replace_receive_ai = ai;
        replace_receive_cancel = !replace_confirmed && replace_chest_previewed;
        replace_chest_original(c);
        replace_receive_ai = 0;
        replace_receive_cancel = false;
        memset(replace_incoming, 0, sizeof replace_incoming);
        return;
    }
    if (replace_chest == ai && (replace_category >= 0 || replace_chest_wait)) {
        char current[97];
        if (replace_range(ai, 0xd8) && LD64(ai + 8) == replace_owner && LD8(ai + 0xb0) &&
            replace_string(LD64(ai + 0x70), current) && !strcmp(current, replace_incoming)) return;
        // The actor changed its receive context while we waited. Release UI
        // ownership and let the live actor update handle its own new state.
        replace_chest_wait = false;
        replace_card_close(c);
        replace_chest = 0;
        replace_closing = true;
        replace_chest_original(c);
        return;
    }
    if (!replace_experiment() || replace_category >= 0 || !replace_range(ai, 0xd8) || !LD8(ai + 0xb0)) {
        replace_chest_original(c);
        return;
    }
    char name[97];
    int category;
    Cpu scratch;
    if (!replace_string(LD64(ai + 0x70), name) || (category = replace_name_category(name)) < 0 ||
        !replace_scratch(c, &scratch)) { replace_chest_original(c); return; }
    // This is the native TreasureBox closed-wait child predicate. Suspend before
    // it clears +b0 and the original receive/modifier context, never after.
    ST64(scratch.sp + 0x10, LD64(0x7001d66260ull) + 0x10);
    ST64(scratch.sp + 0x18, 0x700164f739ull);
    if (!replace_call(&scratch, 0x7001537800ull, ai, scratch.sp + 0x10, 0) ||
        replace_call(&scratch, 0x7000be21e0ull, LD64(ai + 0x70), 0, 0)) {
        replace_chest_original(c);
        return;
    }
    memcpy(replace_incoming, name, sizeof name);
    replace_chest = ai;
    replace_owner = LD64(ai + 8);
    replace_chest_wait = true;
    replace_chest_wait_frames = 0;
    replace_chest_previewed = false;
    replace_confirmed = false;
    input_consume_buttons(input_buttons_now() & 3);
    LOG("[quick-replace] TreasureBox deferred name=%s ai=%" PRIx64 "\n", name, ai);
}

static void replace_fill(Cpu *c) {
    if (replace_category >= 0 && !replace_closing && c->x[30] == 0x7000e2853cull &&
        replace_range(c->x[0], 0x38e5)) {
        replace_screen = c->x[0];
        c->x[1] = replace_category;
        ST32(replace_screen + 0x3738, replace_category);
        ST32(replace_screen + 0x373c, replace_category);
        replace_buttons = input_buttons_now();
        LOG("[quick-replace] selector opened category=%d\n", replace_category);
    }
    replace_fill_original(c);
    if (replace_screen == c->x[0] || replace_screen) replace_card_show(c);
}

// Returns true while this selector owns A/B/Y, preventing ordinary quick-drop.
bool quick_replace_menu(Cpu *c) {
    if (replace_category < 0 || replace_screen != c->x[0]) return false;
    uint64_t buttons = input_buttons_now(), pressed = buttons & ~replace_buttons;
    replace_buttons = buttons;
    if (pressed & 3) {
        if (!(pressed & 2) && replace_incoming[0]) {
            if (!replace_drop_selected(c)) {
                input_consume_buttons(buttons & 1);
                LOG("[quick-replace] replacement rejected; selector retained\n");
                return true;
            }
            replace_confirmed = true;
        }
        input_consume_buttons(buttons & (3 | (7ull << 12)));
        ST32(replace_screen + 0x387c, LD32(replace_screen + 0x3878));
        ST8(replace_screen + 0x38e4, 2);
        replace_closing = true;
        LOG("[quick-replace] selector %s\n", pressed & 2 ? "canceled" : replace_confirmed ? "replacement confirmed" : "confirmed (prototype)");
    }
    return true;
}

// Draw the game's notification Pane after the selector's blur. Keep its
// native layout, animation and game-language message; never redraw the HUD.
static uint64_t replace_hud(void) {
    if (!replace_range(g_site.screen_mgr, 8)) return 0;
    uint64_t mgr = LD64(g_site.screen_mgr);
    if (!replace_range(mgr, 0x38) || LD32(mgr + 0x28) <= 0x25) return 0;
    uint64_t screens = LD64(mgr + 0x30);
    return replace_range(screens, 0x130) ? LD64(screens + 0x128) : 0;
}

static void replace_toast_update(Cpu *c) {
    bool owned = replace_toast && c->x[0] == replace_toast && replace_category >= 0;
    if (!owned) { replace_toast_update_original(c); return; }
    // Native HUD suppression explicitly allows this component to stay visible
    // when +1a8 is set; restore the original byte after its own update.
    uint8_t saved = LD8(replace_toast + 0x1a8);
    ST8(replace_toast + 0x1a8, 1);
    replace_toast_update_original(c);
    ST8(replace_toast + 0x1a8, saved);
    replace_toast_tick = renderer_present_count();
}

static uint64_t replace_toast_pane(void) {
    if (replace_category < 0 || !replace_range(replace_toast, 0x28)) return 0;
    uint64_t ctrl = LD64(replace_toast + 0x20);
    return replace_range(ctrl, 0x20) ? LD64(ctrl + 0x18) : 0;
}

static uint64_t replace_action_pane(void) {
    // DoCommand_00 (screen 12) owns the gameplay ABXY/action diamond. Omit
    // it before the first blur capture too, while the chest card is loading.
    if (replace_category < 0 && !replace_chest_wait) return 0;
    uint64_t mgr = LD64(0x7001e374b8ull);
    if (!replace_range(mgr, 0x38) || LD32(mgr + 0x28) <= 0xc) return 0;
    uint64_t screens = LD64(mgr + 0x30);
    if (!replace_range(screens, 0x68)) return 0;
    uint64_t screen = LD64(screens + 0x60);
    if (!replace_range(screen, 0x38)) return 0;
    uint64_t layout = LD64(screen + 0x30);
    return replace_range(layout, 0x20) ? LD64(layout + 0x18) : 0;
}

static void replace_pane_draw(Cpu *c) {
    // Omit the owned subtree from the HUD captured by the selector blur.
    if (c->x[0] == replace_toast_pane() && !replace_toast_foreground) return;
    if (c->x[0] == replace_action_pane()) return;
    replace_pane_draw_original(c);
}

static void replace_anim_advance(Cpu *c) {
    if (replace_category >= 0 && replace_range(replace_toast, 0x120)) {
        for (unsigned i = 0; i < 2; i++) {
            if (c->x[0] != LD64(replace_toast + 0x110 + i * 8)) continue;
            uint64_t present = renderer_present_count() + 1;
            if (replace_anim_present[i] == present) return;
            replace_anim_present[i] = present;
            break;
        }
    }
    replace_anim_advance_original(c);
}

static void replace_toast_animate(Cpu *c, uint64_t component) {
    uint64_t clock = LD64(0x7001e374b8ull);
    if (!replace_range(clock, 0xb24)) return;
    uint32_t delta = LD32(clock + 0xb20);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t anim = LD64(component + 0x110 + i * 8);
        if (!replace_range(anim, 0x58)) continue;
        uint64_t vt = LD64(anim);
        if (!replace_range(vt, 0x30) || LD64(vt + 0x18) != 0x7000f6c9b4ull) continue;
        if (replace_anim_present[i] == renderer_present_count() + 1) continue;
        replace_call(c, LD64(vt + 0x28), anim, 0, 0);
        Cpu animation = *c;
        animation.v[0] = delta;
        animation.x[0] = anim;
        replace_anim_advance(&animation);
    }
}

static void replace_toast_prepare(Cpu *c) {
    uint64_t hud = replace_hud();
    if (!replace_range(hud, 0x3668)) return;
    uint64_t component = LD64(hud + 0x3660);
    if (!replace_range(component, 0x390) || LD64(component) != 0x7001cde098ull) return;
    replace_toast = component;
    if (!replace_toast_started) {
        replace_call(c, 0x7000da9a0cull, component, 1, 0);
        replace_toast_started = true;
        uint64_t ctrl = LD64(component + 0x20);
        LOG("[quick-replace] native toast started component=%" PRIx64 " ctrl=%" PRIx64
            " pane=%" PRIx64 " state=%u queued=%u\n", component, ctrl,
            replace_range(ctrl, 0x28) ? LD64(ctrl + 0x18) : 0,
            LD32(component + 0x104), LD32(component + 0x200));
    }
    if (replace_screen) {
        if (replace_toast_tick != renderer_present_count())
            replace_call(c, 0x70013eb788ull, component, 0, 0);
        replace_toast_animate(c, component);
        if (replace_frames == 30 || replace_frames == 60) {
            uint64_t anim = LD64(component + 0x110);
            LOG("[quick-replace] native toast frame=%d state=%u anim_frame=%08x draw_count=%u\n",
                replace_frames, LD32(component + 0x104), replace_range(anim, 0x24) ? LD32(anim + 0x20) : 0,
                replace_toast_draw_count);
        }
    }
}

static uint64_t replace_find_pane(uint64_t pane, const char *name, unsigned depth, unsigned *budget) {
    if (!depth || !*budget || !replace_range(pane, 0xd1)) return 0;
    --*budget;
    if (!strncmp((const char *)(uintptr_t)(pane + 0xb8), name, 24)) return pane;
    uint64_t end = pane + 0x20, link = LD64(end);
    while (link != end && *budget && replace_range(link, 0x10)) {
        uint64_t found = replace_find_pane(link - 8, name, depth - 1, budget);
        if (found) return found;
        uint64_t next = LD64(link);
        if (next == link) break;
        link = next;
    }
    return 0;
}

static void replace_selector_draw(Cpu *c) {
    if (!replace_card_open || c->x[0] != replace_screen) { replace_ui_draw_original(c); return; }
    uint64_t layout = LD64(replace_screen + 0x30), info = LD64(replace_screen + 0x68);
    if (!replace_range(layout, 0x20) || !replace_range(info, 0x100)) { replace_ui_draw_original(c); return; }
    uint64_t root = LD64(layout + 0x18);
    const char *names[] = {"N_ShortCutPos_00", "N_CaptureText_00", "N_CaptureTextBlur_00"};
    uint64_t panes[3] = {0}; uint32_t saved[3];
    for (unsigned i = 0; i < 3; i++) {
        unsigned budget = 512;
        panes[i] = replace_find_pane(root, names[i], 20, &budget);
        if (!panes[i]) continue;
        saved[i] = LD32(panes[i] + 0x34);
        float y; memcpy(&y, &saved[i], 4); y -= QUICK_INVENTORY_CHEST_SHIFT;
        uint32_t moved; memcpy(&moved, &y, 4);
        ST32(panes[i] + 0x34, moved); ST8(panes[i] + 0x58, LD8(panes[i] + 0x58) | 0x10);
    }
    uint64_t context = c->x[1], saved_context = LD64(info + 0xf8);
    ST64(info + 0xf8, context);
    replace_call(c, 0x7000025c64ull, layout, info, 0);
    replace_ui_draw_original(c);
    for (unsigned i = 0; i < 3; i++) if (panes[i]) {
        ST32(panes[i] + 0x34, saved[i]); ST8(panes[i] + 0x58, LD8(panes[i] + 0x58) | 0x10);
    }
    ST64(info + 0xf8, context);
    replace_call(c, 0x7000025c64ull, layout, info, 0);
    ST64(info + 0xf8, saved_context);
}

static void replace_ui_draw(Cpu *c) {
    Cpu before = *c;
    if (replace_card_open && replace_category >= 0 && c->x[0] == replace_card) return;
    replace_selector_draw(c);
    if (replace_card_open && replace_screen && before.x[0] == replace_screen) {
        Cpu card_draw = before;
        card_draw.x[0] = replace_card;
        uint8_t visible = LD8(replace_card + 0x100), hidden = LD8(replace_card + 0x102);
        uint8_t state = LD8(replace_card + 0xfe), suppression = LD8(replace_card + 0xff);
        uint64_t layout = LD64(replace_card + 0x30), info = LD64(replace_card + 0x68);
        if (replace_range(layout, 0x20) && replace_range(info, 0x100)) {
            uint64_t saved_context = LD64(info + 0xf8);
            ST64(info + 0xf8, before.x[1]);
            replace_call(&before, 0x7000025c64ull, layout, info, 0);
            // The selector suppresses other screens. Only this owned card is
            // drawn here; restore every native visibility/state byte afterward.
            ST8(replace_card + 0x100, 1); ST8(replace_card + 0x102, 0);
            ST8(replace_card + 0xfe, 2); ST8(replace_card + 0xff, 0);
            replace_ui_draw_original(&card_draw);
            ST64(info + 0xf8, saved_context);
            ST8(replace_card + 0x100, visible); ST8(replace_card + 0x102, hidden);
            ST8(replace_card + 0xfe, state); ST8(replace_card + 0xff, suppression);
            if (replace_toast_draw_count < 2) {
                uint64_t root = LD64(layout + 0x18);
                LOG("[quick-replace] card foreground vt=%" PRIx64 " flags=%u/%u/%u/%u root=%" PRIx64 " alpha=%u/%u\n",
                    LD64(replace_card), visible, hidden, state, suppression, root,
                    replace_range(root, 0x5b) ? LD8(root + 0x59) : 0,
                    replace_range(root, 0x5b) ? LD8(root + 0x5a) : 0);
            }
        }
    }
    if (!replace_screen || before.x[0] != replace_screen || replace_category < 0 || !replace_toast) return;
    uint64_t hud = replace_hud(), context = before.x[1];
    if (!replace_range(hud, 0x70) || !replace_range(context, 0x28)) return;
    uint64_t info = LD64(hud + 0x68), layout = LD64(hud + 0x30);
    uint64_t commands = LD64(context + 0x20);
    if (!replace_range(commands, 0x10)) return;
    uint64_t ctrl = LD64(replace_toast + 0x20);
    if (!replace_range(info, 0x100) || !replace_range(ctrl, 0x28) || !replace_range(layout, 8)) return;
    uint64_t pane = LD64(ctrl + 0x18);
    if (!replace_range(pane, 8)) return;
    uint64_t vtable = LD64(pane);
    if (!replace_range(vtable, 0xb0)) return;
    uint64_t draw = LD64(vtable + 0xa8);
    if (!dispatch_lookup(draw)) return;
    uint64_t saved_context = LD64(info + 0xf8), saved_layout = LD64(info + 0xc0);
    uint16_t saved_flags = LD16(info + 0xeb);
    uint8_t saved_ed = LD8(info + 0xed), saved_ef = LD8(info + 0xef);
    ST64(info + 0xf8, context);
    replace_call(&before, 0x7000025c64ull, layout, info, 0);
    ST64(info + 0xc0, layout);
    ST16(info + 0xeb, 6); ST8(info + 0xed, 0); ST8(info + 0xef, 1);
    if (++replace_toast_draw_count <= 2)
        LOG("[quick-replace] native toast draw caller=%" PRIx64 " pane=%" PRIx64 " fn=%" PRIx64 "\n",
            before.x[30], pane, draw);
    replace_toast_foreground = true;
    replace_call(&before, draw, pane, info, commands + 8);
    replace_toast_foreground = false;
    replace_call(&before, 0x7000f60944ull, info, 0, 0);
    ST64(info + 0xf8, saved_context); ST64(info + 0xc0, saved_layout);
    ST16(info + 0xeb, saved_flags); ST8(info + 0xed, saved_ed); ST8(info + 0xef, saved_ef);
}

static void replace_tick(Cpu *c) {
    uint64_t manager = c->x[0];
    if (!replace_range(manager, 0x64c60)) { replace_tick_original(c); return; }
    if (replace_retry_until && (renderer_present_count() >= replace_retry_until || !replace_ground_target(c, false))) {
        replace_retry_until = 0;
        if (replace_handle) replace_call(c, 0x7001126464ull, replace_handle, 0, 0);
    }
    if (replace_chest_wait) {
        replace_card_show(c);
        if (replace_card_open && LD8(replace_card + 0xfe) == 2) {
            replace_chest_wait = false;
            replace_chest_previewed = true;
            replace_card_pause_saved = LD8(replace_card + 0x103);
            ST8(replace_card + 0x103, 1);
            replace_card_paused = true;
            replace_start(replace_name_category(replace_incoming));
            LOG("[quick-replace] chest card ready; selector requested\n");
        } else if (++replace_chest_wait_frames > 240) {
            LOG("[quick-replace] chest card timed out; native receive restored\n");
            replace_chest_wait = false;
            replace_card_close(c);
            replace_resume_chest = replace_chest;
            replace_chest = 0;
        }
    }
    if (replace_event) {
        uint64_t active = replace_active_event();
        if (!active && replace_event_ready && replace_ground_target(c, true)) {
            replace_event = 0;
            replace_event_ready = false;
            replace_start(replace_name_category(replace_incoming));
            LOG("[quick-replace] discovery event ended; selector and notification requested\n");
        } else if ((active && active != replace_event) ||
                   (!active && (!replace_event_ready || ++replace_event_wait > 180))) {
            replace_event = 0;
            replace_event_ready = false;
            if (replace_handle) replace_call(c, 0x7001126464ull, replace_handle, 0, 0);
            LOG("[quick-replace] discovery source released without replacement\n");
        }
    }
    const char *test = ENV("SAVAGEWIND_QUICK_REPLACE_TEST_CATEGORY");
    if (test && !replace_test_started && renderer_present_count() > 1100 && LD8(manager + 0x64b14) &&
        LD32(manager + 0x64c24) == 0 && LD32(manager + 0x64c2c) == 0) {
        int category = atoi(test);
        replace_test_started = true;
        if (category >= 0 && category <= 2) replace_start(category);
    }
    uint64_t input = LD64(manager + 0x649f0);
    if (replace_category < 0 || !replace_range(input, 0x118)) { replace_tick_original(c); return; }
    replace_toast_prepare(c);
    if (++replace_frames > 180 && !replace_screen) {
        LOG("[quick-replace] selector open timed out; inventory unchanged\n");
        replace_card_close(c);
        replace_category = -1;
        replace_resume_chest = replace_chest;
        replace_chest = 0;
        if (replace_handle && LD64(replace_handle + 8)) {
            Cpu scratch;
            if (replace_scratch(c, &scratch)) {
                ST32(scratch.sp + 0x30, 7);
                scratch.x[0] = replace_handle;
                scratch.x[1] = scratch.sp + 0x30;
                replace_popup_original(&scratch);
            }
        }
        if (replace_handle) replace_call(c, 0x7001126464ull, replace_handle, 0, 0);
        memset(replace_incoming, 0, sizeof replace_incoming);
        replace_tick_original(c);
        return;
    }
    uint32_t held = LD32(input + 0x114), edge = LD32(input + 8);
    if (!replace_closing) {
        uint32_t direction = 1u << (replace_category == 1 ? 18 : 19);
        // Native availability runs only on the first held-direction tick and
        // latches that direction even when unavailable. Release between failed
        // attempts; keep holding once the native opening transition starts.
        bool retry_release = !replace_screen && !(replace_frames & 1) &&
            LD32(manager + 0x64c24) == 0 && LD32(manager + 0x64c2c) == 0;
        ST32(input + 0x114, (held & ~0xf0000u) | (retry_release ? 0 : direction));
        if (!replace_screen)
            ST32(input + 8, (edge & ~0xf0000u) | (retry_release ? 0 : direction));
    } else {
        ST32(input + 0x114, held & ~0xf0000u);
        ST32(input + 8, edge & ~0xf0000u);
    }
    bool gate_trace = !replace_screen && (replace_frames <= 3 || replace_frames == 30 || replace_frames == 60);
    if (gate_trace)
        LOG("[quick-replace] opening pre frame=%d mode=%u pending=%u transition=%u flags=%02x latch=%x available=%u held=%x edge=%x\n",
            replace_frames, LD32(manager + 0x64c24), LD32(manager + 0x64c28), LD32(manager + 0x64c2c),
            LD8(manager + 0x64c34), LD32(manager + 0x64c58), LD8(manager + 0x64b14),
            LD32(input + 0x114), LD32(input + 8));
    replace_tick_original(c);
    if (gate_trace)
        LOG("[quick-replace] opening post frame=%d mode=%u pending=%u transition=%u flags=%02x latch=%x available=%u held=%x edge=%x\n",
            replace_frames, LD32(manager + 0x64c24), LD32(manager + 0x64c28), LD32(manager + 0x64c2c),
            LD8(manager + 0x64c34), LD32(manager + 0x64c58), LD8(manager + 0x64b14),
            LD32(input + 0x114), LD32(input + 8));
    ST32(input + 0x114, held);
    ST32(input + 8, edge);
    if (replace_closing && LD32(manager + 0x64c24) != 3 && LD32(manager + 0x64c2c) == 0) {
        LOG("[quick-replace] native close completed\n");
        if (replace_confirmed && !replace_chest && replace_ground_target(c, false))
            replace_retry_until = renderer_present_count() + 3;
        else if (replace_handle) replace_call(c, 0x7001126464ull, replace_handle, 0, 0);
        replace_card_close(c);
        replace_category = -1;
        replace_screen = 0;
        replace_resume_chest = replace_chest;
        replace_chest = 0;
        if (!replace_resume_chest) memset(replace_incoming, 0, sizeof replace_incoming);
    }
}

// Called after consumption in HID sampling; the native pickup action still
// performs the award, including modifiers/durability, through its normal A path.
uint64_t quick_replace_buttons(uint64_t buttons) {
    if (!replace_retry_until) return buttons;
    if (renderer_present_count() >= replace_retry_until || !replace_handle || !LD64(replace_handle + 8)) return buttons;
    return buttons | 1;
}

static void replace_capacity_trace(Cpu *c) {
    uint64_t caller = c->x[30], item = c->x[0];
    char name[97] = {0};
    if (replace_range(item, 16)) {
        uint64_t address = LD64(item + 8); // sead::SafeString, not Actor.
        if (replace_range(address, sizeof name)) {
            for (size_t i = 0; i + 1 < sizeof name; i++) {
                unsigned ch = LD8(address + i);
                if (!ch) break;
                name[i] = ch >= 32 && ch < 127 ? ch : '?';
            }
        }
    }
    replace_capacity_original(c);
    // Full-inventory eligibility can be queried every frame, too.
    unsigned count = atomic_fetch_add(&replace_trace_count, 1);
    if (count < 100 || count % 600 == 0)
        LOG("[quick-replace] capacity caller=%" PRIx64 " item=%s available=%u\n",
            caller, name, (unsigned)(c->x[0] & 1));
}

void quick_replace_init(void) {
    const char *trace = ENV("SAVAGEWIND_QUICK_REPLACE_TRACE");
    if (replace_capacity_original) return;
    if ((!trace || strcmp(trace, "1")) && !replace_experiment() &&
        !ENV("SAVAGEWIND_QUICK_REPLACE_TEST_CATEGORY")) return;
    replace_tick_original = dispatch_lookup(0x7000e68168ull);
    replace_fill_original = dispatch_lookup(0x7000e27c2cull);
    replace_popup_original = dispatch_lookup(0x7000e78a30ull);
    replace_item_popup_original = dispatch_lookup(0x7000e7a7f0ull);
    replace_chest_original = dispatch_lookup(0x7000a9d1bcull);
    replace_receive_original = dispatch_lookup(0x7000be1e30ull);
    replace_card_action_original = dispatch_lookup(0x7000709ba0ull);
    replace_card_close_original = dispatch_lookup(0x70013ed264ull);
    if (!replace_receive_original || !replace_card_action_original || !replace_card_close_original) return;
    const uint64_t helpers[] = {0x7001537800ull, 0x7000be21e0ull, 0x7001070ce0ull,
        0x7001126464ull, 0x70010d544cull, 0x7000e780f0ull, 0x7000da9a0cull,
        0x70013eb788ull, 0x7000d91414ull, 0x7000573698ull, 0x7000eaff08ull, 0x7000eaff18ull, 0x70013e942cull, 0x70013e99c4ull, 0x7000f60944ull, 0x7000025c64ull};
    for (size_t i = 0; i < sizeof helpers / sizeof *helpers; i++) if (!dispatch_lookup(helpers[i])) return;
    if (!replace_tick_original || !replace_fill_original || !replace_popup_original || !replace_chest_original || !replace_item_popup_original) return;
    replace_query_original = dispatch_lookup(0x7000b445c0ull);
    replace_full_info_original = dispatch_lookup(0x70005e6e48ull);
    replace_ui_draw_original = dispatch_lookup(0x70013ee084ull);
    replace_toast_update_original = dispatch_lookup(0x7000daa3fcull);
    replace_pane_draw_original = dispatch_lookup(0x7000027d30ull);
    replace_anim_advance_original = dispatch_lookup(0x7000f6c9b4ull);
    if (!replace_query_original || !replace_full_info_original ||
        !replace_ui_draw_original || !replace_toast_update_original ||
        !replace_pane_draw_original || !replace_anim_advance_original) return;
    replace_capacity_original = dispatch_lookup(0x7000be21e0ull);
    if (!replace_capacity_original) {
        LOG("[quick-replace] missing capacity callback; trace disabled\n");
        return;
    }
    hle_override(0x7000be21e0ull, replace_capacity_trace, "quick-replace capacity trace");
    hle_override(0x7000e68168ull, replace_tick, "quick-replace native menu lifecycle");
    hle_override(0x7000e27c2cull, replace_fill, "quick-replace gear category");
    hle_override(0x7000e78a30ull, replace_popup, "quick-replace attempted pickup");
    hle_override(0x7000e7a7f0ull, replace_item_popup, "quick-replace attempted item pickup");
    hle_override(0x7000be1e30ull, replace_receive, "quick-replace owned chest receive");
    hle_override(0x70013ed264ull, replace_card_close_gate, "quick-replace keep owned card open");
    hle_override(0x7000709ba0ull, replace_card_action, "quick-replace duplicate chest card");
    hle_override(0x7000a9d1bcull, replace_chest_update, "quick-replace TreasureBox pending receive");
    hle_override(0x7000b445c0ull, replace_query, "quick-replace discovery capacity query");
    hle_override(0x70005e6e48ull, replace_full_info, "quick-replace discovery full notification");
    hle_override(0x70013ee084ull, replace_ui_draw, "quick-replace native toast foreground");
    hle_override(0x7000daa3fcull, replace_toast_update, "quick-replace native toast update");
    hle_override(0x7000027d30ull, replace_pane_draw, "quick-replace toast blur exclusion");
    hle_override(0x7000f6c9b4ull, replace_anim_advance, "quick-replace toast animation clock");
    LOG("[quick-replace] receive trace enabled; inventory unchanged\n");
}
