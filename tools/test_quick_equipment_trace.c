// Native callbacks execute exactly once with their CPU semantics intact;
// observation and developer/build guards cannot write guest state.
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/quick_equipment_trace.c"

const char *g_game_version = "1.9.0";
struct ControlReply { bool failed; unsigned fields; uint64_t drop_calls, equip_calls, update_calls; };
static bool developer, missing;
static uint64_t frame = 77;
static unsigned native_calls[3], overrides, lookups;
static HostFn wrappers[3];
bool control_enabled(void) { return developer; }
uint64_t renderer_present_count(void) { return frame; }
void control_fail(ControlReply *r, const char *fmt, ...) { (void)fmt; r->failed = true; }
void control_field_int(ControlReply *r, const char *key, int64_t value) {
    r->fields++;
    if (!strcmp(key, "drop_calls")) r->drop_calls = value;
    if (!strcmp(key, "equip_calls")) r->equip_calls = value;
    if (!strcmp(key, "update_calls")) r->update_calls = value;
}
void control_field_str(ControlReply *r, const char *key, const char *value) {
    (void)key; (void)value; r->fields++;
}
static void original_drop(Cpu *c) {
    native_calls[0]++;
    assert(c->x[1] == 0x1234 && c->pc == 0x5678);
    c->x[0] = 0xdead; c->x[1] = 0xbeef; c->v[0] = 0x1337; c->pc = 0xcafe; c->nzcv = 0xa0000000;
}
static void original_equip(Cpu *c) {
    native_calls[1]++;
    assert(c->x[1] == 0x1234 && c->pc == 0x5678);
    c->x[0] = 0xfeed; c->sp += 0x10; c->v[31] = 0x9876; c->pc = 0xabcd;
}
static void original_update(Cpu *c) {
    native_calls[2]++;
    assert(c->x[1] == 0x1234 && c->pc == 0x5678);
    c->x[30] = 0x112233; c->v[17] = 0x887766; c->pc = 0xabc123;
}
GuestFn dispatch_lookup(uint64_t address) {
    lookups++;
    if (address == 0x7000cdba80ull) return original_drop;
    if (address == 0x7000cdc43cull) return original_equip;
    assert(address == 0x7000ccfb08ull);
    return missing ? NULL : original_update;
}
void hle_override(uint64_t address, HostFn fn, const char *name) {
    assert(name && fn); overrides++;
    wrappers[address == 0x7000cdba80ull ? 0 : address == 0x7000cdc43cull ? 1 : 2] = fn;
}
static Cpu cpu(uint64_t player) {
    Cpu c = {0}; c.x[0] = player; c.x[1] = 0x1234; c.pc = 0x5678; return c;
}
static ControlReply command(char *operation) {
    char *args[] = {"equipment-trace", operation}; ControlReply r = {0};
    quick_equipment_trace_command(2, args, &r); return r;
}
int main(void) {
    assert(command("status").failed);
    quick_equipment_trace_enable(); assert(!overrides && !lookups);
    developer = true; g_game_version = "1.8.2";
    quick_equipment_trace_enable(); assert(!overrides && !lookups);
    assert(command("status").failed);
    g_game_version = "1.9.0";
    assert(command("status").failed); // no validated enable call yet
    missing = true; quick_equipment_trace_enable(); assert(!overrides && !atomic_load(&installed));
    missing = false; quick_equipment_trace_enable(); assert(overrides == 3);
    quick_equipment_trace_enable(); assert(overrides == 3);
    uint64_t player = 0x7240000000ull;
    void *memory = mmap((void *)(uintptr_t)player, 0x2000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory == (void *)(uintptr_t)player);
    ST32(player + 0xc98, 0x12340030);
    ST8(player + 0x13e0, 1); ST8(player + 0x1470, 1); ST8(player + 0x1121, 1);
    unsigned char snapshot[0x2000]; memcpy(snapshot, memory, sizeof snapshot);
    Cpu a = cpu(player), expected = a;
    original_drop(&expected); native_calls[0] = 0; wrappers[0](&a);
    assert(!memcmp(&a, &expected, sizeof a) && native_calls[0] == 1);
    assert(atomic_load(&observations[0].before) == (64 | 1 | 4 | 16));
    assert(atomic_load(&observations[0].after) == (64 | 1 | 4 | 16));
    assert(atomic_load(&observations[0].frame) == frame);
    a = cpu(player); expected = a; original_equip(&expected); native_calls[1] = 0; wrappers[1](&a);
    assert(!memcmp(&a, &expected, sizeof a) && native_calls[1] == 1);
    a = cpu(player); expected = a; original_update(&expected); native_calls[2] = 0; wrappers[2](&a);
    assert(!memcmp(&a, &expected, sizeof a) && native_calls[2] == 1);
    assert(atomic_load(&observations[2].schedule_before) == 0x12340030);
    assert(atomic_load(&observations[2].schedule_after) == 0x12340030);
    assert(atomic_load(&observations[2].frame) == frame);
    assert(!memcmp(snapshot, memory, sizeof snapshot));
    ControlReply r = command("status"); assert(!r.failed && r.drop_calls == 1 && r.equip_calls == 1 && r.update_calls == 1);
    developer = false; a = cpu(0); wrappers[0](&a);
    assert(native_calls[0] == 2 && atomic_load(&observations[0].calls) == 1);
    assert(command("reset").failed);
    developer = true; g_game_version = "1.8.2"; a = cpu(0); wrappers[1](&a);
    assert(native_calls[1] == 2 && atomic_load(&observations[1].calls) == 1);
    g_game_version = "1.9.0";
    a = cpu(GUEST_HI - 1); wrappers[0](&a);
    assert(atomic_load(&observations[0].after) == 0); // invalid pointer is never read
    assert(command("invalid").failed);
    r = command("reset"); assert(!r.failed && !r.drop_calls && !r.equip_calls && !r.update_calls);
    a = cpu(player); wrappers[0](&a); quick_equipment_trace_reset();
    r = command("status"); assert(!r.failed && !r.drop_calls && !r.equip_calls && !r.update_calls);
    quick_equipment_trace_enable(); assert(overrides == 3); // reboot keeps originals
    a = cpu(player); wrappers[1](&a); assert(native_calls[1] == 3);
    assert(atomic_load(&observations[1].calls) == 1);
    munmap(memory, 0x2000);
    puts("quick equipment trace tests passed");
    return 0;
}
