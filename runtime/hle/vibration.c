// The SDK mixes its vibration nodes itself, then sends HID commands 201 / 206. SDL's two motors cannot reproduce
// HD rumble frequencies: use the strongest low / high band across the two device values instead.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#include "../hle.h"
#include "../vibration.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static VibrationValue values[2] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
static bool permitted = true;

// These are the handles returned by hid_vibration_handles (input.c), for our one player or handheld mode.
static int device(uint32_t handle) {
    uint32_t id = (handle >> 8) & 0xff;
    return (handle & 0xfffe00ffu) == 3 && (id == 0 || id == 0x20) ? (int)((handle >> 16) & 1) : -1;
}

static float amplitude(float a) { return !isfinite(a) || a < 0 ? 0 : a > 1 ? 1 : a; }

void vibration_send(uint32_t handle, const VibrationValue *value) {
    int i = device(handle);
    if (i < 0) return;
    pthread_mutex_lock(&lock);
    if (permitted) {
        VibrationValue previous = values[i];
        values[i] = *value;
        values[i].low_amplitude = amplitude(value->low_amplitude);
        values[i].high_amplitude = amplitude(value->high_amplitude);
        if ((previous.low_amplitude != values[i].low_amplitude || previous.high_amplitude != values[i].high_amplitude) &&
            getenv("SAVAGEWIND_VIBRATION"))
            LOG("[vibration] handle %#x low %.3f high %.3f\n", handle, values[i].low_amplitude, values[i].high_amplitude);
    }
    pthread_mutex_unlock(&lock);
}

VibrationValue vibration_get(uint32_t handle) {
    VibrationValue v = {0, 160, 0, 320};
    int i = device(handle);
    pthread_mutex_lock(&lock);
    if (i >= 0 && permitted) v = values[i];
    pthread_mutex_unlock(&lock);
    return v;
}

void vibration_clear(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 2; i++) values[i] = (VibrationValue){0, 160, 0, 320};
    pthread_mutex_unlock(&lock);
}

void vibration_permit(bool allow) {
    pthread_mutex_lock(&lock);
    permitted = allow;
    if (!allow) memset(values, 0, sizeof values);
    pthread_mutex_unlock(&lock);
}

bool vibration_permitted(void) {
    pthread_mutex_lock(&lock);
    bool allow = permitted;
    pthread_mutex_unlock(&lock);
    return allow;
}

RT_ON_RESET(vibration_reset) {
    vibration_clear();
    vibration_permit(true);
}

void vibration_update(SDL_Gamepad *pad, bool focused) {
    static SDL_Gamepad *last_pad;
    static uint16_t last_low, last_high;
    static uint64_t last_ms;
    static bool failed;
    if (!pad || !focused) vibration_clear();
    bool changed = pad != last_pad;
    if (changed) {
        last_pad = pad;
        last_low = last_high = 0;
        last_ms = 0;
        failed = false;
    }
    if (!pad) return;
    pthread_mutex_lock(&lock);
    uint16_t low = focused && permitted ? (uint16_t)lroundf(fmaxf(values[0].low_amplitude, values[1].low_amplitude) * 65535) : 0;
    uint16_t high = focused && permitted ? (uint16_t)lroundf(fmaxf(values[0].high_amplitude, values[1].high_amplitude) * 65535) : 0;
    pthread_mutex_unlock(&lock);
    uint64_t now = SDL_GetTicks();
    // A short lease stops the motors if the event loop stalls; refresh unchanged effects while it is alive.
    if (!changed && low == last_low && high == last_high && (!(low || high) || now - last_ms < 50)) return;
    if (!SDL_RumbleGamepad(pad, low, high, 100) && !failed) {
        LOG("[input] controller rumble unavailable: %s\n", SDL_GetError());
        failed = true;
    }
    static int nlog;  // (SAVAGEWIND_MOTION_LOG: rumble going on and off, next to the sensor stream's health - a
                      // controller might change its sensor reports once it rumbles, #220)
    if (!(low || high) != !(last_low || last_high) && ENV("SAVAGEWIND_MOTION_LOG") && nlog < 200) {
        nlog++;
        LOG("[motion] rumble %s\n", low || high ? "on" : "off");
    }
    last_low = low;
    last_high = high;
    last_ms = now;
}
