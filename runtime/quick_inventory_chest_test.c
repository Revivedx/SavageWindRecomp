// Native, developer-only BOTW 1.9.0 chest fixture. See the inspected native
// chest caller 6c60b8 and creation-handle consumer b25a88. No pouch/award hooks.
#include <math.h>
#include <string.h>
#include "control.h"
#include "game_sites.h"
#include "hle.h"
#include "quick_inventory_chest_test.h"

enum { CHEST_BLOCK_SIZE = 0x200, CHEST_CREATE = 0xe0, CHEST_ACTOR = 0xf0,
       CHEST_MATRIX = 0x110, CHEST_KEY = 0x140, CHEST_VALUE = 0x150,
       CHEST_NAME = 0x160 };
#define CHEST_TIMEOUT_NS 10000000000ull
static bool chest_ready, chest_pending;
static uint64_t chest_storage, chest_started;

static bool chest_range(uint64_t address, uint64_t size) {
    return address >= GUEST_LO && address < GUEST_HI && size <= GUEST_HI - address;
}

static bool chest_stack(Cpu *cpu) {
    return cpu && !(cpu->sp & 15) && cpu->sp >= GUEST_LO + 0x1000 &&
        chest_range(cpu->sp - 0x1000, 0x1000);
}

static uint64_t chest_call(Cpu *cpu, uint64_t fn, uint64_t x0, uint64_t x1,
                           uint64_t x2, uint64_t x3, uint64_t x4) {
    Cpu call = *cpu;
    call.x[0] = x0; call.x[1] = x1; call.x[2] = x2;
    call.x[3] = x3; call.x[4] = x4; call.x[5] = call.x[6] = call.x[7] = 0;
    guest_call(&call, fn);
    return call.x[0];
}

static void chest_release(Cpu *cpu) {
    if (!chest_storage) return;
    // A creation handle and an ordinary ActorLink have different layouts and
    // cleanup functions. Release the request even on native rejection/timeout.
    chest_call(cpu, 0x7001516908ull, chest_storage + CHEST_CREATE, 0, 0, 0, 0);
    chest_call(cpu, 0x7001126464ull, chest_storage + CHEST_ACTOR, 0, 0, 0, 0);
    chest_pending = false;
}

void quick_inventory_chest_test_reset(void) {
    chest_storage = chest_started = 0;
    chest_pending = false;
    // The exact-build gate/dispatch survive reboot; all guest pointers do not.
}

void quick_inventory_chest_test_enable(void) {
    const uint64_t helpers[] = {0x7000d1499cull, 0x7000cc6fd0ull,
        0x7000cc0334ull, 0x700112615cull, 0x7001126168ull,
        0x7000589948ull, 0x70015363d4ull, 0x7001535b10ull,
        0x7001516814ull, 0x7001516820ull, 0x7001516840ull,
        0x7001516908ull, 0x7001516b74ull, 0x700112649cull, 0x7001126464ull};
    chest_ready = !strcmp(g_game_version, "1.9.0");
    for (size_t i = 0; chest_ready && i < sizeof helpers / sizeof *helpers; i++)
        chest_ready = dispatch_lookup(helpers[i]) != NULL;
}

static void chest_key(uint64_t safe_vtable, uint64_t text) {
    ST64(chest_storage + CHEST_KEY, safe_vtable);
    ST64(chest_storage + CHEST_KEY + 8, text);
}

bool quick_inventory_chest_test_begin(Cpu *cpu, const char *reward, const char **reason) {
    *reason = "unsupported-chest-build";
    if (!chest_ready || strcmp(g_game_version, "1.9.0") || !control_enabled()) return false;
    *reason = "invalid-chest-stack";
    if (!chest_stack(cpu)) return false;
    *reason = "chest-already-spawning";
    if (chest_pending) return false;
    *reason = "unsupported-chest-reward";
    if (!reward || (strcmp(reward, "Weapon_Sword_001") && strcmp(reward, "Weapon_Bow_001") &&
                   strcmp(reward, "Weapon_Shield_001"))) return false;
    *reason = "active-event";
    if (chest_call(cpu, 0x7000d1499cull, 0, 0, 0, 0, 0)) return false;
    *reason = "missing-chest-player-info-slot";
    if (!chest_range(g_site.player_info, 8)) return false;
    uint64_t info = LD64(g_site.player_info);
    *reason = "missing-chest-player-info";
    if (!chest_range(info, 0xa0)) return false;
    uint64_t player = chest_call(cpu, 0x7000cc6fd0ull, info, 0, 0, 0, 0);
    *reason = "missing-chest-player";
    if (!chest_range(player, 0xcf9)) return false;
    if ((LD8(player + 0xcf8) & 4) || chest_call(cpu, 0x7000cc0334ull, player, 0, 0, 0, 0)) {
        *reason = "player-state"; return false;
    }
    float position[3];
    memcpy(position, (const void *)(uintptr_t)(info + 0x88), sizeof position);
    *reason = "invalid-chest-player-position";
    if (!isfinite(position[0]) || !isfinite(position[1]) || !isfinite(position[2]) ||
        (!position[0] && !position[1] && !position[2])) return false;
    //10726a4 returns actor+398 as matrix34. Fixtures use +localZ, in front
    // of Link, intentionally reversing native trash's -localZ placement.
    float matrix[12];
    memcpy(matrix, (const void *)(uintptr_t)(player + 0x398), sizeof matrix);
    *reason = "invalid-chest-player-matrix";
    for (unsigned i = 0; i < 12; i++) if (!isfinite(matrix[i])) return false;
    float facing_length = matrix[2]*matrix[2] + matrix[6]*matrix[6] + matrix[10]*matrix[10];
    *reason = "invalid-chest-player-facing";
    if (facing_length < 0.8f || facing_length > 1.2f) return false;
    for (unsigned row = 0; row < 3; row++)
        matrix[row*4 + 3] += matrix[row*4 + 1]*0.5f + matrix[row*4 + 2]*2.5f;
    *reason = "missing-chest-manager-slot";
    uint64_t holder = LD64(0x7001d66550ull);
    if (!chest_range(holder, 8)) return false;
    uint64_t manager = LD64(holder);
    *reason = "missing-chest-manager";
    if (!chest_range(manager, 0x90)) return false;
    holder = LD64(0x7001d66558ull);
    *reason = "missing-chest-context-slot";
    if (!chest_range(holder, 8)) return false;
    uint64_t context = LD64(holder);
    *reason = "missing-chest-context";
    if (!chest_range(context, 0x30)) return false;
    // b25d54/b25d58 reads this separate singleton's +28. It is an opaque
    // creation argument (1535a08 stores it at args+8), not Player or a string.
    uint64_t creation_arg = LD64(context + 0x28);
    *reason = "missing-chest-creation-argument";
    if (!chest_range(creation_arg, 8)) return false;
    uint64_t safe_type = LD64(0x7001d66260ull), param_type = LD64(0x7001d664b0ull);
    *reason = "missing-chest-string-type";
    if (!chest_range(safe_type, 0x30)) return false;
    *reason = "missing-chest-param-type";
    if (!chest_range(param_type, 0x30)) return false;

    // One small guest-visible block per boot. The native parameter buffer copies
    // reward bytes, but retains key pointers; all keys below are module rodata.
    bool new_storage = !chest_storage;
    if (new_storage) chest_storage = (uint64_t)(uintptr_t)hle_alloc(CHEST_BLOCK_SIZE);
    *reason = "chest-allocation-failed";
    if (!chest_range(chest_storage, CHEST_BLOCK_SIZE)) return false;
    if (!new_storage) chest_release(cpu);
    memset((void *)(uintptr_t)chest_storage, 0, CHEST_BLOCK_SIZE);
    ST64(chest_storage, param_type + 0x10);
    chest_call(cpu, 0x700112615cull, chest_storage + 0x10, 0, 0, 0, 0);
    chest_call(cpu, 0x7001516814ull, chest_storage + CHEST_CREATE, 0, 0, 0, 0);
    uint64_t safe = safe_type + 0x10;
    strcpy((char *)(uintptr_t)(chest_storage + CHEST_NAME), reward);
    ST64(chest_storage + CHEST_VALUE, safe);
    ST64(chest_storage + CHEST_VALUE + 8, chest_storage + CHEST_NAME);
    chest_key(safe, 0x70016382bfull); // DropActor, as native6c60e0.
    chest_call(cpu, 0x7000589948ull, chest_storage + 0x10,
        chest_storage + CHEST_VALUE, chest_storage + CHEST_KEY, 0, 0);
    chest_key(safe, 0x700162c7e8ull); // SharpWeaponJudgeType; no reward modifier override.
    ST32(chest_storage + CHEST_MATRIX, 0);
    chest_call(cpu, 0x7001126168ull, chest_storage + 0x10,
        chest_storage + CHEST_MATRIX, chest_storage + CHEST_KEY, 4, 0);
    Cpu scale = *cpu; scale.x[0] = chest_storage; scale.v[0] = 0x3f800000;
    guest_call(&scale, 0x70015363d4ull); // Native @S vector from uniform scale.
    memcpy((void *)(uintptr_t)(chest_storage + CHEST_MATRIX), matrix, sizeof matrix);
    chest_key(safe, 0x700160f9e5ull); // @M matrix34.
    chest_call(cpu, 0x7001126168ull, chest_storage + 0x10,
        chest_storage + CHEST_MATRIX, chest_storage + CHEST_KEY, 0x30, 7);
    Cpu create = *cpu;
    create.x[0] = manager; create.x[1] = 0x70015fe37cull; // TBox_Field_Iron.
    create.x[2] = creation_arg; create.x[3] = chest_storage + CHEST_CREATE;
    create.x[4] = chest_storage; create.x[5] = 0; create.x[6] = 1; create.x[7] = 0;
    guest_call(&create, 0x7001535b10ull);
    if (!(create.x[0] & 1)) {
        chest_release(cpu); *reason = "native-chest-request-rejected"; return false;
    }
    chest_started = plat_now_ns();
    chest_pending = true;
    *reason = "native-chest-requested";
    return true;
}

int quick_inventory_chest_test_poll(Cpu *cpu, uint64_t *actor, const char **reason) {
    *actor = 0;
    *reason = "no-chest-request";
    if (!chest_pending) return -1;
    *reason = "invalid-chest-stack";
    if (!chest_stack(cpu)) return 0; // Keep native storage until a valid guest tick/reset.
    uint64_t request = chest_storage + CHEST_CREATE;
    if (chest_call(cpu, 0x7001516820ull, request, 0, 0, 0, 0)) {
        uint64_t created = chest_call(cpu, 0x7001516b74ull, request, 0, 0, 0, 0);
        //1516b74 consumes the task pointer; normalize the remaining empty
        // creation handle with its own native release, retaining only ActorLink.
        chest_call(cpu, 0x7001516908ull, request, 0, 0, 0, 0);
        if (chest_range(created, 0x30) &&
            chest_call(cpu, 0x700112649cull, chest_storage + CHEST_ACTOR, created, 0, 0, 0)) {
            *actor = LD64(chest_storage + CHEST_ACTOR + 8);
            if (*actor == created) {
                chest_pending = false;
                *reason = "native-chest-created";
                return 1;
            }
        }
        chest_release(cpu); *reason = "native-chest-missing-actor"; return -1;
    }
    if (chest_call(cpu, 0x7001516840ull, request, 0, 0, 0, 0)) {
        chest_release(cpu); *reason = "native-chest-creation-failed"; return -1;
    }
    if (plat_now_ns() - chest_started > CHEST_TIMEOUT_NS) {
        chest_release(cpu); *reason = "native-chest-creation-timeout"; return -1;
    }
    *reason = "native-chest-pending";
    return 0;
}
