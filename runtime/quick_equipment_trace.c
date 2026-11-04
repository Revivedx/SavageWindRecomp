// Observe native equipment consumers without queueing any guest work.
#include <stdatomic.h>
#include <string.h>
#include "game_sites.h"
#include "hle.h"
#include "quick_equipment_trace.h"

extern uint64_t renderer_present_count(void);
enum { TRACE_DROP, TRACE_EQUIP, TRACE_UPDATE, TRACE_COUNT };
static GuestFn originals[TRACE_COUNT];
static _Atomic bool installed;
static struct {
    _Atomic uint64_t calls, frame, player;
    // pending drop bits 0..2; pending equipment bits 3..5; bit6 = valid sample.
    _Atomic unsigned before, after, schedule_before, schedule_after;
} observations[TRACE_COUNT];

static bool trace_enabled(void) {
    return atomic_load(&installed) && control_enabled() && !strcmp(g_game_version, "1.9.0");
}

static unsigned pending_flags(uint64_t player) {
    if (player < GUEST_LO || player >= GUEST_HI || GUEST_HI - player < 0x1471) return 0;
    return 64u | !!LD8(player + 0x13e0) | (!!LD8(player + 0x1428) << 1) |
        (!!LD8(player + 0x1470) << 2) | (!!LD8(player + 0x1120) << 3) |
        (!!LD8(player + 0x1121) << 4) | (!!LD8(player + 0x1122) << 5);
}

static void trace_call(Cpu *cpu, unsigned kind) {
    // The original sees the exact input CPU and its output is never restored or
    // changed. In particular cdc43c's x1 argument and native return PC survive.
    bool active = trace_enabled();
    uint64_t player = cpu->x[0];
    unsigned before = active ? pending_flags(player) : 0;
    unsigned schedule_before = before ? LD32(player + 0xc98) : 0;
    originals[kind](cpu);
    if (active) {
        atomic_store(&observations[kind].player, player);
        atomic_store(&observations[kind].before, before);
        atomic_store(&observations[kind].after, pending_flags(player));
        atomic_store(&observations[kind].schedule_before, schedule_before);
        atomic_store(&observations[kind].schedule_after, before ? LD32(player + 0xc98) : 0);
        atomic_store(&observations[kind].frame, renderer_present_count());
        atomic_fetch_add(&observations[kind].calls, 1);
    }
}

static void trace_drop(Cpu *cpu) { trace_call(cpu, TRACE_DROP); }
static void trace_equip(Cpu *cpu) { trace_call(cpu, TRACE_EQUIP); }
// Player virtual+260, dispatched by actor update151f090 at151f310. This
// heartbeat runs even when c98 bits4/5 skip both equipment consumers.
static void trace_update(Cpu *cpu) { trace_call(cpu, TRACE_UPDATE); }

static void trace_clear(void) {
    for (unsigned i = 0; i < TRACE_COUNT; i++) {
        atomic_store(&observations[i].calls, 0);
        atomic_store(&observations[i].frame, 0);
        atomic_store(&observations[i].player, 0);
        atomic_store(&observations[i].before, 0);
        atomic_store(&observations[i].after, 0);
        atomic_store(&observations[i].schedule_before, 0);
        atomic_store(&observations[i].schedule_after, 0);
    }
}

RT_ON_RESET(quick_equipment_trace_reset) {
    trace_clear();
    // Overrides and originals persist across in-place reboot, just like the
    // quick-menu probe. Keep exact-build validation and never wrap a wrapper.
}

void quick_equipment_trace_enable(void) {
    if (atomic_load(&installed) || !control_enabled() || strcmp(g_game_version, "1.9.0")) return;
    GuestFn drop = dispatch_lookup(0x7000cdba80ull);
    GuestFn equip = dispatch_lookup(0x7000cdc43cull);
    GuestFn update = dispatch_lookup(0x7000ccfb08ull);
    if (!drop || !equip || !update) return;
    originals[TRACE_DROP] = drop;
    originals[TRACE_EQUIP] = equip;
    originals[TRACE_UPDATE] = update;
    hle_override(0x7000cdba80ull, trace_drop, "equipment trace: native drop consumer");
    hle_override(0x7000cdc43cull, trace_equip, "equipment trace: native equip consumer");
    hle_override(0x7000ccfb08ull, trace_update, "equipment trace: native player update");
    atomic_store(&installed, true);
}

void quick_equipment_trace_command(int argc, char **argv, ControlReply *reply) {
    if (!control_enabled()) { control_fail(reply, "Developer mode required"); return; }
    if (!trace_enabled()) { control_fail(reply, "Equipment trace unavailable for this build"); return; }
    if (argc != 2 || (strcmp(argv[1], "reset") && strcmp(argv[1], "status"))) {
        control_fail(reply, "Usage: equipment-trace reset|status"); return;
    }
    if (!strcmp(argv[1], "reset")) trace_clear();
    control_field_int(reply, "frame", renderer_present_count());
    static const char *const keys[TRACE_COUNT][7] = {
        {"drop_calls", "drop_frame", "drop_player", "drop_before", "drop_after", "drop_schedule_before", "drop_schedule_after"},
        {"equip_calls", "equip_frame", "equip_player", "equip_before", "equip_after", "equip_schedule_before", "equip_schedule_after"},
        {"update_calls", "update_frame", "update_player", "update_before", "update_after", "update_schedule_before", "update_schedule_after"}
    };
    for (unsigned i = 0; i < TRACE_COUNT; i++) {
        control_field_int(reply, keys[i][0], atomic_load(&observations[i].calls));
        control_field_int(reply, keys[i][1], atomic_load(&observations[i].frame));
        control_field_int(reply, keys[i][2], atomic_load(&observations[i].player));
        control_field_int(reply, keys[i][3], atomic_load(&observations[i].before));
        control_field_int(reply, keys[i][4], atomic_load(&observations[i].after));
        control_field_int(reply, keys[i][5], atomic_load(&observations[i].schedule_before));
        control_field_int(reply, keys[i][6], atomic_load(&observations[i].schedule_after));
    }
    control_field_str(reply, "schedule", "raw player+c98; bit4 permits equip consumer, bit5 permits drop consumer");
    control_field_str(reply, "flags", "bits 0/1/2: pending weapon/shield/bow drop; 3/4/5: equip; 6: valid sample");
}
