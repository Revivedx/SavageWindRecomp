// Native chest fixture ABI/lifetime checks. No native game binary is executed.
#include <assert.h>
#include <sys/mman.h>
#include "../runtime/quick_inventory_chest_test.c"

const char *g_game_version = "1.9.0";
GameSites g_site;
static const uint64_t objects = 0x7240000000ull, storage = 0x7240040000ull;
static const uint64_t player = 0x7240010000ull, info = 0x7240001000ull;
static const uint64_t created_actor = 0x7240020000ull;
static bool developer = true, missing_helper, active_event, bad_player, reject_request;
static bool creation_success, creation_failure, missing_actor;
static bool rotated;
static uint64_t now = 100, request_handle;
static unsigned calls, creates, finalizes, request_releases, actor_releases, actor_refs, allocations;
static unsigned entries;
static struct { uint64_t key; unsigned size, type; unsigned char value[0x60]; } params[4];
uint64_t plat_now_ns(void) { return now; }
bool control_enabled(void) { return developer; }
void *hle_alloc(uint64_t size) {
    assert(size == CHEST_BLOCK_SIZE); allocations++;
    memset((void *)(uintptr_t)storage, 0, size); return (void *)(uintptr_t)storage;
}
static void dummy(Cpu *c) { (void)c; }
GuestFn dispatch_lookup(uint64_t address) { (void)address; return missing_helper ? NULL : dummy; }

static void param(uint64_t key, uint64_t value, unsigned size, unsigned type) {
    assert(entries < 4 && size <= sizeof params[0].value);
    params[entries].key = key; params[entries].size = size; params[entries].type = type;
    memcpy(params[entries++].value, (void *)(uintptr_t)value, size);
}

void guest_call(Cpu *c, uint64_t address) {
    calls++;
    switch (address) {
    case 0x7000d1499cull: c->x[0] = active_event; break;
    case 0x7000cc6fd0ull: assert(c->x[0] == info); c->x[0] = player; break;
    case 0x7000cc0334ull: assert(c->x[0] == player); c->x[0] = bad_player; break;
    case 0x700112615cull:
        assert(c->x[0] == storage + 0x10);
        assert(LD64(storage) == objects + 0x31010 && LD64(storage + 8) == 0);
        memset((void *)(uintptr_t)c->x[0], 0, 0xc4); entries = 0; break;
    case 0x7001516814ull:
        assert(c->x[0] == storage + CHEST_CREATE);
        memset((void *)(uintptr_t)c->x[0], 0, 0x10); break;
    case 0x7000589948ull:
        assert(c->x[0] == storage + 0x10 && c->x[1] == storage + CHEST_VALUE);
        assert(LD64(c->x[1]) == objects + 0x30010);
        assert(LD64(c->x[2]) == objects + 0x30010);
        assert(LD64(c->x[2] + 8) == 0x70016382bfull);
        param(LD64(c->x[2] + 8), LD64(c->x[1] + 8),
              strlen((char *)(uintptr_t)LD64(c->x[1] + 8)) + 1, 5); break;
    case 0x7001126168ull:
        assert(c->x[0] == storage + 0x10 && LD64(c->x[2]) == objects + 0x30010);
        param(LD64(c->x[2] + 8), c->x[1], c->x[3], c->x[4]); break;
    case 0x70015363d4ull: {
        assert(c->x[0] == storage && (uint32_t)c->v[0] == 0x3f800000);
        float scale[3] = {1,1,1};
        memcpy((void *)(uintptr_t)(storage + 0x1e0), scale, sizeof scale);
        param(0x70015d0947ull, storage + 0x1e0, 12, 4); break;
    }
    case 0x7001535b10ull: {
        assert(c->x[0] == objects + 0x32000 && c->x[1] == 0x70015fe37cull);
        assert(c->x[2] == objects + 0x33000 && c->x[3] == storage + CHEST_CREATE);
        assert(c->x[4] == storage && c->x[5] == 0 && c->x[6] == 1);
        assert(entries == 4);
        assert(params[0].key == 0x70016382bfull && params[0].type == 5);
        assert(params[1].key == 0x700162c7e8ull && params[1].type == 0 &&
               params[1].size == 4 && *(uint32_t *)params[1].value == 0);
        assert(params[2].key == 0x70015d0947ull && params[2].type == 4);
        assert(params[3].key == 0x700160f9e5ull && params[3].type == 7 && params[3].size == 48);
        float matrix[12]; memcpy(matrix, params[3].value, sizeof matrix);
        assert(matrix[5] == 1 && matrix[7] == 200.5f);
        if (rotated) {
            assert(matrix[2] == 1 && matrix[8] == -1 && matrix[10] == 0);
            assert(matrix[3] == 102.5f && matrix[11] == 300);
        } else {
            assert(matrix[0] == 1 && matrix[10] == 1);
            assert(matrix[3] == 100 && matrix[11] == 302.5f);
        }
        request_handle = c->x[3]; ST64(request_handle, objects + 0x34000);
        creates++; c->x[0] = !reject_request; break;
    }
    case 0x7001516820ull: assert(c->x[0] == request_handle); c->x[0] = creation_success; break;
    case 0x7001516840ull: assert(c->x[0] == request_handle); c->x[0] = creation_failure; break;
    case 0x7001516b74ull:
        assert(creation_success && c->x[0] == request_handle);
        finalizes++; ST64(request_handle, 0); c->x[0] = missing_actor ? 0 : created_actor; break;
    case 0x700112649cull:
        assert(c->x[0] == storage + CHEST_ACTOR && c->x[1] == created_actor);
        ST8(c->x[0], 1); ST64(c->x[0] + 8, created_actor); actor_refs++; c->x[0] = 1; break;
    case 0x7001516908ull:
        assert(c->x[0] == storage + CHEST_CREATE);
        if (LD64(c->x[0])) request_releases++;
        memset((void *)(uintptr_t)c->x[0], 0, 0x10); break;
    case 0x7001126464ull:
        assert(c->x[0] == storage + CHEST_ACTOR);
        if (LD64(c->x[0] + 8)) actor_releases++;
        memset((void *)(uintptr_t)c->x[0], 0, 0x18); break;
    default: fprintf(stderr, "Unexpected native call %llx\n", (unsigned long long)address); abort();
    }
    // Helpers may clobber every caller-saved register, SP and condition flags.
    c->x[1] = 0xdead; c->pc = 0xbeef; c->v[0] = 17; c->nzcv = 19; c->sp -= 32;
}

int main(void) {
    assert(mmap((void *)(uintptr_t)objects, 0x50000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)objects);
    assert(mmap((void *)0x7001d66000ull, 0x1000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)0x7001d66000ull);
    g_site.player_info = objects; ST64(objects, info);
    float position[3] = {100,200,300}; memcpy((void *)(uintptr_t)(info + 0x88), position, sizeof position);
    float transform[12] = {1,0,0,100, 0,1,0,200, 0,0,1,300};
    memcpy((void *)(uintptr_t)(player + 0x398), transform, sizeof transform);
    ST64(0x7001d66550ull, objects + 0x10); ST64(objects + 0x10, objects + 0x32000);
    // Native creation context differs from Player; +28 is opaque binary data,
    // not a raw name. A zero first byte is valid and must not fail a string test.
    ST64(0x7001d66558ull, objects + 0x18); ST64(objects + 0x18, objects + 0x35000);
    ST64(objects + 0x35028, objects + 0x33000); ST64(player + 0x28, 0);
    ST64(0x7001d66260ull, objects + 0x30000); ST64(0x7001d664b0ull, objects + 0x31000);
    Cpu cpu = {.sp = objects + 0x4f000, .pc = 123, .nzcv = 0xa0000000};
    cpu.x[1] = 42; cpu.v[0] = 43; Cpu original = cpu;
    const char *reason; uint64_t actor;
    assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason) && calls == 0);
    missing_helper = true; quick_inventory_chest_test_enable();
    assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason) && calls == 0);
    missing_helper = false; quick_inventory_chest_test_enable();
    developer = false; assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason)); developer = true;
    g_game_version = "1.8.2"; assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason)); g_game_version = "1.9.0";
    assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_070", &reason));
    active_event = true; assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason)); active_event = false;
    bad_player = true; assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason)); bad_player = false;
    ST64(objects, 0); assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason)); ST64(objects, info);
    ST64(objects + 0x18, 0);
    assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason) &&
           !strcmp(reason, "missing-chest-context"));
    ST64(objects + 0x18, objects + 0x35000); ST64(objects + 0x35028, 0);
    assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason) &&
           !strcmp(reason, "missing-chest-creation-argument"));
    ST64(objects + 0x35028, objects + 0x33000);
    Cpu invalid = cpu; invalid.sp++;
    unsigned before = calls;
    assert(!quick_inventory_chest_test_begin(&invalid, "Weapon_Sword_001", &reason) && calls == before);
    char reward[] = "Weapon_Sword_001";
    assert(quick_inventory_chest_test_begin(&cpu, reward, &reason));
    assert(!strcmp(reason, "native-chest-requested") && creates == 1 && allocations == 1);
    memset(reward, 'x', sizeof reward - 1);
    assert(!strcmp((char *)params[0].value, "Weapon_Sword_001"));
    assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Bow_001", &reason));
    assert(quick_inventory_chest_test_poll(&cpu, &actor, &reason) == 0 && !actor && !finalizes);
    creation_success = true;
    assert(quick_inventory_chest_test_poll(&cpu, &actor, &reason) == 1 && actor == created_actor);
    assert(finalizes == 1 && actor_refs == 1 && !request_releases && !actor_releases);
    creation_success = false; creation_failure = true;
    float facing_east[12] = {0,0,1,100, 0,1,0,200, -1,0,0,300};
    memcpy((void *)(uintptr_t)(player + 0x398), facing_east, sizeof facing_east); rotated = true;
    assert(quick_inventory_chest_test_begin(&cpu, "Weapon_Bow_001", &reason));
    assert(actor_releases == 1 && allocations == 1);
    assert(quick_inventory_chest_test_poll(&cpu, &actor, &reason) == -1 && !actor && request_releases == 1);
    creation_failure = false;
    assert(quick_inventory_chest_test_begin(&cpu, "Weapon_Shield_001", &reason));
    now += CHEST_TIMEOUT_NS + 1;
    assert(quick_inventory_chest_test_poll(&cpu, &actor, &reason) == -1 && !strcmp(reason, "native-chest-creation-timeout"));
    reject_request = true;
    assert(!quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason));
    assert(!chest_pending && request_releases == 3); reject_request = false;
    assert(quick_inventory_chest_test_begin(&cpu, "Weapon_Sword_001", &reason));
    creation_success = missing_actor = true;
    assert(quick_inventory_chest_test_poll(&cpu, &actor, &reason) == -1 && !actor);
    assert(!memcmp(&cpu, &original, sizeof cpu));
    before = calls; quick_inventory_chest_test_reset();
    assert(!chest_storage && !chest_pending && calls == before);
    puts("quick inventory chest spawn ABI/lifetime checks passed");
    return 0;
}
