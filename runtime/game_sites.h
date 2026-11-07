// Addresses in the game's main module that the runtime reads or writes. They differ between game versions:
// tools/game_sites.py finds them for the version the game code was generated from, and the game library carries them
// (runtime/gamelib.h, the generated layout.c). g_site holds them once the library is loaded (gamelib_load).
#pragma once
#include <stdint.h>

// name (as in tools/game_sites.py) and what is there
#define GAME_SITES(X)                                                                                                  \
    X(opt_table)               /* the Options screen's option table: u32 count, +8 entries, +0x10 rows, +0x18 order */ \
    X(opt_two_redraw)          /* optional all-or-nothing dual-row spike gate */                                      \
    X(opt_two_text)            /* native Options message-to-layout text setter */                                    \
    X(opt_two_help)            /* native row description update */                                                   \
    X(opt_two_pane_calc)       /* verified local transform/dirty-bit contract */                                      \
    X(screen_mgr)              /* ScreenMgr *: +0x28 screen count, +0x30 screen pointers by id */                      \
    X(beast_focus_axes)                                                                                                \
    X(beast_focus_linear)                                                                                              \
    X(beast_confirm_axes)                                                                                              \
    X(beast_confirm_linear)                                                                                            \
    X(beast_clamp_pitch)                                                                                               \
    X(beast_wrap_yaw)                                                                                                  \
    X(beast_world)                                                                                                     \
    X(opt_screen_rows)         /* field: the Options screen's row count, +8 its rows (1.8.2 +0x3698, 1.9.0 +0x36b0) */ \
    X(title_options_state)     /* field: the title menu's active button screen (0x6a: Options; 1.8.2 +0x363c) */       \
    X(dialog_answer)           /* the two-button dialog's answer (u32) */                                              \
    X(pause_state)             /* u32: 0 playing, 2 in the pause menu or the map */                                    \
    X(event_mgr)               /* ksys::evt::Manager *: its active event at +0x1d2b8 */                                \
    X(controller_step)         /* float: the controllers' hold counter step (1) */                                     \
    X(frame_loop_interval_ret) /* the frame loop's return address from setting the present interval */                \
    X(title_version_format)    /* "Ver.%s": the title screen's version line */                                         \
    X(player_info)             /* uking::act::PlayerInfo * (test tools) */                                              \
    X(world_mgr)               /* ksys::world::Manager * (test tools, weather.c)*/                                                \
    X(main_data)               /* main's .data (then .bss, up to main_end) */                                           \
    X(main_end)

// Optional helpers are zero when their complete feature group was not found.
#define GAME_SITES_OPTIONAL(X) \
    X(pouch_lock)             /* native recursive inventory mutex */ \
    X(pouch_unlock)           \
    X(pouch_page_count)       /* entries on an inventory page */ \
    X(pouch_page_item)        /* PouchItem * at a page and slot */ \
    X(pouch_item_type)        /* equipment category from an actor name */ \
    X(pouch_sort)             /* native category sort */ \
    X(quick_sort_event)       /* native event restriction */ \
    X(quick_sort_player_busy) /* native player-state restriction */ \
    X(quick_sort_modal)       /* another modal screen is open */ \
    X(quick_sort_refill)      /* rebuild the quick selector's item list */ \
    X(quick_sort_mapping)     /* rebuild visible slot mapping */ \
    X(quick_sort_cursor)      /* move the native cursor */ \
    X(quick_sort_labels)      /* update selected item labels */

typedef struct {
#define GAME_SITE_FIELD(n) uint64_t n;
    GAME_SITES(GAME_SITE_FIELD)
    GAME_SITES_OPTIONAL(GAME_SITE_FIELD)
#undef GAME_SITE_FIELD
} GameSites;

#ifdef __cplusplus
extern "C" {
#endif

extern GameSites g_site;
// The game version the loaded game code is for ("1.8.2"; "" before gamelib_load).
extern const char *g_game_version;

#ifdef __cplusplus
}
#endif
