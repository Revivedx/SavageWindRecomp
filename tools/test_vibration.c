// Synthetic HID -> SDL checks, with no game files or physical controller. make test-vibration
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "../runtime/hle/vibration.c"
#include "../runtime/services/hid_set.c"

Settings g_settings = {.rumble = true};

static uint64_t now;
static unsigned calls;
static uint16_t motor_low, motor_high;
static uint32_t duration;
static int rumble_delay_ms;
static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}
Uint64 SDL_GetTicks(void) { return now; }
bool SDL_RumbleGamepad(SDL_Gamepad *pad, Uint16 low, Uint16 high, Uint32 ms) {
    assert(pad);
    if (rumble_delay_ms > 0) sleep_ms(rumble_delay_ms);
    motor_low = low; motor_high = high; duration = ms; calls++;
    return true;
}
static void update_sync(SDL_Gamepad *pad, bool focused) {
    vibration_update(pad, focused);
    vibration_flush();
}
const char *SDL_GetError(void) { return "test"; }

// Unrelated HID / settings dependencies. Keep the returned vibration object callable.
static Service returned_service;
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    returned_service = (Service){.name = name, .handler = handler, .state = state};
    return &returned_service;
}
void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }
KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) {
    (void)size; (void)on_map; (void)handle; abort();
}
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { (void)w; (void)r; abort(); }
const char *language_text_code(void) { return "en-US"; }

static IpcResp command(uint32_t cmd, const void *data, uint32_t size) {
    IpcReq req = {.cmd = cmd, .data = data, .data_size = size};
    IpcResp resp = {0};
    hid(NULL, &req, &resp);
    return resp;
}

int main(void) {
    const uint32_t left = 0x00000003, right = 0x00010003;
    SDL_Gamepad *pad = (SDL_Gamepad *)(uintptr_t)1;
    vibration_reset();
    IpcResp resp = command(205, NULL, 0);
    assert(resp.data_size == 1 && resp.data[0] == 1);
    resp = command(203, NULL, 0);
    assert(resp.nobjects == 1 && resp.objects[0]->handler == active_vibration_devices);
    IpcReq activate = {0}; IpcResp activated = {0};
    resp.objects[0]->handler(resp.objects[0], &activate, &activated);
    assert(activated.result == 0);
    resp = command(200, &right, sizeof right);
    assert(resp.data_size == 8 && resp.data[0] == 1 && resp.data[4] == 2);

    // Single-device command: the value starts at byte 4, not at the aligned user id at byte 24.
    uint8_t packet[32] = {0};
    VibrationValue value = {.25f, 160, .5f, 320};
    memcpy(packet, &left, 4); memcpy(packet + 4, &value, sizeof value);
    command(201, packet, sizeof packet);
    update_sync(pad, true);
    assert(motor_low == 16384 && motor_high == 32768 && duration == 100);
    unsigned before = calls;
    now = 49; update_sync(pad, true); assert(calls == before);
    now = 50; update_sync(pad, true); assert(calls == before + 1);
    resp = command(202, packet, 16);
    assert(resp.data_size == sizeof value && memcmp(resp.data, &value, sizeof value) == 0);
    command(201, packet, 4);  // truncated: leave the current effect alone
    assert(vibration_get(left).low_amplitude == .25f);

    uint32_t handles[] = {left, right};
    VibrationValue batch[] = {{.75f, 100, .1f, 200}, {.2f, 180, .9f, 400}};
    IpcReq req = {.cmd = 206, .nx = 2, .x = {
        {(uintptr_t)handles, sizeof handles}, {(uintptr_t)batch, sizeof batch}}};
    resp = (IpcResp){0}; hid(NULL, &req, &resp);
    update_sync(pad, true);
    assert(motor_low == 49151 && motor_high == 58982);
    // Bound iteration by the shorter buffer; an incomplete trailing value must not be read.
    batch[0].low_amplitude = .6f; req.x[1].size = sizeof(VibrationValue) + 1;
    hid(NULL, &req, &resp);
    assert(vibration_get(left).low_amplitude == .6f && vibration_get(right).high_amplitude == .9f);

    // The player's toggle stops active motors without changing the game's HID permission.
    g_settings.rumble = false;
    update_sync(pad, true);
    assert(motor_low == 0 && motor_high == 0 && vibration_permitted());
    command(201, packet, sizeof packet);
    assert(vibration_get(left).low_amplitude == 0);
    vibration_reset();
    assert(!g_settings.rumble);  // returning to the title screen preserves the preference
    g_settings.rumble = true;
    update_sync(pad, true);
    assert(motor_low == 0 && motor_high == 0);  // no stale effect after re-enabling
    command(201, packet, sizeof packet);
    update_sync(pad, true);
    assert(motor_low == 16384 && motor_high == 32768);

    uint8_t allow = 0; command(204, &allow, 1);
    update_sync(pad, true); assert(motor_low == 0 && motor_high == 0);
    command(201, packet, sizeof packet);
    assert(vibration_get(left).low_amplitude == 0);
    allow = 1; command(204, &allow, 1);
    update_sync(pad, true); assert(motor_low == 0);  // do not resume an old effect

    value = (VibrationValue){NAN, 160, INFINITY, 320};
    vibration_send(left, &value); update_sync(pad, true);
    assert(motor_low == 0 && motor_high == 0);
    value = (VibrationValue){-1, 160, 2, 320};
    vibration_send(left, &value); update_sync(pad, true);
    assert(motor_low == 0 && motor_high == 65535);
    value = (VibrationValue){1, 160, 1, 320};
    vibration_send(0x00000103, &value);  // player 2: must not drive player 1
    assert(vibration_get(left).low_amplitude == 0);
    vibration_send(0, &value); assert(vibration_get(left).low_amplitude == 0);
    update_sync(pad, false); assert(motor_high == 0);
    update_sync(pad, true); assert(motor_high == 0);
    vibration_send(0x00002003, &value);  // handheld aliases the same physical controller
    update_sync(pad, true); assert(motor_low == 65535);
    update_sync(NULL, false);
    update_sync(pad, true); assert(vibration_get(left).low_amplitude == 0);
    vibration_send(left, &value); command(210, NULL, 0);
    update_sync(pad, true); assert(motor_low == 0);
    vibration_send(left, &value); vibration_reset();
    update_sync(pad, true); assert(motor_low == 0 && vibration_permitted());

    // Slow blocking SDL_RumbleGamepad (e.g. Bluetooth DualSense on Windows) must not block vibration_update,
    // and rapid updates while in flight must coalesce to the latest request.
    rumble_delay_ms = 40;
    unsigned before_slow = calls;
    double t0 = mono_ms();
    for (int step = 1; step <= 10; step++) {
        value = (VibrationValue){step * 0.1f, 160, 0, 320};
        vibration_send(left, &value);
        vibration_update(pad, true);
    }
    double elapsed = mono_ms() - t0;
    assert(elapsed < 20.0);
    vibration_flush();
    assert(motor_low == 65535 && calls - before_slow < 10);
    vibration_pad_closed(pad);
    rumble_delay_ms = 0;

    puts("vibration: HID packets, batching, motors, focus, disconnect, async rumble and reboot checks passed");
    return 0;
}
