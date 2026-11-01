// Quick-menu observation hooks for the verified BOTW 1.9.0 build.
// SAVAGEWIND_QUICK_DROP_PROBE=1 logs only; SAVAGEWIND_QUICK_DROP=1 enables the
// separate experimental action module after the same build checks.
#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "game_sites.h"
#include "hle.h"

extern uint64_t input_buttons_now(void);
extern void input_consume_buttons(uint64_t);
extern bool quick_drop_action_enabled(void);
extern bool quick_drop_action_init(void);
extern void quick_drop_action_request(Cpu *, uint64_t, uint64_t, uint64_t, int, int, const char *);
extern void quick_drop_action_close(Cpu *);

enum { QUICK_DROP_BUTTON = 1 << 3, QUICK_BACK_BUTTON = 1 << 1 };
static uint64_t probe_screen, probe_buttons;
static bool guest_range(uint64_t addr, uint64_t size);
static uint64_t probe_call(Cpu *, uint64_t, uint64_t, uint64_t, uint64_t);
// Packed heartbeat: no guest memory is read by the render thread.
static _Atomic uint64_t drop_hint;
static uint64_t hint_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// 0 hidden, 1 unavailable (dim), 2 available, 3 back only. Expiry also handles reset/load
// transitions that bypass the normal Menu callback.
int quick_drop_hint_state(void) {
    uint64_t hint = atomic_load(&drop_hint);
    if (!hint || hint_now_ms() - (hint >> 2) > 150) return 0;
    return hint & 3;
}

static void update_drop_hint(Cpu *c) {
    uint64_t screen = c->x[0];
    atomic_store(&drop_hint, 0);
    if (!quick_drop_action_enabled() || !guest_range(screen, 0x38e5) ||
        LD8(screen + 0x38e4) == 2) return;
    int category = (int32_t)LD32(screen + 0x3738);
    if (category < 0 || category > 4) return;
    if (category > 2) {
        atomic_store(&drop_hint, (hint_now_ms() << 2) | 3);
        return;
    }
    int selected = (int32_t)LD32(screen + 0x387c);
    int count = (int32_t)LD32(screen + 0x3880);
    uint64_t mgr = LD64(screen + 0x3640);
    if (!guest_range(mgr, 0x45000)) return;
    int state = 1;
    probe_call(c, 0x7000eaff08ull, mgr + 0x28, 0, 0);
    if (count > 0 && count <= 20 && selected >= 0 && selected < count) {
        uint64_t item = LD64(screen + 0x37c8 + (uint64_t)selected * 8);
        if (guest_range(item, 0x38) && LD8(item + 0x25) && !LD8(item + 0x24) &&
            (int32_t)LD32(item + 0x20) > 0) {
            uint64_t name = LD64(item + 0x30);
            if (guest_range(name, 97) &&
                strncmp((const char *)(uintptr_t)name, "Weapon_Sword_070", 97) &&
                strncmp((const char *)(uintptr_t)name, "Weapon_Sword_502", 97)) state = 2;
        }
    }
    probe_call(c, 0x7000eaff18ull, mgr + 0x28, 0, 0);
    atomic_store(&drop_hint, (hint_now_ms() << 2) | state);
}
static GuestFn probe_enter_original, probe_execute_original;

RT_ON_RESET(quick_drop_probe_reset) {
    probe_screen = probe_buttons = 0;
    atomic_store(&drop_hint, 0);
    // dispatch.c retains overrides across an in-place reboot, as it does for
    // the JPEG diagnostic. Keep their original callbacks and do not register twice.
}

static bool probe_enabled(void) {
    const char *value = ENV("SAVAGEWIND_QUICK_DROP_PROBE");
    return ((value && !strcmp(value, "1")) || quick_drop_action_enabled()) && !strcmp(g_game_version, "1.9.0");
}

// Called at Menu::enter, before the game's list initialization. Sampling here
// prevents a Y already held when the menu opens from becoming a new press.
void rt_quick_drop_probe_begin(Cpu *c) {
    if (!probe_enabled()) return;
    probe_screen = c->x[0];
    probe_buttons = input_buttons_now();
    LOG("[quick-drop-probe] Menu enter; Y held=%d (%s)\n", !!(probe_buttons & QUICK_DROP_BUTTON),
        quick_drop_action_enabled() ? "experimental drop" : "logging only");
}

static bool guest_range(uint64_t addr, uint64_t size) {
    return addr >= GUEST_LO && addr < GUEST_HI && size <= GUEST_HI - addr;
}

static uint64_t probe_call(Cpu *original, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t x2) {
    // Keep the interrupted function's entire guest register state unchanged.
    // Guest callees use their normal stack below the caller's current SP.
    Cpu call = *original;
    call.x[0] = x0;
    call.x[1] = x1;
    call.x[2] = x2;
    guest_call(&call, fn);
    return call.x[0];
}

void rt_quick_drop_probe(Cpu *c) {
    if (!probe_enabled()) return;
    uint64_t screen = c->x[0], buttons = input_buttons_now();
    if (screen != probe_screen) {
        // An old game library may lack the enter hook: do not fabricate an edge.
        probe_screen = screen;
        probe_buttons = buttons;
        return;
    }
    bool back = (buttons & QUICK_BACK_BUTTON) && !(probe_buttons & QUICK_BACK_BUTTON);
    bool pressed = (buttons & QUICK_DROP_BUTTON) && !(probe_buttons & QUICK_DROP_BUTTON);
    probe_buttons = buttons;
    if (!guest_range(screen, 0x38e5)) return;
    // Menu::execute commits only when highlighted != applied. Restore the
    // applied index before requesting its native close transition (1.9.0).
    // Back wins over a simultaneous drop, including on an empty selector.
    if (back && quick_drop_action_enabled() &&
        (int32_t)LD32(screen + 0x3738) >= 0 &&
        (int32_t)LD32(screen + 0x3738) <= 4 && LD8(screen + 0x38e4) != 2) {
        // Let the game observe the opening direction as released, so its normal
        // close path also releases the pause/render state. Consume Back as well
        // to avoid turning the same press into a sprint after closing.
        input_consume_buttons(buttons & ((7ull << 12) | QUICK_BACK_BUTTON));
        ST32(screen + 0x387c, LD32(screen + 0x3878));
        ST8(screen + 0x38e4, 2);
        atomic_store(&drop_hint, 0);
        return;
    }
    if (!pressed) return;
    int category = (int32_t)LD32(screen + 0x3738);
    int selected = (int32_t)LD32(screen + 0x387c);
    int count = (int32_t)LD32(screen + 0x3880);
    // Menu::execute sets/checks this transition flag when closing.
    if (LD8(screen + 0x38e4) == 2 || category < 0 || category > 2 ||
        count < 1 || count > 20 || selected < 0 || selected >= count) {
        LOG("[quick-drop-probe] ignored category=%d selected=%d count=%d (no inventory change)\n",
            category, selected, count);
        return;
    }
    uint64_t mgr = LD64(screen + 0x3640);
    if (!guest_range(mgr, 0x45000)) return;

    // Native inventory lock. Getters below do not lock; item/page reads and
    // pointer matching must be in the same critical section.
    probe_call(c, 0x7000eaff08ull, mgr + 0x28, 0, 0);
    uint64_t item = LD64(screen + 0x37c8 + (uint64_t)selected * 8);
    int tab_found = -1, slot_found = -1, matches = 0;
    int type = -1, value = 0, equipped = 0, valid = 0;
    char name[97] = {0};
    if (guest_range(item, 0x38)) {
        // Every inventory page has at most 20 entries (gear is capped at 20,
        // and other categories are paginated). Reject an unexpected count.
        int tabs = (int32_t)LD32(mgr + 0x44498);
        if (tabs < 0 || tabs > 50) tabs = 0;
        for (int tab = 0; tab < tabs; tab++) {
            int n = (int32_t)probe_call(c, 0x7000d970a8ull, mgr, tab, 0);
            if (n < 0 || n > 20) continue;
            for (int slot = 0; slot < n; slot++) {
                uint64_t candidate = probe_call(c, 0x7000d992acull, mgr, tab, slot);
                if (candidate == item) {
                    tab_found = tab;
                    slot_found = slot;
                    matches++;
                }
            }
        }
        // Only dereference an item confirmed in the live inventory, not a stale
        // quick-menu pointer that might have been recycled in another list.
        if (matches == 1) {
            type = (int32_t)LD32(item + 0x18);
            value = (int32_t)LD32(item + 0x20);
            equipped = LD8(item + 0x24);
            valid = LD8(item + 0x25);
            uint64_t str = LD64(item + 0x30);  // SafeString at item+0x28
            if (guest_range(str, sizeof name)) {
                for (size_t i = 0; i + 1 < sizeof name; i++) {
                    unsigned ch = LD8(str + i);
                    if (!ch) break;
                    name[i] = ch >= 32 && ch < 127 ? (char)ch : '?';
                }
            }
        }
    }
    probe_call(c, 0x7000eaff18ull, mgr + 0x28, 0, 0);
    // These two recognized protections are diagnostic hints, not a complete
    // eligibility decision. Native popup restrictions are still being mapped.
    bool protected_name = !strcmp(name, "Weapon_Sword_070") || !strcmp(name, "Weapon_Sword_502");
    LOG("[quick-drop-probe] category=%d selected=%d/%d item=%" PRIx64
        " name=%s type=%d value=%d equipped=%d valid=%d tab=%d slot=%d matches=%d"
        " protected_name=%d eligibility=unverified action=none\n",
        category, selected, count, item, name, type, value, equipped, valid,
        tab_found, slot_found, matches, protected_name);
    if (quick_drop_action_enabled() && matches == 1)
        quick_drop_action_request(c, screen, mgr, item, tab_found, slot_found, name);
}

static void probe_enter(Cpu *c) {
    rt_quick_drop_probe_begin(c);
    probe_enter_original(c);
}

static void probe_execute(Cpu *c) {
    rt_quick_drop_probe(c);
    quick_drop_action_close(c);
    update_drop_hint(c);
    probe_execute_original(c);
}

// Runtime overrides let this diagnostic use an existing game library. The
// original callbacks still run exactly once, with the original register state.
// Install after dispatch_init and before starting guest threads.
void quick_drop_probe_init(const char *exefs) {
    if (strcmp(g_game_version, "1.9.0") || probe_enter_original) return;
    static const unsigned char build[8] = {0xcd, 0x57, 0xb2, 0x3f, 0xa4, 0xbb, 0xad, 0x65};
    unsigned char header[0x48];
    char path[1100];
    int n = snprintf(path, sizeof path, "%s/main", exefs);
    if (n < 0 || n >= (int)sizeof path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t got = fread(header, 1, sizeof header, f);
    fclose(f);
    if (got != sizeof header || memcmp(header, "NSO0", 4) || memcmp(header + 0x40, build, sizeof build)) {
        LOG("[quick-drop-probe] unsupported main build; not installed\n");
        return;
    }
    const uint64_t helpers[] = {0x7000eaff08ull, 0x7000eaff18ull, 0x7000d970a8ull, 0x7000d992acull};
    for (size_t i = 0; i < sizeof helpers / sizeof *helpers; i++) {
        if (!dispatch_lookup(helpers[i])) {
            LOG("[quick-drop-probe] missing lookup helper; not installed\n");
            return;
        }
    }
    probe_enter_original = dispatch_lookup(0x7000e283fcull);
    probe_execute_original = dispatch_lookup(0x7000e28744ull);
    if (!probe_enter_original || !probe_execute_original) {
        probe_enter_original = probe_execute_original = NULL;
        LOG("[quick-drop-probe] missing Menu callbacks; not installed\n");
        return;
    }
    if (!quick_drop_action_init()) {
        probe_enter_original = probe_execute_original = NULL;
        LOG("[quick-drop-probe] action helpers unavailable; not installed\n");
        return;
    }
    hle_override(0x7000e283fcull, probe_enter, "quick-drop probe: Menu enter");
    hle_override(0x7000e28744ull, probe_execute, "quick-drop probe: Menu execute");
    LOG("[quick-drop-probe] installed for 1.9.0 cd57b23fa4bbad65; %s\n",
        quick_drop_action_enabled() ? "Y requests experimental unequipped-item drop" : "Y logs only, inventory unchanged");
}
