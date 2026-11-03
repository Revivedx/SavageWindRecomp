// Opt-in BOTW 1.9.0 experiment: drop and refresh within the open quick menu.
#include <inttypes.h>
#include <string.h>
#include "game_sites.h"
#include "hle.h"
#include "settings.h"

extern uint64_t input_buttons_now(void);
extern bool quick_replace_card_drop_allowed(void);

enum { DROP_NAME_CAP = 97 };
static const uint64_t DROP_LOCK = 0x7000eaff08ull, DROP_UNLOCK = 0x7000eaff18ull;
static const uint64_t DROP_COUNT = 0x7000d970a8ull, DROP_GET = 0x7000d992acull;
static bool drop_ready;
static const char *drop_reason = "not-checked";
static struct {
    uint64_t screen, mgr, item;
    int type, value;
    char name[DROP_NAME_CAP];
} pending_drop;

RT_ON_RESET(quick_drop_action_reset) { memset(&pending_drop, 0, sizeof pending_drop); }

bool quick_drop_action_enabled(void) {
    return g_settings.quick_inventory && !strcmp(g_game_version, "1.9.0");
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

static bool drop_item_valid(uint64_t item, int type, int value, const char *name) {
    char current[DROP_NAME_CAP];
    return drop_range(item, 0x38) && (type == 0 || type == 1 || type == 3) && value > 0 &&
        (int32_t)LD32(item + 0x18) == type && (int32_t)LD32(item + 0x20) == value &&
        !LD8(item + 0x24) && LD8(item + 0x25) && drop_name(item, current) && !strcmp(current, name) &&
        strcmp(name, "Weapon_Sword_070") && strcmp(name, "Weapon_Sword_502");
}

static bool drop_reject(const char *reason) { drop_reason = reason; return false; }

static bool drop_allowed(Cpu *scratch, uint64_t item, int type, int value, const char *name) {
    if (type != 0 && type != 1 && type != 3) return drop_reject("unsupported-type");
    if (!drop_range(item, 0x38) || !LD8(item + 0x25) || value <= 0)
        return drop_reject("invalid-item");
    if (LD8(item + 0x24)) return drop_reject("equipped-item");
    if (!strcmp(name, "Weapon_Sword_070") || !strcmp(name, "Weapon_Sword_502"))
        return drop_reject("protected-name");
    if (!drop_item_valid(item, type, value, name)) return drop_reject("stale-item");
    if (drop_call(scratch, 0x7000d388b0ull, 0, 0, 0)) return drop_reject("TrashProhibition");
    if (drop_call(scratch, 0x7000d35b80ull, 0, 0, 0)) return drop_reject("ChangeEquipProhibition");
    if (drop_call(scratch, 0x7000d1499cull, 0, 0, 0)) return drop_reject("active-event");
    if (drop_call(scratch, 0x7000e7e750ull, 0, 0, 0) && !quick_replace_card_drop_allowed()) return drop_reject("screen-0xb-open");
    uint64_t info = LD64(0x7001e39450ull);
    if (!drop_range(g_site.player_info, 8)) return drop_reject("missing-player-info-site");
    uint64_t player_info = LD64(g_site.player_info);
    if (!drop_range(info, 0x80) || !drop_range(player_info, 0x68))
        return drop_reject("missing-actor-or-player-info");
    uint64_t player = drop_call(scratch, 0x7000cc6fd0ull, player_info, 0, 0);
    if (!drop_range(player, 0xcf9)) return drop_reject("missing-player");
    if ((LD8(player + 0xcf8) & 4) || drop_call(scratch, 0x7000cc0334ull, player, 0, 0))
        return drop_reject("player-state");
    uint64_t iter = scratch->sp + 0x10, guest_name = scratch->sp + 0x30;
    memset((void *)(uintptr_t)iter, 0, 16);
    memcpy((void *)(uintptr_t)guest_name, name, strlen(name) + 1);
    // Reject unknown actors explicitly; hasTag=false alone cannot distinguish
    // a failed lookup from a known actor without that tag.
    if (!drop_call(scratch, 0x700108dc0cull, info, iter, guest_name))
        return drop_reject("unknown-actor");
    if (drop_call(scratch, 0x700108e7a0ull, info, iter, 0x4850af43u))
        return drop_reject("Important-tag");
    if (drop_call(scratch, 0x700108e7a0ull, info, iter, 0x4873f474u))
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
        int count = (int32_t)drop_call(scratch, DROP_COUNT, mgr, tab, 0);
        if (count < 0 || count > 20) return false;
        for (int slot = 0; slot < count; slot++) {
            if (drop_call(scratch, DROP_GET, mgr, tab, slot) == item) {
                *tab_out = tab;
                *slot_out = slot;
                matches++;
            }
        }
    }
    return matches == 1;
}

static void drop_execute(Cpu *c);

// Runs before the original Menu callback, while its list is still stable.
void quick_drop_action_close(Cpu *c) { drop_execute(c); }

void quick_drop_action_request(Cpu *c, uint64_t screen, uint64_t mgr, uint64_t item,
                               int tab, int slot, const char *name) {
    if (!drop_ready || !quick_drop_action_enabled() || pending_drop.item || !name ||
        !drop_range(screen, 0x38e5) || !drop_range(mgr, 0x45000) ||
        !drop_range(item, 0x38) || tab < 0 || tab >= 50 || slot < 0 || slot >= 20 ||
        strnlen(name, DROP_NAME_CAP) >= DROP_NAME_CAP) return;
    Cpu scratch;
    if (!drop_scratch(c, &scratch)) return;
    drop_call(&scratch, DROP_LOCK, mgr + 0x28, 0, 0);
    int count = (int32_t)drop_call(&scratch, DROP_COUNT, mgr, tab, 0);
    int type = (int32_t)LD32(item + 0x18), value = (int32_t)LD32(item + 0x20);
    uint64_t equipped_cache = type >= 0 && type < 4 ? LD64(mgr + 0x447e0 + (uint64_t)type * 8) : 0;
    uint64_t trash_mgr = LD64(0x7001e0cb08ull);
    bool trash_available = drop_range(trash_mgr, 0x60);
    LOG("[quick-drop] preflight name=%s flag_equipped=%u cached_equipped=%" PRIx64
        " target=%" PRIx64 " trash_mgr=%" PRIx64 " trace_context=%" PRIx64 " active=%d capacity=%d free=%" PRIx64 "\n",
        name, LD8(item + 0x24), equipped_cache, item, trash_mgr,
        trash_available ? LD64(trash_mgr + 0x28) : 0,
        trash_available ? (int32_t)LD32(trash_mgr + 0x40) : -1,
        trash_available ? (int32_t)LD32(trash_mgr + 0x58) : -1,
        trash_available ? LD64(trash_mgr + 0x48) : 0);
    // Keep preflight mode available for diagnostics; the native launcher
    // explicitly enables execution after the same eligibility checks.
    const char *execute = getenv("SAVAGEWIND_QUICK_DROP_EXECUTE");
    const char *diagnostic = getenv("SAVAGEWIND_QUICK_DROP");
    if ((execute && strcmp(execute, "1")) ||
        (!execute && diagnostic && !strcmp(diagnostic, "1"))) {
        drop_call(&scratch, DROP_UNLOCK, mgr + 0x28, 0, 0);
        LOG("[quick-drop] preflight only; no menu or inventory change\n");
        return;
    }
    drop_reason = "inventory-slot-mismatch";
    bool ok = count > slot && count <= 20 && equipped_cache != item &&
        drop_call(&scratch, DROP_GET, mgr, tab, slot) == item &&
        drop_allowed(&scratch, item, type, value, name);
    if (ok) {
        pending_drop.screen = screen;
        pending_drop.mgr = mgr;
        pending_drop.item = item;
        pending_drop.type = type;
        pending_drop.value = value;
        memcpy(pending_drop.name, name, strlen(name) + 1);
    }
    drop_call(&scratch, DROP_UNLOCK, mgr + 0x28, 0, 0);
    if (ok) {
        LOG("[quick-drop] queued %s; updating open menu\n", name);

    } else LOG("[quick-drop] rejected %s reason=%s; inventory unchanged\n", name, drop_reason);
}

static void drop_execute(Cpu *c) {
    uint64_t screen = c->x[0];
    if (!quick_drop_action_enabled()) { memset(&pending_drop, 0, sizeof pending_drop); return; }
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
                drop_call(&scratch, DROP_LOCK, request.mgr + 0x28, 0, 0);
                int tab = -1, slot = -1;
                drop_reason = "inventory-item-missing-or-ambiguous";
                bool ok = drop_locate(&scratch, request.mgr, request.item, &tab, &slot) &&
                    drop_allowed(&scratch, request.item, request.type, request.value, request.name);
                if (ok) {
                    const char *spawn_only = getenv("SAVAGEWIND_QUICK_DROP_SPAWN_ONLY");
                    if (spawn_only && !strcmp(spawn_only, "1")) {
                        // Isolate world creation from inventory removal after
                        // the failed first test. Use the exact native placement
                        // helper, source SafeString and FP arguments from trashItem.
                        uint64_t source = scratch.sp + 0x10;
                        ST64(source, 0x7001bc32c8ull);
                        ST64(source + 8, 0x7001639fc5ull);
                        Cpu create = scratch;
                        create.x[0] = request.item;
                        create.x[1] = source;
                        create.v[0] = 0x3f4ccccdu;  // 0.8f
                        create.v[1] = 0xbf4ccccdu;  // -0.8f
                        uint64_t manager = LD64(0x7001e0cb08ull);
                        int before = drop_range(manager, 0x60) ? (int32_t)LD32(manager + 0x40) : -1;
                        guest_call(&create, 0x7000e890b0ull);
                        LOG("[quick-drop] spawn-only name=%s accepted=%" PRIu64 " queue=%d->%d"
                            " value=%d valid=%u equipped=%u; inventory retained\n", request.name, create.x[0], before,
                            drop_range(manager, 0x60) ? (int32_t)LD32(manager + 0x40) : -1,
                            (int32_t)LD32(request.item + 0x20), LD8(request.item + 0x25), LD8(request.item + 0x24));
                    } else {
                    // Confirmed recursive mutex: native ctor calls nn::os::
                    // InitializeMutex(recursive=true). Discard locks it again
                    // on the same guest thread while the outer lock pins identity.
                    uint64_t manager = LD64(0x7001e0cb08ull);
                    int before = drop_range(manager, 0x60) ? (int32_t)LD32(manager + 0x40) : -1;
                    drop_call(&scratch, 0x7000d9784cull, request.mgr, tab, slot);
                    LOG("[quick-drop] native discard requested name=%s tab=%d slot=%d valid=%u value=%d queue=%d->%d\n",
                        request.name, tab, slot, LD8(request.item + 0x25), (int32_t)LD32(request.item + 0x20), before,
                        drop_range(manager, 0x60) ? (int32_t)LD32(manager + 0x40) : -1);
                    if (!LD8(request.item + 0x25)) {
                        // Full pause UI follows trashItem with this sweep (calls
                        // at e38254/e3f978). It recycles invalid entries, rebuilds
                        // tab/list heads and saves. The old pointer is no longer
                        // a live PouchItem afterwards: do not dereference it.
                        int selected = (int32_t)LD32(screen + 0x387c);
                        int category = (int32_t)LD32(screen + 0x3738);
                        drop_call(&scratch, 0x7000d97f68ull, request.mgr, 0, 0);
                        // Rebuild the native pointer array and equipped index;
                        // never retain a pointer recycled by the cleanup sweep.
                        drop_call(&scratch, 0x7000e27c2cull, screen, category, 0);
                        int count = (int32_t)LD32(screen + 0x3880);
                        if (selected >= count) selected = count - 1;
                        if (selected < 0) selected = -1;
                        ST32(screen + 0x387c, selected);
                        drop_call(&scratch, 0x7000e28070ull, screen, 0, 0);
                        int cursor = 0;
                        for (int i = 0; i < 21; i++)
                            if ((int32_t)LD32(screen + 0x3884 + i * 4) == selected) { cursor = i; break; }
                        // The native mapping also includes an unequip entry for
                        // ordinary swords, even when an item is highlighted.
                        // Derive the widget size from that rebuilt mapping, as
                        // category alone misses this slot and hides the last item.
                        int extra = (int32_t)LD32(screen + 0x3884) == -1 ||
                            selected == -1;
                        drop_call(&scratch, 0x7000d665a4ull, LD64(screen + 0x37a8), cursor, count + extra);
                        drop_call(&scratch, 0x7000e278c0ull, screen, 0, 0);
                        int remaining_tab = -1, remaining_slot = -1;
                        bool still_listed = drop_locate(&scratch, request.mgr, request.item,
                                                       &remaining_tab, &remaining_slot);
                        LOG("[quick-drop] cleanup name=%s still_listed=%d\n", request.name, still_listed);
                    }
                    }
                } else LOG("[quick-drop] canceled %s reason=%s\n", request.name, drop_reason);
                drop_call(&scratch, DROP_UNLOCK, request.mgr + 0x28, 0, 0);
            }
        }
    }
}

bool quick_drop_action_init(void) {
    if (strcmp(g_game_version, "1.9.0")) return false;
    if (drop_ready) return true;
    const uint64_t helpers[] = {0x7000eaff08ull, 0x7000eaff18ull, 0x7000d970a8ull, 0x7000d992acull,
        0x7000d388b0ull, 0x7000d35b80ull, 0x7000d1499cull, 0x7000e7e750ull, 0x7000cc6fd0ull,
        0x7000cc0334ull, 0x700108dc0cull, 0x700108e7a0ull, 0x7000d9784cull, 0x7000e890b0ull,
        0x7000d97f68ull, 0x7000e27c2cull, 0x7000e28070ull, 0x7000d665a4ull, 0x7000e278c0ull};
    for (size_t i = 0; i < sizeof helpers / sizeof *helpers; i++)
        if (!dispatch_lookup(helpers[i])) {
            LOG("[quick-drop] missing native helper; action disabled\n");
            return false;
        }
    drop_ready = true;
    LOG("[quick-drop] experimental unequipped drop enabled for 1.9.0\n");
    return true;
}
