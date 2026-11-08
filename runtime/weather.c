// Cheats > Weather in the Savage Wind overlay, and `weather` (settings.h weather): holds the weather the player picks,
// or keeps out rain or thunderstorms. No patch in the game's code: once a frame, ksys::world::Manager's current weather
// is replaced before the game reads it, the way the game's own setWeatherType does (mWeatherType at +0x7d1, its timer
// at +0x794). A cutscene's own weather (mWeatherSetForDemo at +0x7d6) is left alone, so story scenes keep theirs.
#include <stdint.h>
#include <string.h>

#include "control.h"
#include "game_sites.h"
#include "hle.h"
#include "settings.h"

// The game's weather types: 0 clear, 1 cloudy, 2 rain, 3 heavy rain, 4 snow, 5 heavy snow, 6 thunderstorm,
// 7 thunder rain, 8 rain in sunshine. The setting: 0 the game's, 1 no rain, 2 no thunderstorms, 3 + n: n held.
static int logged;  // (changes written to the log this boot: the first few)

void weather_frame(void) {
    int w = g_settings.weather;
    if (w <= 0 || w >= WEATHER_COUNT || !g_site.world_mgr) return;
    uint64_t wm = *(uint64_t *)(uintptr_t)g_site.world_mgr;
    if (!wm || !*(uint32_t *)(uintptr_t)(wm + 0x5c0)) return;  // (the world isn't up yet)
    if (*(volatile uint8_t *)(uintptr_t)(wm + 0x7d6)) return;   // (a cutscene's weather)
    volatile uint8_t *now = (volatile uint8_t *)(uintptr_t)(wm + 0x7d1);
    uint8_t cur = *now;
    int want = w >= 3 ? w - 3 : -1;
    if (w == 1 && (cur == 2 || cur == 3 || cur == 6 || cur == 7 || cur == 8)) want = 1;  // (rain of any kind: cloudy)
    if (w == 2 && cur == 6) want = 1;                                                    // (a dry storm: cloudy)
    if (w == 2 && cur == 7) want = 2;                                                    // (thunder rain: rain)
    if (want < 0) return;
    if (cur != want && logged < 20 && (cur != 255 || !logged)) {  // (255: none requested; held: rewritten each frame)
        LOG("[weather] %d -> %d (setting %d)\n", cur, want, w);
        logged++;
    }
    *now = (uint8_t)want;
    *(volatile int32_t *)(uintptr_t)(wm + 0x794) = 4;  // (as setWeatherType leaves it: the change is taken up)
}

// weather: show | <name>: game (the game's own again), no_rain, no_storms, or one weather held (settings.h names)
static void cmd_weather(int argc, char **argv, ControlReply *r) {
    static const char *const usage = "weather: use weather game, no_rain, no_storms, clear, cloudy, rain, heavy_rain, "
                                     "snow, heavy_snow, thunderstorm, thunder_rain or sun_shower";
    if (argc > 2) { control_fail(r, "%s", usage); return; }
    if (argc == 2) {
        int w = -1;
        for (int i = 0; i < WEATHER_COUNT; i++)
            if (!strcmp(argv[1], g_weather_keys[i])) w = i;
        if (w < 0) { control_fail(r, "%s", usage); return; }
        g_settings.weather = w;  // (weather_frame writes it from the next frame; as `set`, the ini isn't written)
        logged = 0;
    }
    int w = g_settings.weather >= 0 && g_settings.weather < WEATHER_COUNT ? g_settings.weather : 0;
    control_field_str(r, "weather", g_weather_keys[w]);
    control_text(r, "Weather: %s", w == 0 ? "the game's own" : g_weather_keys[w]);
    uint64_t wm = g_site.world_mgr ? *(uint64_t *)(uintptr_t)g_site.world_mgr : 0;
    if (wm && *(uint32_t *)(uintptr_t)(wm + 0x5c0)) {
        uint8_t cur = *(volatile uint8_t *)(uintptr_t)(wm + 0x7d1);
        if (cur < 9) {  // (the game's types, as g_weather_keys from 3 on)
            control_field_str(r, "now", g_weather_keys[3 + cur]);
            control_text(r, "Now: %s", g_weather_keys[3 + cur]);
        }
        if (*(volatile uint8_t *)(uintptr_t)(wm + 0x7d6)) control_text(r, "A cutscene sets the weather now: it keeps it");
    } else if (argc == 2) control_text(r, "No world yet: it applies once one is loaded");
}

// (runtime/control_game.c: developer mode only - it writes the game's memory)
void weather_control_init(void) {
    static const ControlCmd cmds[] = {
        {"weather", "[game | no_rain | no_storms | clear | cloudy | rain | heavy_rain | snow | heavy_snow | thunderstorm | "
                    "thunder_rain | sun_shower]",
         "Cheat: show, hold or limit the weather (game: the game's own again)", cmd_weather, 0},
    };
    control_register(cmds, 1);
}

RT_ON_RESET(weather_reset) { logged = 0; }
