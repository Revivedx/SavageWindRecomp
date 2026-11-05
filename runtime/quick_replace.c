// Native full-inventory replacement for the verified BOTW 1.9.0 layout.
#include <inttypes.h>
#include "quick_replace.h"
#include "settings.h"
#include "quick_drop_action.h"
#include <stdatomic.h>
#include <string.h>
#include "hle.h"
#include "game_sites.h"
#include "ui/quick_inventory_layout.h"

static bool replace_ready;
static bool replace_popup_suppressed;
static uint64_t replace_toast, replace_toast_tick;
static uint64_t replace_anim_present[2];
static bool replace_toast_foreground;
// Exact selector-owned filtered presentation pane, scoped to its draw only.
static uint64_t replace_selector_blur_pane;
static bool replace_toast_started;
static unsigned replace_toast_draw_count;
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
static bool replace_ground_preview;
static uint32_t replace_ground_flags, replace_ground_modifier;
static unsigned replace_event_wait;
static int replace_category = -1, replace_frames;
static uint64_t replace_screen, replace_buttons;
static bool replace_closing;
static uint64_t replace_chest, replace_resume_chest, replace_owner;
static uint32_t replace_chest_flags, replace_chest_modifier;
static char replace_incoming[97];
static uint64_t replace_handle, replace_retry_until;
static bool replace_confirmed;
static bool replace_outgoing_pending, replace_source_lost;
static uint64_t replace_chest_handle;
static uint64_t replace_ground_index, replace_chest_index;
static _Atomic bool replace_chest_context_current;
static bool replace_receive_observed;
extern uint64_t input_buttons_now(void);
extern void input_consume_buttons(uint64_t);
extern uint64_t renderer_present_count(void);

bool quick_replace_active(void) { return replace_category >= 0 && replace_screen && !replace_closing; }
bool quick_replace_bottom_layout(void) { return quick_replace_active() && replace_card_open; }

RT_ON_RESET(quick_replace_reset) {
    replace_category = -1;
    replace_frames = 0;
    replace_screen = replace_buttons = 0;
    replace_closing = false;
    replace_ready = replace_popup_suppressed = false;
    replace_chest = replace_resume_chest = replace_owner = 0;
    replace_chest_flags = replace_chest_modifier = 0;
    memset(replace_incoming, 0, sizeof replace_incoming);
    replace_handle = replace_retry_until = 0;
    replace_confirmed = false;
    replace_outgoing_pending = replace_source_lost = false;
    replace_chest_handle = 0;
    replace_ground_index = replace_chest_index = 0;
    atomic_store(&replace_chest_context_current, false);
    replace_receive_observed = false;
    replace_card = replace_receive_ai = replace_award_until = 0;
    replace_card_open = replace_card_requested = replace_card_drop = replace_receive_cancel = false;
    replace_chest_wait = replace_card_paused = replace_chest_previewed = false;
    replace_chest_wait_frames = 0;
    replace_preview_record = replace_preview_mgr = 0;
    memset(replace_award_name, 0, sizeof replace_award_name);
    replace_event = 0;
    replace_event_ready = false;
    replace_ground_preview = false;
    replace_ground_flags = replace_ground_modifier = 0;
    replace_event_wait = 0;
    replace_toast = replace_toast_tick = 0;
    memset(replace_anim_present, 0, sizeof replace_anim_present);
    replace_toast_foreground = false;
    replace_selector_blur_pane = 0;
    replace_toast_started = false;
    replace_toast_draw_count = 0;
}

static bool replace_range(uint64_t address, uint64_t size) {
    return address >= GUEST_LO && address < GUEST_HI && size <= GUEST_HI - address;
}

bool quick_replace_enabled(void) {
    return g_settings.quick_replace && !strcmp(g_game_version, "1.9.0") && quick_replace_init();
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

// Ordinary ActorLinks may be non-retaining inside a native actor job. Keep a
// retained, generation-checked native link across frames and refresh the
// ordinary view in the current guest job before accessing its actor.
static bool replace_index_capture(Cpu *c, uint64_t *index, uint64_t actor) {
    if (!*index) {
        *index = (uint64_t)(uintptr_t)hle_alloc(0x10);
        if (!replace_range(*index, 0x10)) return false;
        memset((void *)(uintptr_t)*index, 0, 0x10);
        ST32(*index + 8, UINT32_MAX);
    }
    return (replace_call(c, g_site.actor_index_assign, *index, actor, 1) & 1) && LD8(*index + 0xc);
}

static uint64_t replace_index_actor(Cpu *c, uint64_t index, uint64_t ordinary) {
    if (!replace_range(index, 0x10) || !LD8(index + 0xc) ||
        !replace_range(ordinary, 0x18)) return 0;
    uint64_t actor = replace_call(c, g_site.actor_index_resolve, index, ordinary, 0);
    return actor && LD64(ordinary + 8) == actor && replace_range(actor, 0x65) &&
        LD8(actor + 0x64) == 1 ? actor : 0;
}

static uint64_t replace_ground_actor(Cpu *c) {
    return replace_index_actor(c, replace_ground_index, replace_handle);
}

static bool replace_index_identity(uint64_t index, uint64_t actor) {
    if (!actor || !replace_range(index, 0x10) || !LD8(index + 0xc) || LD32(index + 8) == UINT32_MAX)
        return false;
    uint64_t block = LD64(index);
    return replace_range(block, 0x50) && LD32(block + 0x40) == LD32(index + 8) &&
        LD64(block + 0x48) == actor;
}

static void replace_ground_release(Cpu *c) {
    replace_retry_until = 0;
    if (replace_handle) replace_call(c, g_site.actor_link_release, replace_handle, 0, 0);
    if (replace_ground_index) replace_call(c, g_site.actor_index_release, replace_ground_index, 0, 0);
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

// Known ground weapons retain their modifier info on the WeaponBase actor.
// This is the same native accessor used by the ordinary pickup before itemGet.
static void replace_capture_known_ground_preview(Cpu *c) {
    if (replace_ground_preview) return;
    uint64_t actor = replace_ground_actor(c);
    if (!actor) return;
    uint64_t modifier = replace_call(c, g_site.replace_weapon_modifier, replace_handle, 0, 0);
    if (!replace_range(modifier, 8) || modifier != actor + 0xf98) return;
    replace_ground_flags = LD32(modifier);
    replace_ground_modifier = replace_ground_flags ? LD32(modifier + 4) : 0;
    replace_ground_preview = true;
    LOG("[quick-replace] ground preview captured item=%s flags=%x value=%u\n",
        replace_incoming, replace_ground_flags, replace_ground_modifier);
}

// During a first-discovery event the game's item-get record contains the
// actual instance's modifiers. Retain only values after matching its name;
// pointers in that shared record remain owned by the game.
static uint64_t replace_card_screen(void);
static void replace_capture_discovery_preview(Cpu *c) {
    if (!replace_event || replace_ground_preview) return;
    uint64_t card = replace_card_screen();
    if (!replace_range(card, 0x104) || LD8(card + 0xfe) != 2 || !LD8(card + 0x100)) return;
    uint64_t site = LD64(g_site.replace_pouch_manager_slot);
    uint64_t mgr = replace_range(site, 8) ? LD64(site) : 0;
    if (!replace_range(mgr, 0x44800)) return;
    uint64_t record = mgr + 0x44540;
    char name[97];
    replace_call(c, g_site.pouch_lock, mgr + 0x28, 0, 0);
    bool matching = LD8(record + 0x25) && replace_string(record + 0x28, name) &&
        !strcmp(name, replace_incoming);
    if (matching) {
        replace_ground_flags = LD32(record + 0x88);
        replace_ground_modifier = LD32(record + 0x80);
        replace_ground_preview = true;
    }
    replace_call(c, g_site.pouch_unlock, mgr + 0x28, 0, 0);
    if (!matching) return;
    LOG("[quick-replace] discovery preview captured item=%s flags=%x value=%u\n",
        name, replace_ground_flags, replace_ground_modifier);
}

void rt_quick_replace_popup(Cpu *c) {
    if (!quick_replace_enabled() || replace_popup_suppressed) return;
    uint64_t actor = replace_range(c->x[0], 0x18) ? LD64(c->x[0] + 8) : 0;
    if (quick_replace_enabled() && replace_category < 0 && !replace_resume_chest &&
        replace_range(c->x[1], 4) && LD32(c->x[1]) == 7 && replace_range(c->x[0], 0x18)) {
        char name[97];
        if (replace_range(actor, 0x18) && replace_string(actor + 8, name)) {
            int category = replace_name_category(name);
            LOG("[quick-replace] attempted pickup actor=%" PRIx64 " name=%s category=%d\n", actor, name, category);
            if (category >= 0) {
                replace_ground_release(c);
                if (!replace_handle) replace_handle = (uint64_t)(uintptr_t)hle_alloc(0x18);
                replace_call(c, g_site.actor_link_copy, replace_handle, c->x[0], 0);
                if (LD64(replace_handle + 8) != actor ||
                    !replace_index_capture(c, &replace_ground_index, actor)) {
                    replace_ground_release(c); return;
                }
                replace_ground_preview = false;
                memcpy(replace_incoming, name, sizeof name);
                replace_capture_known_ground_preview(c);
                replace_start(category);
                return;
            }
        }
    }
}

static bool replace_capture_ground(Cpu *c, uint64_t string) {
    char incoming[97], target_name[97];
    Cpu scratch;
    if (!replace_string(string, incoming) || replace_name_category(incoming) < 0 ||
        !replace_scratch(c, &scratch)) return false;
    uint64_t slot = LD64(g_site.replace_interaction_manager_slot);
    uint64_t mgr = replace_range(slot, 8) ? LD64(slot) : 0;
    if (!replace_range(mgr, 8)) return false;
    uint64_t handle = scratch.sp + 0x10;
    memset((void *)(uintptr_t)handle, 0, 0x18);
    replace_call(&scratch, g_site.replace_interaction_target, mgr, handle, 0);
    uint64_t actor = LD64(handle + 8);
    bool accepted = replace_range(actor, 0x18) && replace_string(actor + 8, target_name) &&
        !strcmp(incoming, target_name) &&
        !(replace_call(&scratch, g_site.pouch_can_receive, string, 0, 0) & 1);
    if (accepted) {
        // The links are shared with the previous automatic pickup. Retire its
        // retry before assigning a new source, including first discoveries.
        replace_ground_release(&scratch);
        if (!replace_handle) replace_handle = (uint64_t)(uintptr_t)hle_alloc(0x18);
        replace_call(&scratch, g_site.actor_link_copy, replace_handle, handle, 0);
        accepted = LD64(replace_handle + 8) == actor &&
            replace_index_capture(&scratch, &replace_ground_index, actor);
    }
    replace_call(&scratch, g_site.actor_link_release, handle, 0, 0);
    if (!accepted) { replace_ground_release(c); return false; }
    replace_ground_preview = false;
    memcpy(replace_incoming, incoming, sizeof incoming);
    return true;
}

// The ordinary full weapon pickup uses this item-name notification, before the
// older actor/error notification path. Restrict interception to proven A
// handlers, and retain only the matching current native interaction actor.
void rt_quick_replace_item_popup(Cpu *c) {
    if (!quick_replace_enabled()) return;
    char incoming[97];
    Cpu scratch;
    bool pickup_caller = c->x[30] == g_site.replace_pickup_call_a + 4 ||
        c->x[30] == g_site.replace_pickup_call_b + 4 || c->x[30] == g_site.replace_pickup_call_c + 4;
    if (!quick_replace_enabled() || replace_category >= 0 || replace_resume_chest || !pickup_caller ||
        !replace_string(c->x[0], incoming) || replace_name_category(incoming) < 0 ||
        !replace_scratch(c, &scratch)) { return; }
    if (!replace_capture_ground(c, c->x[0])) {
        LOG("[quick-replace] item pickup source validation failed incoming=%s\n", incoming);
        return;
    }
    replace_capture_known_ground_preview(c);
    replace_start(replace_name_category(incoming));
    LOG("[quick-replace] attempted item pickup actor=%" PRIx64 " name=%s\n", LD64(replace_handle + 8), incoming);
    // The checked entry patch continues with the native notification.
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

void rt_quick_replace_query(Cpu *c, uint64_t query) {
    uint64_t event = replace_active_event();
    if (!quick_replace_enabled() || replace_category >= 0 || replace_resume_chest || replace_event || c->x[0] != 0 ||
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

void rt_quick_replace_full_info(Cpu *c, GuestFn native) {
    char name[97];
    bool matching = replace_event && replace_active_event() == replace_event &&
        replace_string(c->x[0], name) &&
        !strcmp(name, replace_incoming);
    native(c); // Preserve the native notification and result.
    if (matching) {
        replace_event_ready = true;
        LOG("[quick-replace] discovery full notification observed name=%s\n", name);
    }

}

// A fresh native target handle prevents retrying against a different nearby
// weapon. The retained handle participates in the game's actor lifetime rules.
static bool replace_ground_target(Cpu *c, bool full) {
    uint64_t actor = replace_ground_actor(c);
    if (!actor) return false;
    Cpu scratch;
    if (!replace_scratch(c, &scratch)) return false;
    uint64_t slot = LD64(g_site.replace_interaction_manager_slot);
    if (!replace_range(slot, 8)) return false;
    uint64_t mgr = LD64(slot), handle = scratch.sp + 0x10;
    if (!replace_range(mgr, 8)) return false;
    memset((void *)(uintptr_t)handle, 0, 0x18);
    replace_call(&scratch, g_site.replace_interaction_target, mgr, handle, 0);
    bool same = LD64(handle + 8) == actor;
    int result = same ? (int)replace_call(&scratch, g_site.replace_pickup_status, handle, 0, 0) : -1;
    replace_call(&scratch, g_site.actor_link_release, handle, 0, 0);
    return same && result == (full ? 7 : 0);
}

static bool replace_chest_owner_live(Cpu *c) {
    // Validate the retained actor before touching an AI saved across world ticks.
    return replace_owner && replace_index_actor(c, replace_chest_index, replace_chest_handle) == replace_owner;
}

static bool replace_chest_live(Cpu *c, uint64_t ai) {
    (void)c;
    // Only the current native AI/receive callback supplies ai here. Native
    // a9d214 loads AI+8 and passes that live owner to be1e30 at a9d274. Native
    // 15198cc can refuse a general resolve in this actor's own job, so validate
    // the callback-owned actor against our retained generation instead. UI
    // polling continues to use replace_chest_owner_live and a fresh resolve.
    return replace_range(ai, 0xd8) && LD64(ai + 8) == replace_owner &&
        replace_index_identity(replace_chest_index, replace_owner) &&
        replace_range(replace_owner, 0x65) && LD8(replace_owner + 0x64) == 1;
}

static bool replace_chest_context(Cpu *c, uint64_t ai) {
    bool owner_live = replace_chest_live(c, ai);
    bool current_ai = replace_range(ai, 0xd8);
    char current[97] = {0};
    bool name_valid = current_ai && replace_string(LD64(ai + 0x70), current);
    bool matching = owner_live && current_ai && LD64(ai + 8) == replace_owner && LD8(ai + 0xb0) &&
        LD32(ai + 0xc8) == replace_chest_flags &&
        (!replace_chest_flags || LD32(ai + 0xcc) == replace_chest_modifier) &&
        name_valid && !strcmp(current, replace_incoming);
    return matching;
}

static void replace_chest_release(Cpu *c) {
    if (replace_chest_handle) replace_call(c, g_site.actor_link_release, replace_chest_handle, 0, 0);
    if (replace_chest_index) replace_call(c, g_site.actor_index_release, replace_chest_index, 0, 0);
    replace_owner = 0;
    atomic_store(&replace_chest_context_current, false);
}

static bool replace_chest_capacity(Cpu *c, bool full) {
    Cpu scratch;
    if (!replace_scratch(c, &scratch) || !replace_incoming[0]) return false;
    // Query with an owned copy, never a SafeString reached through retained AI.
    uint64_t string = scratch.sp + 0x10, name = scratch.sp + 0x30;
    ST64(string, LD64(g_site.replace_safe_string_vtable_slot) + 0x10);
    ST64(string + 8, name);
    memcpy((void *)(uintptr_t)name, replace_incoming, strlen(replace_incoming) + 1);
    bool available = replace_call(&scratch, g_site.pouch_can_receive, string, 0, 0) & 1;
    return available != full;
}

static bool replace_ground_identity(Cpu *c) {
    uint64_t actor = replace_ground_actor(c);
    if (!actor) return false;
    char current[97];
    if (!replace_range(actor, 0x65) || LD8(actor + 0x64) != 1 || !replace_string(actor + 8, current) ||
        strcmp(current, replace_incoming)) return false;
    uint64_t modifier = replace_call(c, g_site.replace_weapon_modifier, replace_handle, 0, 0);
    return replace_ground_preview && replace_range(modifier, 8) && modifier == actor + 0xf98 &&
        LD32(modifier) == replace_ground_flags &&
        (!replace_ground_flags || LD32(modifier + 4) == replace_ground_modifier);
}

static bool replace_source_valid(Cpu *c, bool full) {
    if (replace_chest) {
        return replace_chest_owner_live(c) && atomic_load(&replace_chest_context_current) &&
            replace_chest_capacity(c, full);
    }
    return replace_ground_identity(c) && replace_ground_target(c, full);
}

static QuickDropResult replace_drop_selected(Cpu *c) {
    if (!quick_replace_enabled() || !quick_drop_action_init() || quick_drop_action_busy() || !replace_source_valid(c, true)) return QUICK_DROP_REJECTED;
    uint64_t screen = replace_screen, mgr = LD64(screen + 0x3640);
    int selected = (int32_t)LD32(screen + 0x387c), count = (int32_t)LD32(screen + 0x3880);
    if (!replace_range(mgr, 0x45000) || count < 1 || count > 20 || selected < 0 || selected >= count) return QUICK_DROP_REJECTED;
    Cpu scratch;
    if (!replace_scratch(c, &scratch)) return QUICK_DROP_REJECTED;
    replace_call(&scratch, g_site.pouch_lock, mgr + 0x28, 0, 0);
    uint64_t item = LD64(screen + 0x37c8 + selected * 8);
    int tab_found = -1, slot_found = -1, matches = 0, tabs = (int32_t)LD32(mgr + 0x44498);
    if (tabs > 0 && tabs <= 50) {
        for (int tab = 0; tab < tabs; tab++) {
            int n = (int32_t)replace_call(&scratch, g_site.pouch_page_count, mgr, tab, 0);
            if (n < 0 || n > 20) { matches = -100; break; }
            for (int slot = 0; slot < n; slot++)
                if (replace_call(&scratch, g_site.pouch_page_item, mgr, tab, slot) == item) {
                    matches++; tab_found = tab; slot_found = slot;
                }
        }
    }
    char name[97];
    bool requested = matches == 1 && replace_range(item, 0x38) && replace_string(item + 0x28, name);
    if (requested)
        { replace_card_drop = replace_card_open;
          quick_drop_action_request(c, screen, mgr, item, tab_found, slot_found, name); }
    replace_call(&scratch, g_site.pouch_unlock, mgr + 0x28, 0, 0);
    quick_drop_action_close(c);
    replace_card_drop = false;
    return requested ? quick_drop_action_result() : QUICK_DROP_REJECTED;
}

// A card we opened ourselves is the only screen-0xb exception for the
// guarded discard. Active events and every item/player restriction still apply.
bool quick_replace_card_drop_allowed(void) {
    return replace_card_drop && replace_card_open && replace_card && (replace_chest || replace_ground_preview) &&
        replace_category >= 0 && !replace_active_event();
}

// Sorting is synchronous and doesn't discard or receive an item. Its owned
// preview exception lasts while this selector is active, independently of the
// short discard scope above. An unrelated selector/modal cannot use it.
bool quick_replace_card_sort_allowed(uint64_t screen) {
    return quick_replace_active() && replace_screen == screen && replace_card_open && replace_card &&
        (replace_chest || replace_ground_preview) && !replace_active_event();
}

static uint64_t replace_card_screen(void) {
    uint64_t mgr = LD64(g_site.screen_mgr);
    if (!replace_range(mgr, 0x38) || LD32(mgr + 0x28) <= 0xb) return 0;
    uint64_t screens = LD64(mgr + 0x30);
    return replace_range(screens, 0x60) ? LD64(screens + 0x58) : 0;
}

// Stage only itemGet's UI record; never call its inventory-add/save tail.
static bool replace_preview_stage(Cpu *c) {
    if (replace_preview_record) return true;
    uint64_t site = LD64(g_site.replace_pouch_manager_slot);
    if (!replace_range(site, 8)) return false;
    uint64_t mgr = LD64(site), record = mgr + 0x44540;
    if (!replace_range(mgr, 0x44800)) return false;
    uint64_t actor = replace_chest ? 0 : replace_ground_actor(c);
    uint64_t name = replace_chest && replace_range(replace_chest, 0xd0) ? LD64(replace_chest + 0x70) :
        replace_ground_preview && replace_range(actor, 0x18) ? actor + 8 : 0;
    char current[97];
    if (!replace_string(name, current) || strcmp(current, replace_incoming)) return false;
    int type = (int32_t)replace_call(c, g_site.pouch_item_type, name, 0, 0);
    if (type != 0 && type != 1 && type != 3) return false;
    replace_call(c, g_site.pouch_lock, mgr + 0x28, 0, 0);
    memcpy(replace_preview_saved, (void *)(uintptr_t)record, sizeof replace_preview_saved);
    ST32(record + 0x18, type); ST32(record + 0x20, 1);
    ST8(record + 0x24, 0); ST8(record + 0x25, 1);
    replace_call(c, g_site.replace_string_assign, record + 0x28, name, 0);
    uint32_t flags = replace_chest ? LD32(replace_chest + 0xc8) : replace_ground_flags;
    ST32(record + 0x88, flags);
    ST32(record + 0x80, flags ? (replace_chest ? LD32(replace_chest + 0xcc) : replace_ground_modifier) : 0);
    replace_preview_record = record; replace_preview_mgr = mgr;
    replace_call(c, g_site.pouch_unlock, mgr + 0x28, 0, 0);
    return true;
}

static void replace_card_close(Cpu *c) {
    if (replace_card_paused && replace_range(replace_card, 0x104))
        ST8(replace_card + 0x103, replace_card_pause_saved);
    replace_card_paused = false;
    bool open = replace_card_open;
    replace_card_open = false;
    if (open) replace_call(c, g_site.replace_card_close, 0, 0, 0);
    replace_card_requested = false;
    replace_card = 0;
    if (replace_preview_record) {
        replace_call(c, g_site.pouch_lock, replace_preview_mgr + 0x28, 0, 0);
        memcpy((void *)(uintptr_t)replace_preview_record, replace_preview_saved, sizeof replace_preview_saved);
        replace_call(c, g_site.pouch_unlock, replace_preview_mgr + 0x28, 0, 0);
        replace_preview_record = replace_preview_mgr = 0;
    }
}

bool rt_quick_replace_card_close(Cpu *c) {
    if (replace_card_open && (replace_category >= 0 || replace_chest_wait) && c->x[0] == replace_card) {
        LOG("[quick-replace] owned card close deferred caller=%" PRIx64 " mode=%d\n", c->x[30], (int32_t)c->x[1]);
        return true;
    }
    return false;
}

static void replace_card_show(Cpu *c) {
    Cpu scratch;
    if (replace_closing || replace_outgoing_pending || (!replace_chest && !replace_ground_preview) ||
        replace_card_open || replace_active_event() || !replace_scratch(c, &scratch)) return;
    uint64_t site = LD64(g_site.replace_card_manager_slot);
    if (!replace_range(site, 8) || !replace_range(LD64(site), 0x48)) return;
    if (!replace_card_requested) {
        if (!replace_preview_stage(&scratch)) return;
        // The native helper creates/loads screen 0xb before opening it. It may
        // not exist yet on a fresh load; do not require it before this call.
        uint64_t actor = replace_chest ? 0 : replace_ground_actor(&scratch);
        if (!replace_chest && !actor) return;
        uint64_t name = replace_chest ? LD64(replace_chest + 0x70) : actor + 8;
        replace_call(&scratch, g_site.replace_card_prepare, LD64(site), name, 0);
        replace_call(&scratch, g_site.replace_item_popup, name, 0, 0);
        replace_card_requested = true;
        LOG("[quick-replace] native chest card requested item=%s\n", replace_incoming);
    }
    uint64_t card = replace_card_screen();
    if (!replace_range(card, 0x110)) return;
    replace_card = card;
    replace_card_open = true;
    LOG("[quick-replace] native chest card opened screen=%" PRIx64 " item=%s\n", card, replace_incoming);
}

bool rt_quick_replace_receive(Cpu *c) {
    uint64_t ai = replace_receive_ai;
    bool owned = ai && replace_chest_live(c, ai) && c->x[0] == replace_owner && c->x[1] == LD64(ai + 0x70) &&
        c->x[4] == ai + 0xc8;
    if (owned) replace_receive_observed = true;
    if (owned && replace_receive_cancel) return true; // Native cleanup still runs.
    if (owned && replace_confirmed && replace_chest_previewed) {
        memcpy(replace_award_name, replace_incoming, sizeof replace_award_name);
        replace_award_until = renderer_present_count() + 600;
    }
    return false;
}

bool rt_quick_replace_card_action(Cpu *c) {
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
        return true;
    }
    return false;
}

unsigned rt_quick_replace_chest_begin(Cpu *c) {
    uint64_t ai = c->x[0];
    if (ai == replace_resume_chest) {
        replace_resume_chest = 0;
        if (!replace_chest_live(c, ai)) {
            replace_confirmed = false;
            replace_chest_release(c);
            memset(replace_incoming, 0, sizeof replace_incoming);
            return 1;
        }
        if (!replace_chest_context(c, ai)) atomic_store(&replace_chest_context_current, false);
        if (!atomic_load(&replace_chest_context_current) || replace_active_event() ||
            !replace_chest_capacity(c, false)) replace_confirmed = false;
        bool pending = LD8(ai + 0xb0) != 0;
        replace_receive_ai = ai;
        replace_receive_cancel = !replace_confirmed && replace_chest_previewed;
        replace_receive_observed = false;
        return pending ? 3 : 2;
    }
    if (replace_chest == ai && (replace_category >= 0 || replace_chest_wait)) {
        if (replace_chest_context(c, ai)) return 0;
        atomic_store(&replace_chest_context_current, false);
        if (replace_outgoing_pending && replace_chest_live(c, ai)) {
            // Changed reward data cancels the incoming award. Keep this live
            // AI suspended until outgoing work completes, then run its native
            // cleanup with receive suppressed, as for Back cancellation.
            replace_source_lost = true;
            return 0;
        }
        // The actor changed its receive context while we waited. Release UI
        // ownership and let the live actor update handle its own new state.
        replace_chest_wait = false;
        replace_card_close(c);
        replace_chest = 0;
        replace_source_lost = true;
        replace_confirmed = false;
        replace_chest_release(c);
        replace_closing = true;
        return 1;
    }
    if (!quick_replace_enabled() || replace_category >= 0 || replace_resume_chest ||
        !replace_range(ai, 0xd8) || !LD8(ai + 0xb0)) {
        return 1;
    }
    char name[97];
    int category;
    Cpu scratch;
    if (!replace_string(LD64(ai + 0x70), name) || (category = replace_name_category(name)) < 0 ||
        !replace_scratch(c, &scratch)) { return 1; }
    // This is the native TreasureBox closed-wait child predicate. Suspend before
    // it clears +b0 and the original receive/modifier context, never after.
    ST64(scratch.sp + 0x10, LD64(g_site.replace_safe_string_vtable_slot) + 0x10);
    ST64(scratch.sp + 0x18, g_site.replace_chest_closed_wait_name);
    if (!replace_call(&scratch, g_site.replace_ai_child_matches, ai, scratch.sp + 0x10, 0) ||
        replace_call(&scratch, g_site.pouch_can_receive, LD64(ai + 0x70), 0, 0)) {
        return 1;
    }
    uint64_t owner = LD64(ai + 8);
    if (!replace_range(owner, 0x65) || LD8(owner + 0x64) != 1) { return 1; }
    if (!replace_chest_handle) replace_chest_handle = (uint64_t)(uintptr_t)hle_alloc(0x18);
    if (!replace_range(replace_chest_handle, 0x18) ||
        !replace_call(&scratch, g_site.actor_link_assign, replace_chest_handle, owner, 0) ||
        LD64(replace_chest_handle + 8) != owner || LD8(owner + 0x64) != 1 ||
        !replace_index_capture(&scratch, &replace_chest_index, owner)) {
        replace_chest_release(c);
        return 1;
    }
    memcpy(replace_incoming, name, sizeof name);
    replace_chest = ai;
    replace_owner = owner;
    replace_chest_flags = LD32(ai + 0xc8);
    replace_chest_modifier = LD32(ai + 0xcc);
    // A fresh chest opens its card before replace_start runs. Initialize its
    // transaction now rather than inheriting the preceding selector's close.
    replace_closing = false;
    replace_source_lost = false;
    atomic_store(&replace_chest_context_current, true);
    replace_chest_wait = true;
    replace_chest_wait_frames = 0;
    replace_chest_previewed = false;
    replace_confirmed = false;
    input_consume_buttons(input_buttons_now() & 3);
    LOG("[quick-replace] TreasureBox deferred name=%s ai=%" PRIx64 "\n", name, ai);
    return 0;
}

void rt_quick_replace_chest_end(Cpu *c, uint64_t ai, unsigned mode) {
    if (mode < 2) return;
    replace_receive_ai = 0;
    replace_receive_cancel = false;
    // A deferred native demo may keep the pending receive alive. The callback
    // proves consumption without dereferencing AI after the native update.
    if (!replace_receive_observed && mode == 3 &&
        replace_index_identity(replace_chest_index, replace_owner)) {
        replace_resume_chest = ai;
        return;
    }
    replace_chest_release(c);
    memset(replace_incoming, 0, sizeof replace_incoming);
}

void rt_quick_replace_fill(Cpu *c, GuestFn native) {
    if (replace_category >= 0 && !replace_closing && c->x[30] == g_site.replace_fill_call + 4 &&
        replace_range(c->x[0], 0x38e5)) {
        replace_screen = c->x[0];
        c->x[1] = replace_category;
        ST32(replace_screen + 0x3738, replace_category);
        ST32(replace_screen + 0x373c, replace_category);
        replace_buttons = input_buttons_now();
        LOG("[quick-replace] selector opened category=%d\n", replace_category);
    }
    native(c);
    if (replace_screen == c->x[0] || replace_screen) replace_card_show(c);
}

// Returns true while this selector owns A/B/Y, preventing ordinary quick-drop.
bool quick_replace_menu(Cpu *c) {
    if (replace_category < 0 || replace_screen != c->x[0]) return false;
    if (replace_closing) return true;
    uint64_t buttons = input_buttons_now(), pressed = buttons & ~replace_buttons;
    replace_buttons = buttons;
    if (pressed & 3) {
        if (!(pressed & 2) && replace_incoming[0]) {
            QuickDropResult result = replace_drop_selected(c);
            if (result == QUICK_DROP_REJECTED) {
                input_consume_buttons(buttons & 1);
                LOG("[quick-replace] replacement rejected; selector retained\n");
                return true;
            }
            replace_outgoing_pending = result == QUICK_DROP_PENDING;
            replace_source_lost = false;
            replace_confirmed = result == QUICK_DROP_COMPLETE && replace_source_valid(c, false);
            // Native equipped work only runs after both the selector and its
            // owned item card release the world's pause state.
            if (replace_outgoing_pending) replace_card_close(c);
        }
        input_consume_buttons(buttons & (3 | (7ull << 12)));
        ST32(replace_screen + 0x387c, LD32(replace_screen + 0x3878));
        ST8(replace_screen + 0x38e4, 2);
        replace_closing = true;
        LOG("[quick-replace] selector %s\n", pressed & 2 ? "canceled" :
            replace_outgoing_pending ? "closed; awaiting native outgoing item" :
            replace_confirmed ? "replacement confirmed" : "confirmed");
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

int rt_quick_replace_toast_begin(Cpu *c) {
    if (!replace_toast || c->x[0] != replace_toast || replace_category < 0) return -1;
    int saved = LD8(replace_toast + 0x1a8);
    ST8(replace_toast + 0x1a8, 1);
    return saved;
}

void rt_quick_replace_toast_end(uint64_t toast, int saved) {
    if (saved < 0) return;
    ST8(toast + 0x1a8, saved);
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
    uint64_t mgr = LD64(g_site.screen_mgr);
    if (!replace_range(mgr, 0x38) || LD32(mgr + 0x28) <= 0xc) return 0;
    uint64_t screens = LD64(mgr + 0x30);
    if (!replace_range(screens, 0x68)) return 0;
    uint64_t screen = LD64(screens + 0x60);
    if (!replace_range(screen, 0x38)) return 0;
    uint64_t layout = LD64(screen + 0x30);
    return replace_range(layout, 0x20) ? LD64(layout + 0x18) : 0;
}

static uint64_t replace_find_pane(uint64_t, const char *, unsigned, unsigned *);

static uint64_t replace_world_item_pane(void) {
    // MainScreen3D_00 (screen 2) owns Pa_NoticeItem_00: the world item's
    // name and comparison arrow. Exclude this exact subtree before capture.
    if (!quick_replace_enabled() || (replace_category < 0 && !replace_chest_wait)) return 0;
    uint64_t mgr = LD64(g_site.screen_mgr);
    if (!replace_range(mgr, 0x38) || LD32(mgr + 0x28) <= 2) return 0;
    uint64_t screens = LD64(mgr + 0x30);
    if (!replace_range(screens, 0x18)) return 0;
    uint64_t screen = LD64(screens + 0x10);
    if (!replace_range(screen, 0x38)) return 0;
    uint64_t layout = LD64(screen + 0x30);
    if (!replace_range(layout, 0x20)) return 0;
    unsigned budget = 32;
    return replace_find_pane(LD64(layout + 0x18), "Pa_NoticeItem_00", 2, &budget);
}

bool rt_quick_replace_pane(Cpu *c) {
    if (replace_selector_blur_pane && c->x[0] == replace_selector_blur_pane) return true;
    // Omit the owned subtree from the HUD captured by the selector blur.
    if (c->x[0] == replace_toast_pane() && !replace_toast_foreground) return true;
    if (c->x[0] == replace_action_pane()) return true;
    if (c->x[0] == replace_world_item_pane()) return true;
    return false;
}

bool rt_quick_replace_anim(Cpu *c) {
    if (replace_category >= 0 && replace_range(replace_toast, 0x120)) {
        for (unsigned i = 0; i < 2; i++) {
            if (c->x[0] != LD64(replace_toast + 0x110 + i * 8)) continue;
            uint64_t present = renderer_present_count() + 1;
            if (replace_anim_present[i] == present) return true;
            replace_anim_present[i] = present;
            break;
        }
    }
    return false;
}

static void replace_toast_animate(Cpu *c, uint64_t component) {
    uint64_t clock = LD64(g_site.screen_mgr);
    if (!replace_range(clock, 0xb24)) return;
    uint32_t delta = LD32(clock + 0xb20);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t anim = LD64(component + 0x110 + i * 8);
        if (!replace_range(anim, 0x58)) continue;
        uint64_t vt = LD64(anim);
        if (!replace_range(vt, 0x30) || LD64(vt + 0x18) != g_site.replace_anim_advance) continue;
        if (replace_anim_present[i] == renderer_present_count() + 1) continue;
        replace_call(c, LD64(vt + 0x28), anim, 0, 0);
        Cpu animation = *c;
        animation.v[0] = delta;
        animation.x[0] = anim;
        guest_call(&animation, g_site.replace_anim_advance);
    }
}

static void replace_toast_prepare(Cpu *c) {
    uint64_t hud = replace_hud();
    if (!replace_range(hud, 0x3668)) return;
    uint64_t component = LD64(hud + 0x3660);
    if (!replace_range(component, 0x390) || LD64(component) != g_site.replace_toast_vtable) return;
    replace_toast = component;
    if (!replace_toast_started) {
        replace_call(c, g_site.replace_toast_start, component, 1, 0);
        replace_toast_started = true;
        uint64_t ctrl = LD64(component + 0x20);
        LOG("[quick-replace] native toast started component=%" PRIx64 " ctrl=%" PRIx64
            " pane=%" PRIx64 " state=%u queued=%u\n", component, ctrl,
            replace_range(ctrl, 0x28) ? LD64(ctrl + 0x18) : 0,
            LD32(component + 0x104), LD32(component + 0x200));
    }
    if (replace_screen) {
        if (replace_toast_tick != renderer_present_count())
            replace_call(c, g_site.replace_component_update, component, 0, 0);
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

static void replace_selector_native_draw(Cpu *c, uint64_t filtered_pane, uint64_t focus, GuestFn native) {
    uint64_t saved = replace_selector_blur_pane;
    uint64_t pictures[4] = {0};
    uint8_t colors[4][16];
    const char *corners[] = {"N_LT_00", "N_RT_00", "N_LD_00", "N_RD_00"};
    const char *arrows[] = {"P_ArrowS_00", "P_ArrowS_01", "P_ArrowS_02", "P_ArrowS_03"};
    for (unsigned i = 0; focus && i < 4; i++) {
        unsigned budget = 128;
        uint64_t corner = replace_find_pane(focus, corners[i], 12, &budget);
        budget = 64;
        uint64_t picture = replace_find_pane(corner, arrows[i], 8, &budget);
        // Verified 1.9.0 Picture and game Picture-derived vptrs. Both retain
        // getter/setter slots +20/+28 = 289bc/289c8 and vertices +e8..f7.
        // Native selected corners stay separate from the blue equipped cell.
        if (!replace_range(picture, 0xf8)) continue;
        uint64_t vptr = LD64(picture);
        if (vptr != (g_site.replace_picture_vtable_base + 0x10) && vptr != g_site.replace_game_picture_vtable) continue;
        pictures[i] = picture;
        memcpy(colors[i], (void *)(uintptr_t)(picture + 0xe8), 16);
        for (unsigned vertex = 0; vertex < 4; vertex++) {
            ST8(picture + 0xe8 + vertex * 4, 255);
            ST8(picture + 0xe9 + vertex * 4, 218);
            ST8(picture + 0xea + vertex * 4, 68);
        }
    }
    // Picture's +a0 calculation copies vertex colors into its render data;
    // tint before recalculating, then the caller recalculates restored colors.
    if (focus) {
        uint64_t layout = LD64(c->x[0] + 0x30), info = LD64(c->x[0] + 0x68);
        replace_call(c, g_site.replace_layout_calculate, layout, info, 0);
    }
    replace_selector_blur_pane = filtered_pane;
    native(c);
    replace_selector_blur_pane = saved;
    for (unsigned i = 0; i < 4; i++) if (pictures[i])
        memcpy((void *)(uintptr_t)(pictures[i] + 0xe8), colors[i], 16);
}

static void replace_selector_draw(Cpu *c, GuestFn native) {
    if (!replace_card_open || c->x[0] != replace_screen) { replace_selector_native_draw(c, 0, 0, native); return; }
    uint64_t layout = LD64(replace_screen + 0x30), info = LD64(replace_screen + 0x68);
    if (!replace_range(layout, 0x20) || !replace_range(info, 0x100)) { replace_selector_native_draw(c, 0, 0, native); return; }
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
    replace_call(c, g_site.replace_layout_calculate, layout, info, 0);
    // MainShortCut_00 presents the original world through P_Capture_00,
    // then its MainShortCut-filtered capture through P_Capture_01. Keep the
    // original native world capture and omit only this selector's blur layer.
    unsigned blur_budget = 512;
    bool style = quick_replace_active() && quick_replace_enabled();
    uint64_t filtered = style ? replace_find_pane(root, "P_Capture_01", 20, &blur_budget) : 0;
    unsigned focus_budget = 512;
    uint64_t focus = style ? replace_find_pane(root, "N_FocusL_00", 20, &focus_budget) : 0;
    replace_selector_native_draw(c, filtered, focus, native);
    for (unsigned i = 0; i < 3; i++) if (panes[i]) {
        ST32(panes[i] + 0x34, saved[i]); ST8(panes[i] + 0x58, LD8(panes[i] + 0x58) | 0x10);
    }
    ST64(info + 0xf8, context);
    replace_call(c, g_site.replace_layout_calculate, layout, info, 0);
    ST64(info + 0xf8, saved_context);
}

typedef struct {
    uint64_t panes[16];
    uint8_t flags[16];
    unsigned count;
    uint64_t shifted[4];
    uint32_t x[4];
    uint8_t shift_flags[4];
    unsigned shift_count;
} ReplaceCardStyle;

static void replace_card_hide_pane(ReplaceCardStyle *style, uint64_t pane) {
    if (!pane || style->count == 16) return;
    for (unsigned i = 0; i < style->count; i++) if (style->panes[i] == pane) return;
    unsigned i = style->count++;
    style->panes[i] = pane; style->flags[i] = LD8(pane + 0x58);
    ST8(pane + 0x58, (style->flags[i] & ~1u) | 0x10);
}

static uint64_t replace_card_child(uint64_t root, const char *name) {
    unsigned budget = 128;
    return replace_find_pane(root, name, 2, &budget);
}

static ReplaceCardStyle replace_card_style_begin(uint64_t root) {
    ReplaceCardStyle style = {0};
    if (!quick_replace_active() || replace_closing ||
        !replace_card_open || !quick_replace_enabled()) return style;
    // MessageGet_00: both the ordinary and captured Inventory/+ guides.
    uint64_t capture = replace_card_child(root, "N_Capture_00");
    replace_card_hide_pane(&style, replace_card_child(capture, "N_State_01"));
    uint64_t ordinary = replace_card_child(root, "N_InOut_00");
    uint64_t glow = replace_card_child(ordinary, "N_Sp3_00");
    ordinary = replace_card_child(glow, "N_Sp_00");
    replace_card_hide_pane(&style, replace_card_child(ordinary, "N_State_00"));
    const char *gears[] = {"Pa_Gear_00", "Pa_Gear_01"};
    const char *old_stat[] = {"T_OldNum_00", "W_Base_00", "W_BaseLine_00",
                              "P_NumberIconOld_00", "P_ArrowS_01"};
    for (unsigned i = 0; i < 2; i++) {
        uint64_t icon = replace_card_child(i ? capture : ordinary, i ? "N_Icon_01" : "N_Icon_00");
        uint64_t gear = replace_card_child(icon, gears[i]);
        unsigned budget = 256;
        uint64_t parameter = replace_find_pane(gear, "Pa_Param_00", 16, &budget);
        // PaPauseInfoParam_00 keeps incoming T_Num_00/its _01 frames and
        // category icon visible. The modifier remains a complete native part.
        for (unsigned j = 0; parameter && j < 5; j++) {
            budget = 128;
            replace_card_hide_pane(&style, replace_find_pane(parameter, old_stat[j], 12, &budget));
        }
        budget = 128;
        uint64_t all = replace_find_pane(parameter, "N_All_00", 8, &budget);
        uint64_t color = replace_card_child(all, "N_Color_00");
        uint64_t incoming = replace_card_child(color, "T_Num_00");
        uint64_t old = replace_card_child(all, "T_OldNum_00");
        if (color && incoming && old) {
            // Native PaPauseInfoParam_00 places incoming number/frame together
            // under N_Color_00. Align that group with the old number's slot;
            // P_Icon_00 stays fixed outside this group.
            unsigned moved = style.shift_count++;
            style.shifted[moved] = color; style.x[moved] = LD32(color + 0x30);
            style.shift_flags[moved] = LD8(color + 0x58);
            uint32_t old_bits = LD32(old + 0x30), incoming_bits = LD32(incoming + 0x30);
            float old_x, incoming_x, destination;
            memcpy(&old_x, &old_bits, 4); memcpy(&incoming_x, &incoming_bits, 4);
            destination = old_x - incoming_x;
            uint32_t moved_bits; memcpy(&moved_bits, &destination, 4);
            ST32(color + 0x30, moved_bits);
            ST8(color + 0x58, style.shift_flags[moved] | 0x10);
            budget = 128;
            uint64_t modifier = replace_find_pane(gear, "Pa_Param_01", 16, &budget);
            if (modifier) {
                // Pa_Param_00/01 have native unit scale and share N_Align_01.
                // Preserve their existing spacing for every modifier kind.
                float original_x, modifier_x;
                memcpy(&original_x, &style.x[moved], 4);
                unsigned mod = style.shift_count++;
                style.shifted[mod] = modifier; style.x[mod] = LD32(modifier + 0x30);
                style.shift_flags[mod] = LD8(modifier + 0x58);
                memcpy(&modifier_x, &style.x[mod], 4);
                modifier_x += destination - original_x;
                memcpy(&moved_bits, &modifier_x, 4);
                ST32(modifier + 0x30, moved_bits);
                ST8(modifier + 0x58, style.shift_flags[mod] | 0x10);
            }
        }
    }
    return style;
}

static void replace_card_style_end(const ReplaceCardStyle *style) {
    for (unsigned i = 0; i < style->count; i++)
        ST8(style->panes[i] + 0x58, style->flags[i] | 0x10);
    for (unsigned i = 0; i < style->shift_count; i++) {
        ST32(style->shifted[i] + 0x30, style->x[i]);
        ST8(style->shifted[i] + 0x58, style->shift_flags[i] | 0x10);
    }
}

void rt_quick_replace_ui_draw(Cpu *c, GuestFn native) {
    Cpu before = *c;
    if (replace_card_open && replace_category >= 0 && c->x[0] == replace_card) return;
    if (replace_card_open && replace_chest_wait && replace_chest && !replace_closing &&
        quick_replace_enabled() && c->x[0] == replace_card) {
        // Open/update the native card internally, but keep it out of the
        // world image captured before the quick selector. FE=2 is advanced
        // by native f6f09c, independently of this draw-only callback.
        return;
    }
    replace_selector_draw(c, native);
    if (replace_card_open && replace_screen && before.x[0] == replace_screen) {
        Cpu card_draw = before;
        card_draw.x[0] = replace_card;
        uint8_t visible = LD8(replace_card + 0x100), hidden = LD8(replace_card + 0x102);
        uint8_t state = LD8(replace_card + 0xfe), suppression = LD8(replace_card + 0xff);
        uint64_t layout = LD64(replace_card + 0x30), info = LD64(replace_card + 0x68);
        if (replace_range(layout, 0x20) && replace_range(info, 0x100)) {
            uint64_t saved_context = LD64(info + 0xf8);
            ST64(info + 0xf8, before.x[1]);
            ReplaceCardStyle style = replace_card_style_begin(LD64(layout + 0x18));
            replace_call(&before, g_site.replace_layout_calculate, layout, info, 0);
            // The selector suppresses other screens. Only this owned card is
            // drawn here; restore every native visibility/state byte afterward.
            ST8(replace_card + 0x100, 1); ST8(replace_card + 0x102, 0);
            ST8(replace_card + 0xfe, 2); ST8(replace_card + 0xff, 0);
            native(&card_draw);
            replace_card_style_end(&style);
            if (style.count || style.shift_count) {
                ST64(info + 0xf8, before.x[1]);
                replace_call(&before, g_site.replace_layout_calculate, layout, info, 0);
            }
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
    replace_call(&before, g_site.replace_layout_calculate, layout, info, 0);
    ST64(info + 0xc0, layout);
    ST16(info + 0xeb, 6); ST8(info + 0xed, 0); ST8(info + 0xef, 1);
    if (++replace_toast_draw_count <= 2)
        LOG("[quick-replace] native toast draw caller=%" PRIx64 " pane=%" PRIx64 " fn=%" PRIx64 "\n",
            before.x[30], pane, draw);
    replace_toast_foreground = true;
    replace_call(&before, draw, pane, info, commands + 8);
    replace_toast_foreground = false;
    replace_call(&before, g_site.replace_draw_info_reset, info, 0, 0);
    ST64(info + 0xf8, saved_context); ST64(info + 0xc0, saved_layout);
    ST16(info + 0xeb, saved_flags); ST8(info + 0xed, saved_ed); ST8(info + 0xef, saved_ef);
}

static void replace_finish_close(Cpu *c) {
    if (replace_confirmed && !replace_chest && replace_source_valid(c, false))
        replace_retry_until = renderer_present_count() + 3;
    else replace_ground_release(c);
    replace_card_close(c);
    replace_category = -1;
    replace_ground_preview = false;
    replace_screen = 0;
    replace_outgoing_pending = false;
    replace_source_lost = false;
    // Preserve only an opaque callback identity. AI memory is inspected again
    // only if the game's native AI update supplies that pointer as current.
    replace_resume_chest = replace_chest && replace_chest_owner_live(c) ? replace_chest : 0;
    replace_chest = 0;
    if (!replace_resume_chest) {
        replace_chest_release(c);
        memset(replace_incoming, 0, sizeof replace_incoming);
    }
}

static void replace_pending_tick(Cpu *c, uint64_t manager, GuestFn native) {
    bool identity = replace_chest ? replace_chest_owner_live(c) && atomic_load(&replace_chest_context_current) :
        replace_ground_identity(c) && (replace_ground_target(c, true) || replace_ground_target(c, false));
    if (!identity || replace_active_event()) replace_source_lost = true;
    // Retain ownership without fabricating an opening direction. Mask the old
    // direction until native close completes, then let ordinary world ticks run.
    uint64_t input = LD64(manager + 0x649f0);
    bool mask = replace_range(input, 0x118) &&
        (LD32(manager + 0x64c24) == 3 || LD32(manager + 0x64c2c));
    uint32_t held = mask ? LD32(input + 0x114) : 0, edge = mask ? LD32(input + 8) : 0;
    if (mask) { ST32(input + 0x114, held & ~0xf0000u); ST32(input + 8, edge & ~0xf0000u); }
    native(c);
    if (mask) { ST32(input + 0x114, held); ST32(input + 8, edge); }
    QuickDropResult result = quick_drop_action_result();
    if (result == QUICK_DROP_PENDING || LD32(manager + 0x64c24) == 3 || LD32(manager + 0x64c2c)) return;
    replace_confirmed = result == QUICK_DROP_COMPLETE && !replace_source_lost &&
        !replace_active_event() && replace_source_valid(c, false);
    LOG("[quick-replace] outgoing finished result=%d incoming_valid=%d; native receive %s\n",
        result, !replace_source_lost, replace_confirmed ? "released" : "canceled");
    replace_finish_close(c);
}

void rt_quick_replace_tick(Cpu *c, GuestFn native) {
    uint64_t manager = c->x[0];
    if (!replace_range(manager, 0x64c60)) { native(c); return; }
    if (replace_retry_until && (renderer_present_count() >= replace_retry_until || !replace_ground_target(c, false))) {
        replace_retry_until = 0;
        replace_ground_release(c);
    }
    if (replace_outgoing_pending) { replace_pending_tick(c, manager, native); return; }
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
        replace_capture_discovery_preview(c);
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
            replace_ground_preview = false;
            replace_ground_release(c);
            LOG("[quick-replace] discovery source released without replacement\n");
        }
    }
    uint64_t input = LD64(manager + 0x649f0);
    if (replace_category < 0 || !replace_range(input, 0x118)) { native(c); return; }
    replace_toast_prepare(c);
    if (++replace_frames > 180 && !replace_screen) {
        LOG("[quick-replace] selector open timed out; inventory unchanged\n");
        replace_card_close(c);
        replace_category = -1;
        replace_ground_preview = false;
        replace_resume_chest = replace_chest;
        replace_chest = 0;
        if (replace_ground_actor(c)) {
            Cpu scratch;
            if (replace_scratch(c, &scratch)) {
                ST32(scratch.sp + 0x30, 7);
                scratch.x[0] = replace_handle;
                scratch.x[1] = scratch.sp + 0x30;
                replace_popup_suppressed = true;
                replace_call(&scratch, g_site.replace_popup, scratch.x[0], scratch.x[1], scratch.x[2]);
                replace_popup_suppressed = false;
            }
        }
        replace_ground_release(c);
        memset(replace_incoming, 0, sizeof replace_incoming);
        native(c);
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
    native(c);
    ST32(input + 0x114, held);
    ST32(input + 8, edge);
    if (replace_closing && LD32(manager + 0x64c24) != 3 && LD32(manager + 0x64c2c) == 0) {
        LOG("[quick-replace] native close completed\n");
        replace_finish_close(c);
    }
}

// Called after consumption in HID sampling; the native pickup action still
// performs the award, including modifiers/durability, through its normal A path.
uint64_t quick_replace_buttons(uint64_t buttons) {
    if (!replace_retry_until) return buttons;
    if (renderer_present_count() >= replace_retry_until || !replace_handle || !LD64(replace_handle + 8)) return buttons;
    // HID cannot resolve a guest ActorLink. Its fixed native control block is
    // safe to inspect without touching the old actor if teardown invalidated
    // the source between the last guest validation and this input sample.
    if (!replace_index_identity(replace_ground_index, LD64(replace_handle + 8))) return buttons;
    return buttons | 1;
}

bool quick_replace_init(void) {
    if (strcmp(g_game_version, "1.9.0")) return false;
    if (replace_ready) return true;
    const uint64_t helpers[] = {
        g_site.actor_index_assign, g_site.actor_index_release, g_site.actor_index_resolve,
        g_site.actor_link_assign, g_site.actor_link_copy, g_site.actor_link_release, g_site.pouch_can_receive,
        g_site.pouch_item_type, g_site.pouch_lock, g_site.pouch_page_count, g_site.pouch_page_item,
        g_site.pouch_unlock, g_site.replace_ai_child_matches, g_site.replace_anim_advance,
        g_site.replace_card_close, g_site.replace_card_prepare, g_site.replace_component_update,
        g_site.replace_draw_info_reset, g_site.replace_interaction_target, g_site.replace_item_popup,
        g_site.replace_layout_calculate, g_site.replace_pickup_status, g_site.replace_popup,
        g_site.replace_string_assign, g_site.replace_toast_start, g_site.replace_weapon_modifier
    };
    for (size_t i = 0; i < sizeof helpers / sizeof *helpers; i++)
        if (!helpers[i] || !dispatch_lookup(helpers[i])) return false;
    const uint64_t data[] = {
        g_site.event_mgr, g_site.replace_card_manager_slot, g_site.replace_chest_closed_wait_name,
        g_site.replace_game_picture_vtable, g_site.replace_interaction_manager_slot,
        g_site.replace_picture_vtable_base, g_site.replace_pouch_manager_slot,
        g_site.replace_safe_string_vtable_slot, g_site.replace_toast_vtable, g_site.screen_mgr
    };
    for (size_t i = 0; i < sizeof data / sizeof *data; i++)
        if (!data[i]) return false;
    if (!quick_drop_action_init()) return false;
    replace_ready = true;
    return true;
}
