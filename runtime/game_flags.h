// The game's own options (runtime/game_flags.c): read and set where the game keeps them, its memory while it runs and
// option.sav, for the F1 menu's Gameplay page
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
enum {
    GOPT_INVERT_Y,         // CameraUpDownReverse
    GOPT_INVERT_X,         // CameraRLReverse
    GOPT_JUMP_SWAP,        // JumpButtonChange: Jump on the B button, Cancel on X
    GOPT_MOTION_AIM,       // GyroOnOff: aim with the motion sensors
    GOPT_RUBY,             // TextRubyOnOff: furigana (Japanese text only)
    GOPT_MINIMAP_NORTH,    // MiniMapDirection: the minimap fixed to north
    GOPT_CAMERA_SPEED,     // StickSensitivity: 0 very slow .. 4 very fast
    GOPT_AMIIBO,           // AmiiboItemOnOff
    GOPT_HUD_PRO,          // MainScreenOnOff: HUD mode Pro
    GOPT_MESSAGE_OPAQUE,   // TextWindowChange: 1 regular, 2 opaque
    GOPT_COUNT
};
int game_opt_get(int id);       // the value: the game's memory, else option.sav, else the game's default
void game_opt_set(int id, int v);
int game_opt_default(int id);
void game_opt_defaults(void);   // every one back to the game's default
bool game_opt_live(int id);     // the game runs with it in memory (a change applies at once)
#ifdef __cplusplus
}
#endif
