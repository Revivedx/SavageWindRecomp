// Native-copy fixture contract: presenter queues only; native calls use a
// guest CPU snapshot, locked inventory identity, and never remove the source.
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/quick_inventory_test.c"

const char *g_game_version = "1.9.0";
GameSites g_site;
struct ControlReply { bool failed; };
static bool developer = true, missing_helper, event_active, unsuitable, fail_spawn;
static bool creation_success, creation_failure;
static uint64_t now = 100, native_player, expected_item;
static unsigned calls, creates, locks, unlocks, depth;
static const uint64_t mgr = 0x7240000000ull, objects = 0x7240046000ull;
static const uint64_t spawn_mgr = 0x7240056000ull;
static const uint64_t gear_block = 0x7240058000ull, created_actor = 0x724005a000ull;
static unsigned allocations, request_releases, actor_refs, actor_releases, finalized, placed;
void *hle_alloc(uint64_t size) {
    assert(size == GEAR_STORAGE_SIZE); allocations++;
    memset((void *)(uintptr_t)gear_block, 0, size);
    return (void *)(uintptr_t)gear_block;
}
static uint64_t entries[4];
static unsigned chest_begins, chest_polls;
static int chest_result;
void quick_inventory_chest_test_enable(void) {}
void quick_inventory_chest_test_reset(void) {}
bool quick_inventory_chest_test_begin(Cpu *cpu, const char *reward, const char **reason) {
    (void)cpu; assert(!strcmp(reward, "Weapon_Bow_001"));
    chest_begins++; *reason = "pending-native-creation"; return true;
}
int quick_inventory_chest_test_poll(Cpu *cpu, uint64_t *actor, const char **reason) {
    (void)cpu; chest_polls++; *actor = chest_result > 0 ? 0x7240058800ull : 0;
    *reason = chest_result > 0 ? "native-chest-created" : "native-chest-failed";
    return chest_result;
}
uint64_t plat_now_ns(void) { return now; }
bool control_enabled(void) { return developer; }
void control_fail(ControlReply *r, const char *fmt, ...) { (void)fmt; r->failed = true; }
void control_field_bool(ControlReply *r, const char *k, bool v) { (void)r; (void)k; (void)v; }
void control_field_int(ControlReply *r, const char *k, int64_t v) { (void)r; (void)k; (void)v; }
void control_field_str(ControlReply *r, const char *k, const char *v) { (void)r; (void)k; assert(v); }
void control_field_num(ControlReply *r, const char *k, double v) { (void)r; (void)k; (void)v; }
static void dummy(Cpu *c) { (void)c; }
GuestFn dispatch_lookup(uint64_t address) { (void)address; return missing_helper ? NULL : dummy; }

void guest_call(Cpu *c, uint64_t address) {
    calls++;
    switch (address) {
    case 0x7000d1499cull: c->x[0] = event_active; break;
    case 0x7000cc6fd0ull: c->x[0] = native_player; break;
    case 0x7000cc0334ull: assert(c->x[0] == native_player); c->x[0] = unsuitable; break;
    case 0x7000eaff08ull:
        assert(c->x[0] == mgr + 0x28 && !depth); depth++; locks++; break;
    case 0x7000eaff18ull:
        assert(c->x[0] == mgr + 0x28 && depth == 1); depth--; unlocks++; break;
    case 0x7000d970a8ull:
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 2); c->x[0] = 2; break;
    case 0x7000d992acull:
        assert(depth == 1 && c->x[0] == mgr && c->x[1] < 2 && c->x[2] < 2);
        c->x[0] = entries[c->x[1] * 2 + c->x[2]]; break;
    case 0x7001516814ull:
        assert(c->x[0] == gear_block); ST64(c->x[0], 0); ST8(c->x[0] + 8, 0); break;
    case 0x70007a6488ull:
        assert(depth == 1 && c->x[1] == expected_item && c->x[0] == gear_block + GEAR_MODIFIERS);
        ST32(c->x[0], LD32(expected_item + 0x88)); ST32(c->x[0] + 4, LD32(expected_item + 0x80)); break;
    case 0x7000be3110ull:
        assert(depth == 1 && !strcmp((char *)(uintptr_t)c->x[0], (char *)(uintptr_t)LD64(expected_item + 0x30)));
        assert((uint32_t)c->v[0] == 0x3f800000 && LD32(c->sp) == 2 && !(c->sp & 15));
        assert(c->x[2] == mgr + 0x59000 && c->x[3] == gear_block && c->x[4] == LD32(expected_item + 0x20));
        assert(c->x[5] == 0 && c->x[6] == gear_block + GEAR_MODIFIERS && c->x[7] == 2);
        assert(LD32(c->x[6]) == LD32(expected_item + 0x88) && LD32(c->x[6] + 4) == LD32(expected_item + 0x80));
        memcpy((void *)(uintptr_t)(created_actor + 0x398), (void *)(uintptr_t)c->x[1], 0x30);
        creates++;
        ST64(gear_block, fail_spawn ? 0 : mgr + 0x5b000);
        c->x[0] = !fail_spawn;
        break;
    case 0x7001516820ull: assert(c->x[0] == gear_block); c->x[0] = creation_success; break;
    case 0x7001516840ull: assert(c->x[0] == gear_block); c->x[0] = creation_failure; break;
    case 0x7001516b74ull:
        assert(c->x[0] == gear_block && creation_success); finalized++;
        ST64(gear_block, 0); c->x[0] = created_actor; break;
    case 0x7001516908ull:
        assert(c->x[0] == gear_block); if (LD64(gear_block)) request_releases++;
        ST64(gear_block, 0); ST8(gear_block + 8, 0); break;
    case 0x700112649cull:
        assert(c->x[0] == gear_block + GEAR_ACTOR && c->x[1] == created_actor);
        ST64(c->x[0] + 8, created_actor); actor_refs++; c->x[0] = 1; break;
    case 0x7001126464ull:
        assert(c->x[0] == gear_block + GEAR_ACTOR);
        if (LD64(c->x[0] + 8)) actor_releases++; ST64(c->x[0] + 8, 0); break;
    case 0x700118effcull:
        assert(c->x[0] == spawn_mgr + 0xf0 && c->x[1] == created_actor && c->x[2] == 1); break;
    case 0x7000be24a0ull:
        assert(c->x[0] == gear_block + GEAR_MATRIX && c->x[1] == created_actor); break;
    case 0x700152261cull:
        assert(c->x[0] == created_actor && c->x[1] == gear_block + GEAR_MATRIX && c->x[2] == 0);
        placed++; memcpy((void *)(uintptr_t)(created_actor + 0x398), (void *)(uintptr_t)c->x[1], 0x30); break;
    default: fprintf(stderr, "Unexpected guest call: %" PRIx64 "\n", address); abort();
    }
    // Every helper may clobber the CPU; caller state must remain intact.
    c->x[3] = 0xdead; c->pc = 0xbeef; c->nzcv = 0x10000000;
    c->v[0] = 0x1234; c->sp -= 32;
}

static ControlReply command(const char *kind, const char *index) {
    char *argv[] = {"inventory-test", "spawn", (char *)kind, (char *)index};
    ControlReply r = {0};
    quick_inventory_test_command(4, argv, &r);
    return r;
}
static void status(void) {
    char *argv[] = {"inventory-test", "status"};
    ControlReply r = {0}; quick_inventory_test_command(2, argv, &r); assert(!r.failed);
}
static void chest_command(void) {
    char *argv[] = {"inventory-test", "chest", "bow"};
    ControlReply reply = {0}; quick_inventory_test_command(3, argv, &reply); assert(!reply.failed);
}
static void rejected(Cpu *c, const char *reason) {
    assert(!command("weapon", "1").failed);
    quick_inventory_test_tick(c);
    assert(!strcmp(test_status.state, "rejected") && !strcmp(test_status.reason, reason));
    assert(depth == 0 && locks == unlocks);
}

int main(void) {
    void *memory = mmap((void *)(uintptr_t)mgr, 0x60000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    void *globals = mmap((void *)0x7001d66000ull, 0x101000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory == (void *)(uintptr_t)mgr && globals == (void *)0x7001d66000ull);
    ST64(0x7001d67af0ull, mgr + 0x57000); ST64(mgr + 0x57000, mgr);
    ST64(0x7001e0cb08ull, spawn_mgr);
    ST64(0x7001d66550ull, mgr + 0x57010); ST64(mgr + 0x57010, spawn_mgr);
    ST64(0x7001d66558ull, mgr + 0x57020); ST64(mgr + 0x57020, mgr + 0x57030);
    ST64(mgr + 0x57030 + 0x28, mgr + 0x59000); // opaque context has a zero first byte
    ST64(0x7001d66310ull, mgr + 0x57010);
    g_site.player_info = mgr + 0x50000; ST64(g_site.player_info, mgr + 0x51000);
    native_player = mgr + 0x52000;
    float player_matrix[12] = {1,0,0,100, 0,1,0,200, 0,0,1,300};
    memcpy((void *)(uintptr_t)(native_player + 0x398), player_matrix, sizeof player_matrix);
    ST32(mgr + 0x44498, 2);
    const int types[] = {0, 1, 0, 3};
    const char *names[] = {"Weapon_Sword_001", "Weapon_Bow_001", "Weapon_Sword_002", "Weapon_Shield_001"};
    for (int i = 0; i < 4; i++) {
        uint64_t item = entries[i] = objects + i * 0x100;
        memset((void *)(uintptr_t)item, 0xa5, 0x100); // includes durability/modifier bytes
        ST32(item + 0x18, types[i]); ST32(item + 0x20, 123 + i);
        ST8(item + 0x24, 0); ST8(item + 0x25, 1);
        ST64(item + 0x30, mgr + 0x49000 + i * 0x100);
        strcpy((char *)(uintptr_t)LD64(item + 0x30), names[i]);
    }
    expected_item = entries[2];
    Cpu cpu = {.sp = mgr + 0x5f000, .nzcv = 0xa0000000};
    cpu.x[3] = 17; cpu.v[0] = 19;
    Cpu original = cpu;
    unsigned char source[0x400]; memcpy(source, (void *)(uintptr_t)objects, sizeof source);
    assert(command("weapon", "1").failed && calls == 0); // not validated
    quick_inventory_test_enable();
    developer = false; assert(command("weapon", "1").failed); developer = true;
    g_game_version = "1.8.2"; assert(command("weapon", "1").failed); g_game_version = "1.9.0";
    missing_helper = true; quick_inventory_test_enable(); assert(command("weapon", "1").failed);
    missing_helper = false; quick_inventory_test_enable();
    assert(command("arrow", "0").failed && command("weapon", "20").failed);
    assert(command("weapon", "-1").failed && command("weapon", "x").failed);
    assert(!command("weapon", "1").failed);
    status(); assert(calls == 0 && !strcmp(test_status.state, "queued"));
    assert(command("bow", "0").failed); // one outstanding request
    quick_inventory_test_tick(&cpu);
    assert(creates == 1 && !strcmp(test_status.state, "spawning"));
    assert(!strcmp(test_status.name, names[2]) && test_status.value == 125);
    assert(test_status.target[0] == 100 && test_status.target[1] == 200.8f && test_status.target[2] == 302.5f);
    assert(test_status.modifier_flags == LD32(expected_item + 0x88) && test_status.modifier_value == LD32(expected_item + 0x80));
    assert(command("bow", "0").failed);
    quick_inventory_test_tick(&cpu); assert(!strcmp(test_status.state, "spawning"));
    creation_success = true; quick_inventory_test_tick(&cpu); creation_success = false;
    assert(!strcmp(test_status.state, "created") && test_status.actor == created_actor && placed == 1);
    assert(test_status.position[0] == 100 && test_status.position[1] == 200.8f && test_status.position[2] == 302.5f);
    assert(finalized == 1 && actor_refs == 1 && allocations == 1);
    assert(!memcmp(&cpu, &original, sizeof cpu));
    assert(!memcmp(source, (void *)(uintptr_t)objects, sizeof source));
    quick_inventory_test_tick(&cpu); assert(creates == 1); // no repeat
    event_active = true; rejected(&cpu, "active-event"); event_active = false;
    uint64_t player = native_player; native_player = 0;
    rejected(&cpu, "missing-player"); native_player = player;
    unsuitable = true; rejected(&cpu, "player-state"); unsuitable = false;
    ST8(player + 0xcf8, 4); rejected(&cpu, "player-state"); ST8(player + 0xcf8, 0);
    ST8(expected_item + 0x25, 0); rejected(&cpu, "invalid-item"); ST8(expected_item + 0x25, 1);
    strcpy((char *)(uintptr_t)LD64(expected_item + 0x30), "Weapon_Sword_070");
    rejected(&cpu, "protected-name");
    strcpy((char *)(uintptr_t)LD64(expected_item + 0x30), names[2]);
    assert(!command("shield", "1").failed); quick_inventory_test_tick(&cpu);
    assert(!strcmp(test_status.reason, "category-index-missing"));
    ST64(0x7001d66550ull, 0); rejected(&cpu, "missing-spawn-manager"); ST64(0x7001d66550ull, mgr + 0x57010);
    cpu.sp++; rejected(&cpu, "invalid-guest-stack"); cpu = original;
    cpu.sp = GUEST_LO + 0x80; rejected(&cpu, "invalid-guest-stack"); cpu = original;
    fail_spawn = true; rejected(&cpu, "native-copy-rejected"); fail_spawn = false;
    assert(!command("weapon", "1").failed); quick_inventory_test_tick(&cpu);
    creation_failure = true; quick_inventory_test_tick(&cpu); creation_failure = false;
    assert(!strcmp(test_status.reason, "native-copy-creation-failed") && request_releases == 1);
    assert(!command("weapon", "1").failed); quick_inventory_test_tick(&cpu);
    now += 10000000001ull; quick_inventory_test_tick(&cpu);
    assert(!strcmp(test_status.reason, "native-copy-creation-timeout") && request_releases == 2);
    assert(!command("bow", "0").failed); now += TEST_TIMEOUT_NS + 1;
    status(); assert(!strcmp(test_status.reason, "guest-tick-timeout"));
    unsigned before = creates; quick_inventory_test_tick(&cpu); assert(creates == before);
    assert(!command("bow", "0").failed); quick_inventory_test_reset();
    quick_inventory_test_tick(&cpu); assert(creates == before && !strcmp(test_status.state, "idle"));
    // Each supported category selects the matching item, including equipped
    // sources: copying never calls the native inventory-removal helper.
    for (int i = 1; i <= 3; i += 2) {
        expected_item = entries[i]; ST8(expected_item + 0x24, 1);
        if (i == 3) {
            float rotated[12] = {0,0,1,100, 0,1,0,200, -1,0,0,300};
            memcpy((void *)(uintptr_t)(native_player + 0x398), rotated, sizeof rotated);
        }
        assert(!command(i == 1 ? "bow" : "shield", "0").failed);
        quick_inventory_test_tick(&cpu); assert(!strcmp(test_status.state, "spawning"));
        creation_success = true; quick_inventory_test_tick(&cpu); creation_success = false;
        assert(!strcmp(test_status.state, "created"));
        if (i == 3) assert(test_status.position[0] == 102.5f && test_status.position[2] == 300);
        ST8(expected_item + 0x24, 0);
    }
    chest_command(); assert(chest_begins == 0);
    quick_inventory_test_tick(&cpu); assert(chest_begins == 1 && !strcmp(test_status.state, "spawning"));
    assert(command("weapon", "0").failed);
    quick_inventory_test_tick(&cpu); assert(chest_polls == 1 && !strcmp(test_status.state, "spawning"));
    chest_result = 1; quick_inventory_test_tick(&cpu);
    assert(!strcmp(test_status.state, "created") && test_status.actor == 0x7240058800ull);
    chest_command(); quick_inventory_test_tick(&cpu); chest_result = -1; quick_inventory_test_tick(&cpu);
    assert(!strcmp(test_status.state, "rejected") && !test_status.actor);
    assert(!memcmp(source, (void *)(uintptr_t)objects, sizeof source));
    assert(!memcmp(&cpu, &original, sizeof cpu));
    assert(depth == 0 && locks == unlocks);
    assert(actor_refs == actor_releases + 1 && allocations == 2); // one retained actor, one block per boot
    munmap(memory, 0x60000); munmap(globals, 0x101000);
    puts("ok: native gear creation ABI/lifetime/positions/modifiers, guards, unchanged source/CPU, timeout/reset, balanced locks, chest queue lifecycle");
    return 0;
}
