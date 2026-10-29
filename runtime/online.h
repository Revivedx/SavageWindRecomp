// Online play (runtime/online.c): up to ONLINE_MAX players in one world, over TCP. One player creates a session (the
// game hosts the server itself, and joins it as everyone else does), the others join it by address; `savagewind.exe
// --server [port]` runs only the server, without the game (a dedicated server). Each client sends its Link's state 20
// times a second (position, heading, what the game is doing) and gets everyone else's; the overlay
// (runtime/ui/online_ui.mm) draws the other players where they are, through the scene's camera (vulkan/exec.c).
// What the game reads: uking::act::PlayerInfo [player_info] +0x88 Link's position (framepace.c), the pause state.
// The settings (name, address, port, password) are in online.ini in the data root.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_MAX 8          // players in a session, the host included
#define ONLINE_PORT 52520     // the default port (TCP)
#define ONLINE_NAME_MAX 24    // a player's name, the terminating 0 included
#define ONLINE_CHAT_MAX 120   // a chat line

enum { ONLINE_OFF, ONLINE_CONNECTING, ONLINE_CONNECTED, ONLINE_FAILED };
enum { ONLINE_F_PAUSED = 1, ONLINE_F_LOADING = 2 };  // OnlinePlayer.flags
// What a player's Link is doing (OnlinePlayer.anim): the animation the others' copies of him play
// (runtime/online_actors.c k_anims, tools/online_mod.py ANIMS: the same order)
enum { ONLINE_A_WAIT, ONLINE_A_WALK, ONLINE_A_RUN, ONLINE_A_DASH, ONLINE_A_JUMP, ONLINE_A_FALL, ONLINE_A_GLIDE,
       ONLINE_A_SWIM_WAIT, ONLINE_A_SWIM, ONLINE_A_CLIMB_WAIT, ONLINE_A_CLIMB, ONLINE_A_SQUAT_WAIT, ONLINE_A_SQUAT,
       ONLINE_A_COUNT };

typedef struct {
    bool used;
    char name[ONLINE_NAME_MAX];
    float pos[3];       // Link's feet, in the world (interpolated: a tenth of a second behind)
    float yaw;          // heading, radians (0: +z), from where he has been moving
    uint8_t flags;      // ONLINE_F_*
    uint8_t anim;       // ONLINE_A_*
    uint32_t ping_ms;   // (the local player's own slot: the round trip to the server)
} OnlinePlayer;

typedef struct {
    char name[ONLINE_NAME_MAX];
    char host[64];      // the last address joined
    int port;           // the port to host on and to join
    char password[ONLINE_NAME_MAX];  // "" = none
    bool show_names;    // names over the other players
    bool show_markers;  // the others' figures in the world (off: the list only)
} OnlineSettings;

extern OnlineSettings g_online;

void online_settings_load(void);  // after rt_paths_init (online.ini, the working directory)
void online_settings_save(void);

// Starting and ending a session (any thread: the work is the network thread's)
bool online_host(void);           // a server on g_online.port, and join it
bool online_join(void);           // join g_online.host:g_online.port
void online_leave(void);          // leave (and stop the server when hosting)
int online_state(void);           // ONLINE_*
bool online_is_host(void);
// what is going on, for the menu: an English text (tr() it) with at most one %s, *arg what goes there
const char *online_status(char *arg, size_t cap);
int online_local_id(void);        // the local player's slot, -1 when not connected

// A copy of every slot (ONLINE_MAX); false = not connected
bool online_players(OnlinePlayer out[ONLINE_MAX]);

// Chat: send a line; the lines received are kept (newest last)
void online_chat_send(const char *text);
int online_chat_lines(char out[][ONLINE_CHAT_MAX + ONLINE_NAME_MAX + 4], int max, uint64_t *newest_ms);

// Join / leave notices for the overlay (newest last)
// (fmts: "%s joined" / "%s left", English: tr() them)
int online_notices(char names[][ONLINE_NAME_MAX], const char *fmts[], uint64_t ages_ms[], int max);

// This PC's own IPv4 addresses, comma separated (what others type to join it)
void online_local_addresses(char *out, size_t cap);

// The scene camera (vulkan/exec.c, each presented frame while online): view-projection (row-major, the game's world
// to clip space) and the image rows' direction (-1: row 0 at the top)
bool online_wants_camera(void);
void online_camera_set(const double vp[16], float ysign);
bool online_camera_get(double vp[16], float *ysign, uint64_t *age_ns);

// A dedicated server (main.c --server): runs until the process ends
int online_server_main(int port, const char *password);

#ifdef __cplusplus
}
#endif
