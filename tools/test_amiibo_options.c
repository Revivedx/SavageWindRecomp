// Combined switch bridge: use the native boolean row, and restore the caller's CPU on success or failure.
#include <assert.h>
#include <string.h>
#include "../runtime/amiibo_options.c"

static Cpu cpu;
static bool have_cpu = true, have_tables = true, have_store = true, accept = true, expected;
static int calls;
static const uint32_t nsites = 1;
static const GameLibSite sites[] = {{"opt_store", 0x1234}};
static const GameLibTables tables = {.sites = sites, .nsites = &nsites};
const GameLibTables *gamelib_tables(void) { return have_tables ? &tables : NULL; }
Cpu *kernel_current_cpu(void) { return have_cpu ? &cpu : NULL; }
static void native_store(Cpu *c) {
    uint8_t *row = (void *)(uintptr_t)c->x[0];
    uint32_t index, type, choice;
    memcpy(&index, row + 0x50, sizeof index);
    memcpy(&type, row + 0x68, sizeof type);
    memcpy(&choice, row + 0x58, sizeof choice);
    assert(index == 9 && type == 0 && row[0x54] == expected);
    // AmiiboItemOnOff is a button row in 1.9.0: native opt_store uses +0x64 for this entry.
    assert(row[0x64] == expected && choice == (uint32_t)expected);
    calls++;
    memset(c, 0xa5, sizeof *c);
    c->x[0] = accept;
}
GuestFn dispatch_lookup(uint64_t address) {
    assert(address == 0x1234);
    return have_store ? native_store : NULL;
}
int main(void) {
    memset(&cpu, 0x5a, sizeof cpu);
    Cpu saved = cpu;
    expected = true; assert(amiibo_set_game_enabled(true));
    assert(!memcmp(&cpu, &saved, sizeof cpu));
    expected = false; assert(amiibo_set_game_enabled(false));
    assert(!memcmp(&cpu, &saved, sizeof cpu));
    accept = false; assert(!amiibo_set_game_enabled(false));
    assert(!memcmp(&cpu, &saved, sizeof cpu));
    have_cpu = false; assert(!amiibo_set_game_enabled(false)); have_cpu = true;
    have_tables = false; assert(!amiibo_set_game_enabled(false)); have_tables = true;
    have_store = false; assert(!amiibo_set_game_enabled(false));
    assert(calls == 3);
    puts("PASS: combined amiibo switch uses the native flag setter and preserves CPU state");
}
