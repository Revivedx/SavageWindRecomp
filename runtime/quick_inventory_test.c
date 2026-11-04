// BOTW 1.9.0 cd57b23fa4bbad65 developer fixture. Copies existing gear into
// native actor creation without removing or rewriting the source item.
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game_sites.h"
#include "hle.h"
#include "quick_inventory_test.h"
#include "quick_inventory_chest_test.h"

enum { TEST_NAME_CAP = 97 };
#define TEST_TIMEOUT_NS 5000000000ull
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool test_ready;
static uint64_t gear_storage, gear_started;
enum { GEAR_CREATE = 0, GEAR_ACTOR = 0x10, GEAR_MATRIX = 0x30,
       GEAR_MODIFIERS = 0x60, GEAR_NAME = 0x80, GEAR_STORAGE_SIZE = 0x100 };
typedef struct {
    const char *state, *reason;
    uint64_t id, queued_ns;
    uint64_t actor;
    bool chest;
    int type, index, value;
    uint32_t modifier_flags, modifier_value;
    float target[3], position[3];
    char name[TEST_NAME_CAP];
} InventoryTestStatus;
static InventoryTestStatus test_status = {.state = "idle", .reason = "not-checked", .type = -1};

RT_ON_RESET(quick_inventory_test_reset) {
    quick_inventory_chest_test_reset();
    gear_storage = gear_started = 0;
    pthread_mutex_lock(&test_mutex);
    uint64_t id = test_status.id + 1;
    memset(&test_status, 0, sizeof test_status);
    test_status.id = id;
    test_status.state = "idle";
    test_status.reason = "reset";
    test_status.type = -1;
    // The validated library and dispatch overrides survive an in-place reboot.
    pthread_mutex_unlock(&test_mutex);
}

static bool test_range(uint64_t address, uint64_t size) {
    return address >= GUEST_LO && address < GUEST_HI && size <= GUEST_HI - address;
}

static uint64_t test_call(Cpu *source, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t x2) {
    Cpu call = *source;
    call.x[0] = x0; call.x[1] = x1; call.x[2] = x2; call.x[3] = 0;
    guest_call(&call, fn);
    return call.x[0];
}

void quick_inventory_test_enable(void) {
    const uint64_t helpers[] = {0x7000eaff08ull, 0x7000eaff18ull,
        0x7000d970a8ull, 0x7000d992acull, 0x7000d1499cull,
        0x7000cc6fd0ull, 0x7000cc0334ull, 0x70007a6488ull, 0x7000be3110ull,
        0x7001516814ull, 0x7001516820ull, 0x7001516840ull, 0x7001516908ull,
        0x7001516b74ull, 0x700112649cull, 0x7001126464ull,
        0x700118effcull, 0x7000be24a0ull, 0x700152261cull};
    bool ready = !strcmp(g_game_version, "1.9.0");
    for (size_t i = 0; ready && i < sizeof helpers / sizeof *helpers; i++)
        ready = dispatch_lookup(helpers[i]) != NULL;
    if (ready) quick_inventory_chest_test_enable();
    pthread_mutex_lock(&test_mutex);
    test_ready = ready;
    if (!strcmp(test_status.state, "idle")) test_status.reason = ready ? "ready" : "missing-native-helper";
    pthread_mutex_unlock(&test_mutex);
}

static void test_expire(void) { // test_mutex held; never cancel a native call in flight
    if (!strcmp(test_status.state, "queued") && plat_now_ns() - test_status.queued_ns > TEST_TIMEOUT_NS) {
        test_status.state = "rejected";
        test_status.reason = "guest-tick-timeout";
    }
}

void quick_inventory_test_command(int argc, char **argv, ControlReply *r) {
    const char *usage = "Usage: inventory-test spawn weapon|bow|shield <zero-based-category-index> | inventory-test chest weapon|bow|shield | inventory-test status";
    bool status = argc == 2 && !strcmp(argv[1], "status");
    bool chest = argc == 3 && !strcmp(argv[1], "chest");
    int type = -1, index = -1;
    if (!control_enabled()) { control_fail(r, "Developer mode required"); return; }
    if (!status) {
        if (!chest && (argc != 4 || strcmp(argv[1], "spawn"))) { control_fail(r, "%s", usage); return; }
        type = !strcmp(argv[2], "weapon") ? 0 : !strcmp(argv[2], "bow") ? 1 : !strcmp(argv[2], "shield") ? 3 : -1;
        if (type < 0) { control_fail(r, "%s", usage); return; }
        if (!chest) {
            char *end;
            errno = 0;
            long parsed = strtol(argv[3], &end, 10);
            if (!*argv[3] || *end || errno || parsed < 0 || parsed >= 20) {
                control_fail(r, "%s", usage); return;
            }
            index = (int)parsed;
        }
    }
    pthread_mutex_lock(&test_mutex);
    test_expire();
    bool ready = test_ready && !strcmp(g_game_version, "1.9.0");
    const char *error = NULL;
    if (!status) {
        if (!ready) error = "Unsupported or unavailable native build";
        else if (!strcmp(test_status.state, "queued") || !strcmp(test_status.state, "running") ||
                 !strcmp(test_status.state, "spawning"))
            error = "An inventory-test request is already outstanding";
        else {
            uint64_t id = test_status.id + 1;
            memset(&test_status, 0, sizeof test_status);
            test_status.id = id; test_status.type = type; test_status.index = index;
            test_status.chest = chest;
            if (chest) snprintf(test_status.name, sizeof test_status.name, "%s",
                type == 0 ? "Weapon_Sword_001" : type == 1 ? "Weapon_Bow_001" : "Weapon_Shield_001");
            test_status.queued_ns = plat_now_ns();
            test_status.state = "queued"; test_status.reason = "waiting-for-guest-tick";
        }
    }
    __typeof__(test_status) snapshot = test_status;
    pthread_mutex_unlock(&test_mutex);
    if (error) { control_fail(r, "%s", error); return; }
    control_field_bool(r, "ready", ready);
    control_field_int(r, "request", (int64_t)snapshot.id);
    control_field_str(r, "state", snapshot.state);
    control_field_str(r, "reason", snapshot.reason);
    control_field_int(r, "type", snapshot.type);
    control_field_int(r, "index", snapshot.index);
    control_field_str(r, "source", snapshot.name);
    control_field_str(r, "kind", snapshot.chest ? "chest" : "gear");
    char actor[32]; snprintf(actor, sizeof actor, "0x%" PRIx64, snapshot.actor);
    control_field_str(r, "actor", actor);
    control_field_int(r, "value", snapshot.value);
    control_field_int(r, "modifier_flags", snapshot.modifier_flags);
    control_field_int(r, "modifier_value", snapshot.modifier_value);
    if (!snapshot.chest) {
        control_field_num(r, "target_x", snapshot.target[0]);
        control_field_num(r, "target_y", snapshot.target[1]);
        control_field_num(r, "target_z", snapshot.target[2]);
    }
    if (snapshot.actor) {
        control_field_num(r, "actor_x", snapshot.position[0]);
        control_field_num(r, "actor_y", snapshot.position[1]);
        control_field_num(r, "actor_z", snapshot.position[2]);
    }
}

static bool test_name(uint64_t item, char out[TEST_NAME_CAP]) {
    uint64_t address = LD64(item + 0x30);
    if (!test_range(address, TEST_NAME_CAP)) return false;
    for (int i = 0; i < TEST_NAME_CAP; i++) {
        unsigned ch = LD8(address + i);
        if (!ch) { out[i] = 0; return i != 0; }
        if (ch < 32 || ch > 126) return false;
        out[i] = (char)ch;
    }
    out[TEST_NAME_CAP - 1] = 0;
    return false;
}

static bool gear_stack(Cpu *cpu) {
    return !(cpu->sp & 15) && cpu->sp >= GUEST_LO + 0x1000 &&
        test_range(cpu->sp - 0x1000, 0x1000);
}

static void gear_release(Cpu *cpu) {
    test_call(cpu, 0x7001516908ull, gear_storage + GEAR_CREATE, 0, 0);
    test_call(cpu, 0x7001126464ull, gear_storage + GEAR_ACTOR, 0, 0);
}

// Same be3110 ABI as b25bf4, with a creation handle owned by this fixture.
// Native creation tasks have exactly one owner address; queue handles cannot
// be copied or retained after the native queue recycles their nodes.
static bool gear_begin(Cpu *cpu, uint64_t item, uint64_t player,
                       InventoryTestStatus *request, const char **reason) {
    *reason = "invalid-guest-stack";
    if (!gear_stack(cpu)) return false;
    *reason = "missing-gear-creator";
    uint64_t holder = LD64(0x7001d66558ull);
    if (!test_range(holder, 8)) return false;
    uint64_t owner = LD64(holder);
    if (!test_range(owner, 0x30)) return false;
    uint64_t creator = LD64(owner + 0x28);
    // GOT+558 is the native creation-context object, separate from Link.
    // Its +28 argument is opaque; do not interpret it as a creator name.
    if (!test_range(creator, 8)) return false;
    *reason = "missing-spawn-manager";
    holder = LD64(0x7001d66550ull);
    if (!test_range(holder, 8) || !test_range(LD64(holder), 0x90)) return false;
    *reason = "invalid-player-transform";
    float matrix[12];
    memcpy(matrix, (const void *)(uintptr_t)(player + 0x398), sizeof matrix);
    for (unsigned i = 0; i < 12; i++) if (!isfinite(matrix[i])) return false;
    float facing = matrix[2]*matrix[2] + matrix[6]*matrix[6] + matrix[10]*matrix[10];
    if (facing < 0.8f || facing > 1.2f) return false;
    // Fixtures belong in front of Link (+localZ). Native trash uses -localZ
    // to place discarded gear behind him; retain only its vertical lift here.
    for (unsigned row = 0; row < 3; row++)
        request->target[row] = matrix[row*4 + 3] += matrix[row*4 + 1]*0.8f + matrix[row*4 + 2]*2.5f;
    bool fresh = !gear_storage;
    if (fresh) gear_storage = (uint64_t)(uintptr_t)hle_alloc(GEAR_STORAGE_SIZE);
    *reason = "gear-allocation-failed";
    if (!test_range(gear_storage, GEAR_STORAGE_SIZE)) return false;
    if (!fresh) gear_release(cpu);
    memset((void *)(uintptr_t)gear_storage, 0, GEAR_STORAGE_SIZE);
    test_call(cpu, 0x7001516814ull, gear_storage + GEAR_CREATE, 0, 0);
    test_call(cpu, 0x70007a6488ull, gear_storage + GEAR_MODIFIERS, item, 0);
    request->modifier_flags = LD32(gear_storage + GEAR_MODIFIERS);
    request->modifier_value = LD32(gear_storage + GEAR_MODIFIERS + 4);
    memcpy((void *)(uintptr_t)(gear_storage + GEAR_MATRIX), matrix, sizeof matrix);
    memcpy((void *)(uintptr_t)(gear_storage + GEAR_NAME), request->name, strlen(request->name) + 1);
    Cpu create = *cpu;
    // The ninth integer argument lives at caller SP; preserve a separate guest
    // scratch slot above the lowered SP while callees use the stack below it.
    create.sp -= 0x100;
    ST32(create.sp, 2);
    create.x[0] = gear_storage + GEAR_NAME;
    create.x[1] = gear_storage + GEAR_MATRIX;
    create.x[2] = creator;
    create.x[3] = gear_storage + GEAR_CREATE;
    create.x[4] = request->value; create.x[5] = 0;
    create.x[6] = gear_storage + GEAR_MODIFIERS; create.x[7] = 2;
    create.v[0] = 0x3f800000;
    guest_call(&create, 0x7000be3110ull);
    if (!(create.x[0] & 1)) {
        gear_release(cpu); *reason = "native-copy-rejected"; return false;
    }
    gear_started = plat_now_ns();
    *reason = "native-copy-requested";
    return true;
}

static int gear_poll(Cpu *cpu, InventoryTestStatus *request, const char **reason) {
    *reason = "invalid-guest-stack";
    if (!gear_stack(cpu)) return 0;
    uint64_t handle = gear_storage + GEAR_CREATE;
    if (test_call(cpu, 0x7001516820ull, handle, 0, 0)) {
        uint64_t actor = test_call(cpu, 0x7001516b74ull, handle, 0, 0);
        test_call(cpu, 0x7001516908ull, handle, 0, 0);
        *reason = "native-copy-missing-actor";
        if (test_range(actor, 0x5b0) &&
            test_call(cpu, 0x700112649cull, gear_storage + GEAR_ACTOR, actor, 0) &&
            LD64(gear_storage + GEAR_ACTOR + 8) == actor) {
            // Match the native b25a88 success path's registration, equipment
            // model orientation/ground adjustment and transform application.
            uint64_t holder = LD64(0x7001d66310ull);
            if (!test_range(holder, 8) || !test_range(LD64(holder), 0x100)) {
                gear_release(cpu); *reason = "missing-gear-placement-manager"; return -1;
            }
            test_call(cpu, 0x700118effcull, LD64(holder) + 0xf0, actor, 1);
            uint64_t matrix = gear_storage + GEAR_MATRIX;
            memcpy((void *)(uintptr_t)matrix, (const void *)(uintptr_t)(actor + 0x398), 0x30);
            test_call(cpu, 0x7000be24a0ull, matrix, actor, 0);
            test_call(cpu, 0x700152261cull, actor, matrix, 0);
            request->actor = actor;
            for (unsigned row = 0; row < 3; row++)
                memcpy(&request->position[row], (const void *)(uintptr_t)(actor + 0x3a4 + row*16), 4);
            *reason = "native-copy-created";
            return 1;
        }
        gear_release(cpu); return -1;
    }
    if (test_call(cpu, 0x7001516840ull, handle, 0, 0)) {
        gear_release(cpu); *reason = "native-copy-creation-failed"; return -1;
    }
    if (plat_now_ns() - gear_started > 10000000000ull) {
        gear_release(cpu); *reason = "native-copy-creation-timeout"; return -1;
    }
    *reason = "native-copy-pending";
    return 0;
}

void quick_inventory_test_tick(Cpu *cpu) {
    pthread_mutex_lock(&test_mutex);
    test_expire();
    if (!strcmp(test_status.state, "spawning")) {
        __typeof__(test_status) request = test_status;
        test_status.state = "running";
        pthread_mutex_unlock(&test_mutex);
        const char *reason = "pending-native-creation";
        int result = request.chest ? quick_inventory_chest_test_poll(cpu, &request.actor, &reason) :
            gear_poll(cpu, &request, &reason);
        if (result > 0 && request.chest && test_range(request.actor, 0x3c8))
            for (unsigned row = 0; row < 3; row++)
                memcpy(&request.position[row], (const void *)(uintptr_t)(request.actor + 0x3a4 + row*16), 4);
        request.reason = reason;
        if (result) {
            request.state = result > 0 ? "created" : "rejected";
            LOG("[inventory-test] request=%" PRIu64 " state=%s reason=%s source=%s actor=%" PRIx64 " position=%.3f,%.3f,%.3f\n",
                request.id, request.state, request.reason, request.name, request.actor,
                request.position[0], request.position[1], request.position[2]);
        }
        pthread_mutex_lock(&test_mutex);
        if (test_status.id == request.id) test_status = request;
        pthread_mutex_unlock(&test_mutex);
        return;
    }
    if (strcmp(test_status.state, "queued")) { pthread_mutex_unlock(&test_mutex); return; }
    __typeof__(test_status) request = test_status;
    test_status.state = "running";
    bool ready = test_ready;
    pthread_mutex_unlock(&test_mutex);
    const char *reason = "unsupported-build";
    const char *state = "rejected";
    uint64_t mgr = 0;
    bool locked = false;
    Cpu scratch;
    if (!ready || strcmp(g_game_version, "1.9.0") || !control_enabled()) goto done;
    reason = "invalid-guest-stack";
    if ((cpu->sp & 15) || cpu->sp < GUEST_LO + 0x100 || !test_range(cpu->sp - 0x100, 0x100)) goto done;
    scratch = *cpu; scratch.sp -= 0x100;
    reason = "active-event";
    if (test_call(&scratch, 0x7000d1499cull, 0, 0, 0)) goto done;
    reason = "missing-player";
    if (!test_range(g_site.player_info, 8)) goto done;
    uint64_t info = LD64(g_site.player_info);
    if (!test_range(info, 0x68)) goto done;
    uint64_t player = test_call(&scratch, 0x7000cc6fd0ull, info, 0, 0);
    if (!test_range(player, 0xcf9)) goto done;
    reason = "player-state";
    if ((LD8(player + 0xcf8) & 4) || test_call(&scratch, 0x7000cc0334ull, player, 0, 0)) goto done;
    if (request.chest) {
        if (quick_inventory_chest_test_begin(cpu, request.name, &reason)) state = "spawning";
        goto done;
    }
    reason = "missing-inventory";
    uint64_t holder = LD64(0x7001d67af0ull);
    if (!test_range(holder, 8)) goto done;
    mgr = LD64(holder);
    if (!test_range(mgr, 0x45000)) goto done;
    test_call(&scratch, 0x7000eaff08ull, mgr + 0x28, 0, 0); locked = true;
    reason = "invalid-inventory";
    int tabs = (int32_t)LD32(mgr + 0x44498), category_index = 0;
    if (tabs < 0 || tabs > 50) goto done;
    uint64_t item = 0;
    for (int tab = 0; tab < tabs && !item; tab++) {
        int count = (int32_t)test_call(&scratch, 0x7000d970a8ull, mgr, tab, 0);
        if (count < 0 || count > 20) goto done;
        for (int slot = 0; slot < count; slot++) {
            uint64_t candidate = test_call(&scratch, 0x7000d992acull, mgr, tab, slot);
            if (!test_range(candidate, 0x8c)) goto done;
            if ((int32_t)LD32(candidate + 0x18) == request.type && category_index++ == request.index) {
                item = candidate; break;
            }
        }
    }
    reason = "category-index-missing";
    if (!item) goto done;
    request.value = (int32_t)LD32(item + 0x20);
    reason = "invalid-item";
    if (!LD8(item + 0x25) || request.value <= 0 || !test_name(item, request.name)) goto done;
    reason = "protected-name";
    if (!strcmp(request.name, "Weapon_Sword_070") || !strcmp(request.name, "Weapon_Sword_502")) goto done;
    if (gear_begin(&scratch, item, player, &request, &reason)) state = "spawning";
done:
    if (locked) test_call(&scratch, 0x7000eaff18ull, mgr + 0x28, 0, 0);
    request.state = state;
    request.reason = reason;
    LOG("[inventory-test] request=%" PRIu64 " state=%s reason=%s source=%s type=%d index=%d value=%d target=%.3f,%.3f,%.3f modifiers=%x,%u; source inventory retained\n",
        request.id, request.state, request.reason, request.name, request.type, request.index,
        request.value, request.target[0], request.target[1], request.target[2],
        request.modifier_flags, request.modifier_value);
    pthread_mutex_lock(&test_mutex);
    if (test_status.id == request.id) test_status = request;
    pthread_mutex_unlock(&test_mutex);
}
