// Verified BOTW 1.9.0 quick drop: refresh unequipped items in the selector;
// close it and await native actor detach for equipped items.
#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>
#include "game_sites.h"
#include "hle.h"
#include "settings.h"
#include "quick_drop_action.h"

extern uint64_t input_buttons_now(void);
extern void input_consume_buttons(uint64_t);
extern uint64_t renderer_present_count(void);

enum { DROP_NAME_CAP = 97 };
static bool drop_ready;
static const char *drop_reason = "not-checked";
typedef struct {
    uint64_t screen, mgr, item, player;
    int type, value, category;
    bool equipped;
    char name[DROP_NAME_CAP];
} DropRequest;
static DropRequest pending_drop;
static QuickDropResult drop_result;
static uint64_t drop_links;
static uint64_t drop_ui_input;
static _Atomic uint64_t drop_generation;
static _Atomic bool drop_actor_consumed;
static struct {
    _Atomic bool active;
    bool recovery;
    uint64_t player, actor_link, actor_id, mgr, started;
    int actor_category;
    char name[DROP_NAME_CAP];
} equipped_drop;

QuickDropResult quick_drop_action_result(void) { return drop_result; }
bool quick_drop_action_busy(void) { return atomic_load(&equipped_drop.active); }

uint64_t quick_drop_action_filter_buttons(uint64_t buttons) {
    if (!quick_drop_action_busy()) return buttons;
    // Bits16..23 are derived stick directions, not equipment/menu actions.
    // Include SL/SR and any other injected action bits outside that range.
    uint64_t actions = buttons & ~UINT64_C(0xff0000);
    input_consume_buttons(actions);
    return buttons & UINT64_C(0xff0000);
}

static void drop_clear_ui_actions(void) {
    if (drop_ui_input >= GUEST_LO && drop_ui_input <= GUEST_HI - 0x118) {
        ST32(drop_ui_input + 0x114, 0);
        ST32(drop_ui_input + 8, 0);
    }
}

RT_ON_RESET(quick_drop_action_reset) {
    drop_ready = false;
    atomic_store(&drop_generation, 0);
    memset(&pending_drop, 0, sizeof pending_drop);
    memset(&equipped_drop, 0, sizeof equipped_drop);
    drop_result = QUICK_DROP_REJECTED;
    drop_links = 0;
    drop_ui_input = 0;
    atomic_store(&drop_actor_consumed, false);
}

bool quick_drop_action_enabled(void) {
    return g_settings.quick_drop && !strcmp(g_game_version, "1.9.0");
}

static bool drop_range(uint64_t addr, uint64_t size) {
    return addr >= GUEST_LO && addr < GUEST_HI && size <= GUEST_HI - addr;
}

// Guest code may only see scratch in its own address space. Leave a 0x100-byte
// area above the lowered SP for ByamlIter/name data, and preserve caller state.
static uint64_t drop_call(Cpu *source, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t x2) {
    Cpu call = *source;
    call.x[0] = x0;
    call.x[1] = x1;
    call.x[2] = x2;
    call.x[3] = 0;
    guest_call(&call, fn);
    return call.x[0];
}

static bool drop_scratch(Cpu *original, Cpu *scratch) {
    if ((original->sp & 15) || original->sp < GUEST_LO + 0x100 ||
        !drop_range(original->sp - 0x100, 0x100)) return false;
    *scratch = *original;
    scratch->sp -= 0x100;
    return true;
}

static bool drop_name(uint64_t item, char out[DROP_NAME_CAP]) {
    uint64_t addr = LD64(item + 0x30);
    if (!drop_range(addr, DROP_NAME_CAP)) return false;
    for (int i = 0; i < DROP_NAME_CAP; i++) {
        unsigned ch = LD8(addr + i);
        if (!ch) { out[i] = 0; return i > 0; }
        if (ch < 32 || ch > 126) return false;
        out[i] = (char)ch;
    }
    return false;
}

static bool drop_item_valid(uint64_t item, int type, int value, const char *name, bool equipped) {
    char current[DROP_NAME_CAP];
    return drop_range(item, 0x38) && (type == 0 || type == 1 || type == 3) && value > 0 &&
        (int32_t)LD32(item + 0x18) == type && (int32_t)LD32(item + 0x20) == value &&
        !!LD8(item + 0x24) == equipped && LD8(item + 0x25) && drop_name(item, current) && !strcmp(current, name) &&
        strcmp(name, "Weapon_Sword_070") && strcmp(name, "Weapon_Sword_502");
}

static bool drop_reject(const char *reason) { drop_reason = reason; return false; }

static bool drop_allowed(Cpu *scratch, uint64_t item, int type, int value, const char *name, bool equipped) {
    if (type != 0 && type != 1 && type != 3) return drop_reject("unsupported-type");
    if (!drop_range(item, 0x38) || !LD8(item + 0x25) || value <= 0)
        return drop_reject("invalid-item");
    if (!strcmp(name, "Weapon_Sword_070") || !strcmp(name, "Weapon_Sword_502"))
        return drop_reject("protected-name");
    if (!drop_item_valid(item, type, value, name, equipped)) return drop_reject("stale-item");
    if (drop_call(scratch, g_site.quick_drop_trash_prohibited, 0, 0, 0)) return drop_reject("TrashProhibition");
    if (drop_call(scratch, g_site.quick_sort_player_busy, 0, 0, 0)) return drop_reject("ChangeEquipProhibition");
    if (drop_call(scratch, g_site.quick_sort_event, 0, 0, 0)) return drop_reject("active-event");
    if (drop_call(scratch, g_site.quick_sort_modal, 0, 0, 0)) return drop_reject("screen-0xb-open");
    if (!drop_range(g_site.actor_info, 8)) return drop_reject("missing-actor-info-site");
    uint64_t info = LD64(g_site.actor_info);
    if (!drop_range(g_site.player_info, 8)) return drop_reject("missing-player-info-site");
    uint64_t player_info = LD64(g_site.player_info);
    if (!drop_range(info, 0x80) || !drop_range(player_info, 0x68))
        return drop_reject("missing-actor-or-player-info");
    uint64_t player = drop_call(scratch, g_site.player_actor, player_info, 0, 0);
    if (!drop_range(player, 0xcf9)) return drop_reject("missing-player");
    if ((LD8(player + 0xcf8) & 4) || drop_call(scratch, g_site.player_drop_forbidden, player, 0, 0))
        return drop_reject("player-state");
    uint64_t iter = scratch->sp + 0x10, guest_name = scratch->sp + 0x30;
    memset((void *)(uintptr_t)iter, 0, 16);
    memcpy((void *)(uintptr_t)guest_name, name, strlen(name) + 1);
    // Reject unknown actors explicitly; hasTag=false alone cannot distinguish
    // a failed lookup from a known actor without that tag.
    if (!drop_call(scratch, g_site.actor_info_lookup, info, iter, guest_name))
        return drop_reject("unknown-actor");
    if (drop_call(scratch, g_site.actor_info_has_tag, info, iter, 0x4850af43u))
        return drop_reject("Important-tag");
    if (drop_call(scratch, g_site.actor_info_has_tag, info, iter, 0x4873f474u))
        return drop_reject("NotDropFromPlayer-tag");
    drop_reason = "allowed";
    return true;
}

// Called only while holding the native manager mutex. Item identity, not the
// quick-list index or name, determines the page/slot passed to discard.
static bool drop_locate(Cpu *scratch, uint64_t mgr, uint64_t item, int *tab_out, int *slot_out) {
    int tabs = (int32_t)LD32(mgr + 0x44498), matches = 0;
    if (tabs < 1 || tabs > 50) return false;
    for (int tab = 0; tab < tabs; tab++) {
        int count = (int32_t)drop_call(scratch, g_site.pouch_page_count, mgr, tab, 0);
        if (count < 0 || count > 20) return false;
        for (int slot = 0; slot < count; slot++) {
            if (drop_call(scratch, g_site.pouch_page_item, mgr, tab, slot) == item) {
                *tab_out = tab;
                *slot_out = slot;
                matches++;
            }
        }
    }
    return matches == 1;
}

static bool drop_cache_compatible(uint64_t cached, uint64_t item, bool equipped) {
    // This is a scoped pause-UI snapshot: native quick selectors normally
    // leave it empty. A conflicting populated snapshot still fails closed.
    return equipped ? !cached || cached == item : cached != item;
}

// The equipped flag is authoritative only when one live category entry owns
// it. Pin that identity before preparing the native pause-style snapshot.
static bool drop_unique_equipped(Cpu *cpu, uint64_t mgr, uint64_t item, int type) {
    int tabs = (int32_t)LD32(mgr + 0x44498), equipped = 0;
    bool target = false;
    if (tabs < 1 || tabs > 50) return false;
    for (int tab = 0; tab < tabs; tab++) {
        int count = (int32_t)drop_call(cpu, g_site.pouch_page_count, mgr, tab, 0);
        if (count < 0 || count > 20) return false;
        for (int slot = 0; slot < count; slot++) {
            uint64_t candidate = drop_call(cpu, g_site.pouch_page_item, mgr, tab, slot);
            if (!candidate) continue;
            if (!drop_range(candidate, 0x26)) return false;
            if ((int32_t)LD32(candidate + 0x18) == type && LD8(candidate + 0x24)) {
                equipped++;
                target |= candidate == item;
            }
        }
    }
    return equipped == 1 && target;
}

static void drop_execute(Cpu *c);
static bool drop_equipped_begin(Cpu *, const DropRequest *, int tab, int slot);
static uint64_t drop_player(Cpu *);

// Runs before the original Menu callback, while its list is still stable.
void quick_drop_action_close(Cpu *c) { drop_execute(c); }

void quick_drop_action_request(Cpu *c, uint64_t screen, uint64_t mgr, uint64_t item,
                               int tab, int slot, const char *name) {
    if (equipped_drop.active || pending_drop.item) return;
    drop_result = QUICK_DROP_REJECTED;
    if (!drop_ready || !quick_drop_action_enabled() || pending_drop.item || !name ||
        !drop_range(screen, 0x38e5) || !drop_range(mgr, 0x45000) ||
        !drop_range(item, 0x38) || tab < 0 || tab >= 50 || slot < 0 || slot >= 20 ||
        strnlen(name, DROP_NAME_CAP) >= DROP_NAME_CAP) return;
    Cpu scratch;
    if (!drop_scratch(c, &scratch)) return;
    drop_call(&scratch, g_site.pouch_lock, mgr + 0x28, 0, 0);
    int count = (int32_t)drop_call(&scratch, g_site.pouch_page_count, mgr, tab, 0);
    int type = (int32_t)LD32(item + 0x18), value = (int32_t)LD32(item + 0x20);
    uint64_t equipped_cache = type >= 0 && type < 4 ? LD64(mgr + 0x447e0 + (uint64_t)type * 8) : 0;
    drop_reason = "inventory-slot-mismatch";
    bool equipped = !!LD8(item + 0x24);
    bool ok = count > slot && count <= 20 && drop_cache_compatible(equipped_cache, item, equipped) &&
        drop_call(&scratch, g_site.pouch_page_item, mgr, tab, slot) == item &&
        drop_allowed(&scratch, item, type, value, name, equipped);
    if (ok) {
        pending_drop.screen = screen;
        pending_drop.mgr = mgr;
        pending_drop.item = item;
        pending_drop.player = drop_player(&scratch);
        pending_drop.type = type;
        pending_drop.value = value;
        pending_drop.equipped = equipped;
        drop_result = QUICK_DROP_PENDING;
        pending_drop.category = (int32_t)LD32(screen + 0x3738);
        memcpy(pending_drop.name, name, strlen(name) + 1);
    }
    drop_call(&scratch, g_site.pouch_unlock, mgr + 0x28, 0, 0);
    if (ok) {
        LOG("[quick-drop] queued %s; updating open menu\n", name);

    } else LOG("[quick-drop] rejected %s reason=%s; inventory unchanged\n", name, drop_reason);
}

static void drop_execute(Cpu *c) {
    uint64_t screen = c->x[0];
    if (equipped_drop.active) return;
    if (!quick_drop_action_enabled()) { memset(&pending_drop, 0, sizeof pending_drop); drop_result = QUICK_DROP_REJECTED; return; }
    // A request belongs to the menu state in which it was queued. If that
    // screen has moved on, do not leave it armed for a later menu session.
    if (pending_drop.item && pending_drop.screen == screen &&
        (!drop_range(screen, 0x38e5) || (int32_t)LD32(screen + 0x3738) < 0 ||
         (int32_t)LD32(screen + 0x3738) > 2 ||
         (int32_t)LD32(screen + 0x3738) != pending_drop.category || LD8(screen + 0x38e4) == 2)) {
        memset(&pending_drop, 0, sizeof pending_drop);
        drop_result = QUICK_DROP_REJECTED;
        return;
    }
    if (pending_drop.item && pending_drop.screen == screen && drop_range(screen, 0x38e5) &&
        (int32_t)LD32(screen + 0x3738) >= 0 &&
        (int32_t)LD32(screen + 0x3738) <= 2 && LD8(screen + 0x38e4) != 2) {
        {
            Cpu scratch;
            if (drop_scratch(c, &scratch)) {
                // Consume the request before any guest code can re-enter this
                // callback. Every subsequent failure leaves inventory unchanged.
                __typeof__(pending_drop) request = pending_drop;
                memset(&pending_drop, 0, sizeof pending_drop);
                drop_result = QUICK_DROP_REJECTED;
                drop_call(&scratch, g_site.pouch_lock, request.mgr + 0x28, 0, 0);
                int tab = -1, slot = -1;
                drop_reason = "inventory-item-missing-or-ambiguous";
                bool ok = drop_locate(&scratch, request.mgr, request.item, &tab, &slot);
                if (ok && !drop_cache_compatible(LD64(request.mgr + 0x447e0 + (uint64_t)request.type * 8), request.item, request.equipped))
                    ok = drop_reject("changed-equipped-item");
                if (ok) ok = drop_allowed(&scratch, request.item, request.type, request.value, request.name, request.equipped);
                if (ok && request.equipped) {
                    if (!drop_equipped_begin(&scratch, &request, tab, slot))
                        LOG("[quick-drop] equipped drop rejected name=%s reason=%s; inventory unchanged\n", request.name, drop_reason);
                } else if (ok) {
                    // Confirmed recursive mutex: native ctor calls nn::os::
                    // InitializeMutex(recursive=true). Discard locks it again
                    // on the same guest thread while the outer lock pins identity.
                    drop_call(&scratch, g_site.pouch_discard, request.mgr, tab, slot);
                    if (!LD8(request.item + 0x25)) {
                        // Full pause UI follows trashItem with this sweep (calls
                        // at e38254/e3f978). It recycles invalid entries, rebuilds
                        // tab/list heads and saves. The old pointer is no longer
                        // a live PouchItem afterwards: do not dereference it.
                        int selected = (int32_t)LD32(screen + 0x387c);
                        int category = (int32_t)LD32(screen + 0x3738);
                        drop_call(&scratch, g_site.pouch_cleanup, request.mgr, 0, 0);
                        drop_result = QUICK_DROP_COMPLETE;
                        // Rebuild the native pointer array and equipped index;
                        // never retain a pointer recycled by the cleanup sweep.
                        drop_call(&scratch, g_site.quick_sort_refill, screen, category, 0);
                        int count = (int32_t)LD32(screen + 0x3880);
                        if (selected >= count) selected = count - 1;
                        if (selected < 0) selected = -1;
                        ST32(screen + 0x387c, selected);
                        drop_call(&scratch, g_site.quick_sort_mapping, screen, 0, 0);
                        int cursor = 0;
                        for (int i = 0; i < 21; i++)
                            if ((int32_t)LD32(screen + 0x3884 + i * 4) == selected) { cursor = i; break; }
                        // The native mapping also includes an unequip entry for
                        // ordinary swords, even when an item is highlighted.
                        // Derive the widget size from that rebuilt mapping, as
                        // category alone misses this slot and hides the last item.
                        int extra = (int32_t)LD32(screen + 0x3884) == -1 ||
                            selected == -1;
                        drop_call(&scratch, g_site.quick_sort_cursor, LD64(screen + 0x37a8), cursor, count + extra);
                        drop_call(&scratch, g_site.quick_sort_labels, screen, 0, 0);
                        int remaining_tab = -1, remaining_slot = -1;
                        bool still_listed = drop_locate(&scratch, request.mgr, request.item,
                                                       &remaining_tab, &remaining_slot);
                        LOG("[quick-drop] cleanup name=%s still_listed=%d\n", request.name, still_listed);
                    }
                } else LOG("[quick-drop] canceled %s reason=%s\n", request.name, drop_reason);
                drop_call(&scratch, g_site.pouch_unlock, request.mgr + 0x28, 0, 0);
            } else { memset(&pending_drop, 0, sizeof pending_drop); drop_result = QUICK_DROP_REJECTED; }
        }
    }
}


static uint64_t drop_pending_offset(int category) {
    return category == 0 ? 0x13e0 : category == 1 ? 0x1428 : 0x1470;
}

static uint64_t drop_player(Cpu *cpu) {
    if (!drop_range(g_site.player_info, 8)) return 0;
    uint64_t info = LD64(g_site.player_info);
    return drop_range(info, 0x68) ? drop_call(cpu, g_site.player_actor, info, 0, 0) : 0;
}

static bool drop_index_matches(uint64_t link, uint64_t actor) {
    if (!drop_range(link, 0x10) || !LD8(link + 0xc) || LD32(link + 8) == UINT32_MAX) return false;
    uint64_t block = LD64(link);
    return drop_range(block, 0x54) && LD32(block + 0x40) == LD32(link + 8) && LD64(block + 0x48) == actor;
}

// An indexed retained reference survives actor-job callbacks. Resolve it into
// a fresh ordinary link for this callback only: native actor jobs deliberately
// omit the ordinary reference count, relying on their current-job lifetime.
static bool drop_resolve(Cpu *cpu, uint64_t index, uint64_t local, uint64_t actor) {
    if (!drop_index_matches(index, actor)) return false;
    return drop_call(cpu, g_site.actor_index_resolve, index, local, 0) == actor &&
        LD64(local + 8) == actor && drop_range(actor, 0x980) && LD8(actor + 0x64) == 1;
}

static void drop_release_locals(Cpu *cpu) {
    drop_call(cpu, g_site.actor_link_release, drop_links + 0x20, 0, 0);
    drop_call(cpu, g_site.actor_link_release, drop_links + 0x38, 0, 0);
}

static void drop_release_indices(Cpu *cpu) {
    drop_call(cpu, g_site.actor_index_release, drop_links, 0, 0);
    drop_call(cpu, g_site.actor_index_release, drop_links + 0x10, 0, 0);
}

// Called with the source pouch identity pinned. All checks and reference
// acquisition precede invalidation; after trashItem this code never reads item.
static bool drop_equipped_begin(Cpu *cpu, const DropRequest *request, int tab, int slot) {
    int category = request->type == 0 ? 0 : request->type == 3 ? 1 : 2;
    if (!drop_unique_equipped(cpu, request->mgr, request->item, request->type))
        return drop_reject("equipped-item-ambiguous");
    uint64_t player = drop_player(cpu);
    if (player != request->player || !drop_range(player, 0x1471))
        return drop_reject("equipped-player-changed");
    drop_call(cpu, g_site.pouch_lock, player + 0xeb8, 0, 0);
    bool equip_pending = LD8(player + 0x1120 + category);
    drop_call(cpu, g_site.pouch_unlock, player + 0xeb8, 0, 0);
    uint64_t pending_mutex = player + drop_pending_offset(category) - 0x40;
    drop_call(cpu, g_site.pouch_lock, pending_mutex, 0, 0);
    bool drop_pending = LD8(player + drop_pending_offset(category));
    drop_call(cpu, g_site.pouch_unlock, pending_mutex, 0, 0);
    if (equip_pending || drop_pending) return drop_reject("equipped-category-already-pending");
    uint64_t actor = drop_call(cpu, g_site.player_equipment, player, category, 0);
    if (!drop_range(actor, 0xfa0) || LD8(actor + 0x64) != 1 ||
        LD8(actor + 0x934) || !LD64(actor + 0x190))
        return drop_reject("equipped-actor-not-stable");
    if (!drop_call(cpu, g_site.actor_link_alive, actor + 0x938, 0, 0))
        return drop_reject("equipped-actor-owner-missing");
    uint64_t vtable = LD64(actor);
    if (!drop_range(vtable, 0x590) || LD64(vtable + 0x588) != g_site.quick_drop_actor_method)
        return drop_reject("unsupported-equipped-drop-method");
    uint64_t name = LD64(actor + 0x10);
    if (!drop_range(name, DROP_NAME_CAP) || strncmp((const char *)(uintptr_t)name, request->name, DROP_NAME_CAP))
        return drop_reject("equipped-actor-name-changed");
    uint64_t unequip = drop_call(cpu, g_site.pouch_default_name, request->mgr, request->type, 0);
    if (!drop_range(unequip, 0x10)) return drop_reject("equipped-default-name-missing");
    if (!drop_links) {
        drop_links = (uint64_t)(uintptr_t)hle_alloc(0x50);
        if (drop_range(drop_links, 0x50)) {
            memset((void *)(uintptr_t)drop_links, 0, 0x50);
            ST32(drop_links + 8, UINT32_MAX);
            ST32(drop_links + 0x18, UINT32_MAX);
        }
    }
    if (!drop_range(drop_links, 0x50)) return drop_reject("equipped-link-storage");
    uint64_t actor_link_result = drop_call(cpu, g_site.actor_index_assign, drop_links, actor, 1);
    uint64_t player_link_result = drop_call(cpu, g_site.actor_index_assign, drop_links + 0x10, player, 1);
    if (!actor_link_result || !player_link_result ||
        !drop_resolve(cpu, drop_links, drop_links + 0x20, actor) ||
        !drop_resolve(cpu, drop_links + 0x10, drop_links + 0x38, player)) {
        LOG("[quick-drop] retained native link assignment actor_result=%" PRIu64 " player_result=%" PRIu64 "\n",
            actor_link_result, player_link_result);
        drop_release_locals(cpu);
        drop_release_indices(cpu);
        return drop_reject("equipped-actor-link-failed");
    }
    // Full pause modes populate these four words on entry and clear them on
    // exit; native quick mode does neither. Scope the same native snapshot to
    // trashItem's branch selection, under the already-held recursive lock.
    uint64_t saved_equipped[4];
    for (unsigned i = 0; i < 4; i++) saved_equipped[i] = LD64(request->mgr + 0x447e0 + i * 8);
    drop_call(cpu, g_site.pouch_equipped_snapshot, request->mgr, 0, 0);
    bool snapshot_matches = LD64(request->mgr + 0x447e0 + (uint64_t)request->type * 8) == request->item;
    if (!snapshot_matches) {
        for (unsigned i = 0; i < 4; i++) ST64(request->mgr + 0x447e0 + i * 8, saved_equipped[i]);
        drop_release_locals(cpu);
        drop_release_indices(cpu);
        return drop_reject("equipped-snapshot-mismatch");
    }
    equipped_drop.recovery = false;
    atomic_store(&drop_actor_consumed, false);
    equipped_drop.player = player;
    equipped_drop.actor_link = drop_links;
    equipped_drop.actor_id = actor;
    equipped_drop.actor_category = category;
    equipped_drop.mgr = request->mgr;
    equipped_drop.started = renderer_present_count();
    memcpy(equipped_drop.name, request->name, sizeof equipped_drop.name);
    drop_result = QUICK_DROP_PENDING;
    atomic_fetch_add(&drop_generation, 1);
    atomic_store(&equipped_drop.active, true);
    drop_clear_ui_actions();
    // Establish drop intent first. Another guest worker may run between these
    // native calls: it must never consume an unequip before the drop is queued.
    // All remaining arguments are saved values, never the invalidated item.
    drop_call(cpu, g_site.pouch_discard, request->mgr, tab, slot);
    // cbe9b0 queued only a player flag. No later consumer needs this snapshot;
    // restore its exact previous UI scope without touching the invalid item.
    for (unsigned i = 0; i < 4; i++) ST64(request->mgr + 0x447e0 + i * 8, saved_equipped[i]);
    // Native player virtual+858 schedules an empty category update and its
    // drop consumer under native locks. d9784c only queues the drop byte;
    // without this companion request idle players can leave it unconsumed.
    drop_call(cpu, g_site.player_schedule_equip, player, unequip, 0xffffffffu);
    // Close through the native selector transition without applying another
    // item. Consuming action buttons prevents reopening or equipping
    // incoming gear while the category-based native drop is queued.
    quick_drop_action_filter_buttons(input_buttons_now());
    ST32(request->screen + 0x387c, LD32(request->screen + 0x3878));
    ST8(request->screen + 0x38e4, 2);
    LOG("[quick-drop] equipped native drop pending name=%s category=%d actor=%" PRIx64 "; selector closing\n",
        equipped_drop.name, category, actor);
    drop_release_locals(cpu);
    return true;
}

void quick_drop_action_tick(Cpu *c) {
    if (!drop_ready || strcmp(g_game_version, "1.9.0")) return;
    // Remember this callback's native input before Menu::execute can admit a
    // request later in the same update. Admission clears these cached actions.
    drop_ui_input = 0;
    if (drop_range(c->x[0], 0x64c60)) {
        uint64_t input = LD64(c->x[0] + 0x649f0);
        if (drop_range(input, 0x118)) drop_ui_input = input;
    }
    if (!equipped_drop.active) return;
    drop_clear_ui_actions();
    quick_drop_action_filter_buttons(input_buttons_now());
    Cpu scratch;
    if (!drop_scratch(c, &scratch)) return;
    uint64_t player = equipped_drop.player, actor = equipped_drop.actor_id;
    bool identity = drop_player(&scratch) == player &&
        drop_resolve(&scratch, equipped_drop.actor_link, drop_links + 0x20, actor) &&
        drop_resolve(&scratch, drop_links + 0x10, drop_links + 0x38, player);
    if (identity && atomic_load(&drop_actor_consumed) && !LD8(player + 0x1120 + equipped_drop.actor_category) &&
        !LD8(player + drop_pending_offset(equipped_drop.actor_category)) &&
        !drop_call(&scratch, g_site.player_equipment, player, equipped_drop.actor_category, 0) &&
        LD8(actor + 0x920) == 0xff && !LD8(actor + 0x934) &&
        !drop_call(&scratch, g_site.actor_link_alive, actor + 0x938, 0, 0) &&
        !drop_call(&scratch, g_site.actor_link_alive, actor + 0x958, 0, 0)) {
        // Native1220268 has completed detach/placement and released owner links.
        // Only now recycle invalid pouch records; no saved PouchItem is read.
        drop_call(&scratch, g_site.pouch_lock, equipped_drop.mgr + 0x28, 0, 0);
        drop_call(&scratch, g_site.pouch_cleanup, equipped_drop.mgr, 0, 0);
        drop_call(&scratch, g_site.pouch_unlock, equipped_drop.mgr + 0x28, 0, 0);
        drop_release_indices(&scratch);
        drop_result = QUICK_DROP_COMPLETE;
        atomic_store(&equipped_drop.active, false);
        LOG("[quick-drop] equipped native drop completed name=%s actor=%" PRIx64 "\n", equipped_drop.name, actor);
    } else if (!equipped_drop.recovery && (!identity || renderer_present_count() - equipped_drop.started > 600)) {
        equipped_drop.recovery = true;
        int pending = -1, equip = -1, state = -1, detach = -1, owner = -1, other_owner = -1;
        uint64_t current = 0;
        // Only inspect actors protected by the same freshly resolved links used by
        // completion. This one-shot diagnostic does not admit or cancel work.
        if (identity) {
            pending = LD8(player + drop_pending_offset(equipped_drop.actor_category));
            equip = LD8(player + 0x1120 + equipped_drop.actor_category);
            state = LD8(actor + 0x920);
            detach = LD8(actor + 0x934);
            current = drop_call(&scratch, g_site.player_equipment, player, equipped_drop.actor_category, 0);
            owner = drop_call(&scratch, g_site.actor_link_alive, actor + 0x938, 0, 0) & 1;
            other_owner = drop_call(&scratch, g_site.actor_link_alive, actor + 0x958, 0, 0) & 1;
        }
        LOG("[quick-drop] equipped drop awaiting recovery name=%s identity=%d consumed=%d pending=%d equip=%d"
            " getter=%" PRIx64 " actor=%" PRIx64 " state=%d detach=%d owners=%d/%d; no rollback/requeue\n",
            equipped_drop.name, identity, atomic_load(&drop_actor_consumed), pending, equip,
            current, actor, state, detach, owner, other_owner);
    }
    drop_release_locals(&scratch);
}

// Capture an admitted transaction before the native virtual call. A later
// transaction cannot inherit this call's result, even if it reuses the actor.
uint64_t rt_quick_drop_actor_begin(uint64_t actor, uint64_t method) {
    if (!atomic_load(&equipped_drop.active) || actor != equipped_drop.actor_id ||
        method != g_site.quick_drop_actor_method || !drop_index_matches(equipped_drop.actor_link, actor)) return 0;
    return atomic_load(&drop_generation);
}

void rt_quick_drop_actor_end(uint64_t token, uint64_t actor, uint64_t result) {
    if (token && token == atomic_load(&drop_generation) && atomic_load(&equipped_drop.active) &&
        actor == equipped_drop.actor_id && drop_index_matches(equipped_drop.actor_link, actor) && (result & 1))
        atomic_store(&drop_actor_consumed, true);
}

bool quick_drop_action_init(void) {
    if (strcmp(g_game_version, "1.9.0")) return false;
    if (drop_ready) return true;
    if (!g_site.actor_info) return false;
    const uint64_t helpers[] = {g_site.pouch_lock, g_site.pouch_unlock, g_site.pouch_page_count, g_site.pouch_page_item,
        g_site.quick_drop_trash_prohibited, g_site.quick_sort_player_busy, g_site.quick_sort_event, g_site.quick_sort_modal, g_site.player_actor,
        g_site.player_drop_forbidden, g_site.actor_info_lookup, g_site.actor_info_has_tag, g_site.pouch_discard,
        g_site.player_equipment, g_site.pouch_default_name, g_site.player_schedule_equip, g_site.pouch_equipped_snapshot, g_site.actor_index_assign,
        g_site.actor_index_resolve, g_site.actor_index_release, g_site.actor_link_release, g_site.actor_link_alive, g_site.quick_drop_actor_method,
        g_site.pouch_cleanup, g_site.quick_sort_refill, g_site.quick_sort_mapping, g_site.quick_sort_cursor, g_site.quick_sort_labels};
    for (size_t i = 0; i < sizeof helpers / sizeof *helpers; i++)
        if (!helpers[i] || !dispatch_lookup(helpers[i])) {
            LOG("[quick-drop] missing native helper; action disabled\n");
            return false;
        }
    drop_ready = true;
    LOG("[quick-drop] guarded native quick drop enabled for 1.9.0\n");
    return true;
}
