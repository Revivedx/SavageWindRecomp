// Options > Accessibility > Weather (settings.h weather): holds the weather the player picks, or keeps out rain or
// thunderstorms. No patch in the game's code: once a frame, ksys::world::Manager's current weather is replaced before
// the game reads it, the way the game's own setWeatherType does (mWeatherType at +0x7d1, its timer at +0x794). A
// cutscene's own weather (mWeatherSetForDemo at +0x7d6) is left alone, so story scenes keep theirs.
#include <stdint.h>

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

RT_ON_RESET(weather_reset) { logged = 0; }
