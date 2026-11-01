// Diagnostic invariants: input edges, native locked lookup, unchanged guest CPU/inventory,
// and strict build gating. The mock rejects any unexpected guest function call.
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../runtime/quick_drop.c"

const char *g_game_version = "1.9.0";
static bool action_enabled;
bool quick_drop_action_enabled(void) { return action_enabled; }
bool quick_drop_action_init(void) { return true; }
void quick_drop_action_request(Cpu *c, uint64_t screen, uint64_t mgr, uint64_t item,
                               int tab, int slot, const char *name) {
    (void)c; (void)screen; (void)mgr; (void)item; (void)tab; (void)slot; (void)name;
    assert(0);
}
void quick_drop_action_close(Cpu *c) { (void)c; }
static uint64_t buttons;
static int locks, unlocks, depth, reads, originals, overrides;
static uint64_t live_item;
static int duplicate;
static GuestFn installed_enter, installed_execute;
static uint64_t consumed;
void input_consume_buttons(uint64_t mask) { consumed = mask; }
uint64_t input_buttons_now(void) { return buttons; }

void guest_call(Cpu *c, uint64_t addr) {
    uint64_t mgr = 0x7240000000ull;
    if (addr == 0x7000eaff08ull) {
        assert(c->x[0] == mgr + 0x28 && depth == 0);
        locks++; depth++;
    } else if (addr == 0x7000eaff18ull) {
        assert(c->x[0] == mgr + 0x28 && depth == 1);
        unlocks++; depth--;
    } else if (addr == 0x7000d970a8ull) {
        assert(c->x[0] == mgr && depth == 1 && c->x[1] < 3);
        c->x[0] = c->x[1] == 1 || (duplicate && c->x[1] == 2) ? 2 : 0;
    } else if (addr == 0x7000d992acull) {
        assert(c->x[0] == mgr && depth == 1 && c->x[2] < 2);
        reads++;
        c->x[0] = c->x[2] == 1 ? live_item : 0;
    } else if (addr == 0x7000e25dc0ull) {
        ST32(c->x[0] + 0x38e0, -1);
        ST8(c->x[0] + 0x38e4, 1);
    } else {
        fprintf(stderr, "unexpected guest call: %" PRIx64 "\n", addr);
        abort();
    }
    // Native functions may clobber caller-saved registers. The probe must isolate them.
    c->x[3] = 0xdead;
    c->pc = 0xbeef;
    c->nzcv = 0x10000000;
    c->v[0] = 0x1234;
}

static void original(Cpu *c) { (void)c; originals++; }
GuestFn dispatch_lookup(uint64_t addr) {
    assert(addr == 0x7000e283fcull || addr == 0x7000e28744ull || addr == 0x7000eaff08ull ||
           addr == 0x7000eaff18ull || addr == 0x7000d970a8ull || addr == 0x7000d992acull || addr == 0x7000e25dc0ull || addr == 0x7000e28838ull);
    return original;
}
void hle_override(uint64_t addr, HostFn fn, const char *name) {
    (void)name;
    overrides++;
    if (addr == 0x7000e283fcull) installed_enter = fn;
    else if (addr == 0x7000e28744ull) installed_execute = fn;
    else abort();
}

static void press(Cpu *c) {
    buttons = 0;
    rt_quick_drop_probe(c);
    buttons = QUICK_DROP_BUTTON;
    rt_quick_drop_probe(c);
}

int main(int argc, char **argv) {
    bool enabled = argc == 1 || strcmp(argv[1], "disabled");
    if (enabled) setenv("SAVAGEWIND_QUICK_DROP_PROBE", "1", 1);
    else unsetenv("SAVAGEWIND_QUICK_DROP_PROBE");
    uint64_t mgr = 0x7240000000ull, screen = mgr + 0x50000, item = mgr + 0x48000;
    void *mem = mmap((void *)(uintptr_t)mgr, 0x60000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mem == (void *)(uintptr_t)mgr);
    ST64(screen + 0x3640, mgr);
    ST32(screen + 0x3738, 0);
    ST32(screen + 0x387c, 1);
    ST32(screen + 0x3880, 2);
    ST32(mgr + 0x44498, 3);
    ST64(screen + 0x37c8 + 8, item);
    ST32(item + 0x18, 0);
    ST32(item + 0x20, 123);
    ST8(item + 0x24, 1);
    ST8(item + 0x25, 1);
    ST64(item + 0x30, mgr + 0x49000);
    strcpy((char *)(uintptr_t)(mgr + 0x49000), "Weapon_Sword_070");
    live_item = item;
    Cpu c = {.sp = mgr + 0x47000, .nzcv = 0xa0000000};
    c.x[0] = screen;
    c.x[3] = 17;
    c.v[0] = 19;
    Cpu saved = c;
    unsigned char *snapshot = malloc(0x60000);
    memcpy(snapshot, mem, 0x60000);

    // Opening while Y is held must not synthesize a press.
    buttons = QUICK_DROP_BUTTON;
    rt_quick_drop_probe_begin(&c);
    rt_quick_drop_probe(&c);
    assert(locks == 0);
    buttons = 1 << 2;  // former X trigger is no longer the drop action
    rt_quick_drop_probe(&c);
    assert(locks == 0);
    press(&c);
    if (!enabled) {
        assert(locks == 0 && !memcmp(&c, &saved, sizeof c));
        quick_drop_probe_init("/nonexistent");
        assert(overrides == 0);
        puts("ok: probe disabled by default");
        return 0;
    }
    assert(locks == 1 && unlocks == 1 && reads == 2 && depth == 0);
    assert(!memcmp(&c, &saved, sizeof c));
    assert(!memcmp(snapshot, mem, 0x60000));
    rt_quick_drop_probe(&c);  // held Y: no repeat
    assert(locks == 1);
    press(&c);
    assert(locks == 2);

    // Duplicate and stale pointers are reported without dereferencing stale memory.
    duplicate = 1;
    press(&c);
    duplicate = 0;
    live_item = 0;
    press(&c);
    live_item = item;
    assert(!memcmp(snapshot, mem, 0x60000));
    assert(!memcmp(&c, &saved, sizeof c));
    int before = locks;
    ST32(screen + 0x3738, 3); press(&c);  // arrows
    ST32(screen + 0x3738, 4); press(&c);  // runes
    ST32(screen + 0x3738, 0);
    ST32(screen + 0x387c, 2); press(&c);  // out of bounds
    ST32(screen + 0x387c, 1);
    ST32(screen + 0x3880, 0); press(&c);  // empty list
    ST32(screen + 0x3880, 2);
    ST8(screen + 0x38e4, 2); press(&c);   // closing
    ST8(screen + 0x38e4, 0);
    g_game_version = "1.8.2"; press(&c);
    assert(locks == before);
    g_game_version = "1.9.0";

    // A real header enables exactly two wrappers; mismatching headers enable none.
    char dir[] = "/tmp/quick-drop-test-XXXXXX";
    assert(mkdtemp(dir));
    char path[128]; snprintf(path, sizeof path, "%s/main", dir);
    unsigned char header[0x48] = {'N', 'S', 'O', '0'};
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(header, 1, sizeof header, f) == sizeof header); fclose(f);
    quick_drop_probe_init(dir);
    assert(overrides == 0);
    const unsigned char build[] = {0xcd, 0x57, 0xb2, 0x3f, 0xa4, 0xbb, 0xad, 0x65};
    memcpy(header + 0x40, build, sizeof build);
    f = fopen(path, "wb"); assert(f);
    assert(fwrite(header, 1, sizeof header, f) == sizeof header); fclose(f);
    quick_drop_probe_init(dir);
    assert(overrides == 2 && installed_enter && installed_execute);
    quick_drop_probe_init(dir);
    assert(overrides == 2);
    installed_enter(&c);
    installed_execute(&c);
    assert(originals == 2 && !memcmp(&c, &saved, sizeof c));
    quick_drop_probe_reset();
    assert(!probe_screen && !probe_buttons && probe_enter_original && probe_execute_original);
    quick_drop_probe_init(dir);
    assert(overrides == 2);  // dispatcher retains its wrappers across a reboot
    unlink(path); rmdir(dir);
    free(snapshot);
    // UI snapshot is published on the guest thread and never mutates Cpu.
    action_enabled = true;
    update_drop_hint(&c); assert(quick_drop_hint_state() == 1); // protected/equipped
    ST8(item + 0x24, 0);
    strcpy((char *)(uintptr_t)(mgr + 0x49000), "Weapon_Sword_001");
    update_drop_hint(&c); assert(quick_drop_hint_state() == 2);
    assert(!memcmp(&c, &saved, sizeof c));
    ST32(screen + 0x3738, 4);
    update_drop_hint(&c); assert(quick_drop_hint_state() == 3);
    ST32(screen + 0x3738, 0); ST8(screen + 0x38e4, 2);
    update_drop_hint(&c); assert(quick_drop_hint_state() == 0);
    atomic_store(&drop_hint, ((hint_now_ms() - 200) << 2) | 2);
    assert(quick_drop_hint_state() == 0);
    quick_drop_probe_reset(); assert(quick_drop_hint_state() == 0);
    // Back cancels the highlighted choice, wins over drop, works in empty
    // categories, and does not repeat when held or already held on enter.
    ST8(screen + 0x38e4, 0);
    ST32(screen + 0x3878, 0);
    ST32(screen + 0x387c, 1);
    buttons = 0; rt_quick_drop_probe_begin(&c);
    buttons = QUICK_BACK_BUTTON | QUICK_DROP_BUTTON | (1ull << 14);
    rt_quick_drop_probe(&c);
    assert(LD8(screen + 0x38e4) == 2 && LD32(screen + 0x387c) == 0);
    assert(!memcmp(&c, &saved, sizeof c));
    assert(consumed == (QUICK_BACK_BUTTON | (1ull << 14)));
    ST8(screen + 0x38e4, 0); ST32(screen + 0x387c, 1);
    rt_quick_drop_probe(&c);
    assert(LD8(screen + 0x38e4) == 0 && LD32(screen + 0x387c) == 1);
    rt_quick_drop_probe_begin(&c); rt_quick_drop_probe(&c);
    assert(LD8(screen + 0x38e4) == 0);
    ST32(screen + 0x3738, 4); ST32(screen + 0x3880, 0);
    buttons = 0; rt_quick_drop_probe(&c);
    buttons = QUICK_BACK_BUTTON; rt_quick_drop_probe(&c);
    assert(LD8(screen + 0x38e4) == 2 && LD32(screen + 0x387c) == 0);
    assert(consumed == QUICK_BACK_BUTTON);
    munmap(mem, 0x60000);
    puts("ok: input edges, locked exact lookup, no inventory/CPU mutations, build gating, wrappers");
    return 0;
}
