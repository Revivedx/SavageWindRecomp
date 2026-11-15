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

// What a player's Link is doing, from his game (runtime/online_actors.c, 20 times a second): the AS his Link plays
// (slot 0 bank 0, the body; slot 1 banks 0 and 1, the upper body; slot 2, drawing a weapon), where each piece of his
// gear is (on which of Link's bones, and how) and his heading
#define ONLINE_AS_CHANNELS 4
#define ONLINE_AS_MAX 32
#define ONLINE_GEAR 6  // weapon, shield, bow, sheath, quiver, paraglider (while open)
#define ONLINE_GEAR_WORLD 0xff  // OnlinePose.gear_bone: on no bone, gear_rel is its world matrix (older games: hidden)
// What a player has thrown or shot, from his game: the bombs and arrows he holds or that fly (each as it is in the
// world, by an id of his), and the bombs that went off lately (each by a number that only grows, sent for a while)
#define ONLINE_SHOTS 8
#define ONLINE_BOOMS 4
enum { ONLINE_K_REMOTE_BOMB, ONLINE_K_REMOTE_BOMB_CUBE, ONLINE_K_REMOTE_BOMB2, ONLINE_K_REMOTE_BOMB_CUBE2,
       ONLINE_K_NORMAL_ARROW, ONLINE_K_FIRE_ARROW, ONLINE_K_ICE_ARROW, ONLINE_K_ELECTRIC_ARROW, ONLINE_K_BOMB_ARROW,
       ONLINE_K_ANCIENT_ARROW, ONLINE_K_BRIGHT_ARROW, ONLINE_K_BEAM, ONLINE_K_COUNT };  // (runtime/online_actors.c k_shot_names)
#pragma pack(push, 1)
typedef struct {
    uint8_t n;
    struct { uint16_t id; uint8_t kind; float m[12]; } shot[ONLINE_SHOTS];  // (m: Matrix34f, row-major)
    struct { uint16_t seq; uint8_t kind; float pos[3]; } boom[ONLINE_BOOMS];  // (seq 0: none)
} OnlineShots;
#pragma pack(pop)

// What a player holds with the Magnesis rune: that actor's name ("": nothing), where it was when he caught it (the
// same object in the others' worlds is the one of that name nearest there), and where it is now
#define ONLINE_GRAB_NAME 40
#define ONLINE_BOSSES 2
#define ONLINE_FX_MAX 24  // (OnlinePose.fx: an effect's name)
#define ONLINE_FX_EVENTS 8  // (OnlinePose.fxev: his last effects and sounds)
#define ONLINE_FX_PROPS 24  // (OnlinePose.fx_props: his Link's ELink property values)
#pragma pack(push, 1)
typedef struct { uint16_t seq; uint8_t who; char name[21]; } OnlineFxEvent;  // (who: 0 his Link, 1 his weapon;
// 2 his Link's sound, 3 his weapon's)
#pragma pack(pop)
#pragma pack(push, 1)
typedef struct { char name[ONLINE_GRAB_NAME]; float from[3]; float m[12]; } OnlineGrab;
#pragma pack(pop)

typedef struct {
    char as[ONLINE_AS_CHANNELS][ONLINE_AS_MAX];  // "" none
    uint8_t gear_bone[ONLINE_GEAR];              // 0: not shown; ONLINE_GEAR_WORLD: gear_rel is its matrix in the
                                                 // world (thrown, dropped); else the bone + 1 (online_actors.c k_bones)
    float gear_rel[ONLINE_GEAR][12];             // the piece's matrix in that bone's space (Matrix34f, row-major)
    float yaw;                                   // heading, radians (0: +z), as his Link faces
    uint8_t valid;
    OnlineShots shots;
    uint8_t effects;                             // ONLINE_E_*: what shows on him
    OnlineGrab grab;                             // what he holds with the Magnesis rune
    OnlineGrab stop;                             // what he stopped with the Stasis rune (as grab)
    uint8_t ice_n;                               // his Cryonis pillars (up to 3): where each one is
    float ice[3][12];
    struct { uint32_t hash; char name[28]; int32_t life; } boss[ONLINE_BOSSES];  // the bosses near him (Ganon, the
                                                 // Blights): each one's map HashId (0 none), its life ("" none)
    char map[32];                                // the map he is on ("<type>/<name>": MainField/, CDungeon/Dungeon000,
                                                 // MainFieldDungeon/RemainsWind, ...; "" unknown)
    char fx[ONLINE_FX_MAX];                      // his weapon's swing trail now, its ELink's ("Blur_Default"; "" none)
    int16_t fx_anim[3];                          // while it is on: his model's animations in its slots 0-2 (each one's
    float fx_frame[3];                           // index in its table, -1 none) and their frames: the trail SwordBlur
                                                 // draws is the one of the attack it finds there (runtime/online_actors.c)
    OnlineFxEvent fxev[ONLINE_FX_EVENTS];        // the last effects his Link and his weapon played (a charge's level,
                                                 // a spin attack, a heavy blow...), each by its number (0: none)
    uint32_t fx_props[ONLINE_FX_PROPS];          // his Link's ELink property values (its effects choose what to play by
                                                 // them: a charge's level, his weapon's kind...)
    uint8_t weather;                             // the weather where he is (the game's type, 0 clear .. 8; 255 unknown)
    int8_t dye[3];                               // his armour's dye colours (head, upper, lower; -1 none)
    uint16_t pvp_seq;                            // his last blow on another player (by its number, 0 none): who it hit
    uint8_t pvp_target;                          // (his slot) and how hard (his weapon's attack power); taken where the
    int16_t pvp_damage;                          // one hit has damage between players on (OnlineSettings.pvp)
    char horse[24];                              // the horse he rides (its actor, "" none), where it is, its AS
    float horse_m[12];
    char horse_as[32];
    char tack[2][24];                            // its saddle and reins (their actors, "" none), where they are
    float tack_m[2][12];
    uint32_t ground_props[16];                   // his ground's (ReactionField's) ELink property values at its last
                                                 // reaction to him (his fxev who 4: the ground's kind, his speed...)
} OnlinePose;
// OnlinePose.effects: Daruk's Protection's shield taking a hit (KAGO), its ring while he guards with its charges left
// (KAGO_COUNT: 1 to 3, in effects >> KAGO_COUNT_SHIFT)
enum { ONLINE_E_KAGO = 1, ONLINE_E_KAGO_COUNT_SHIFT = 1, ONLINE_E_KAGO_COUNT = 3 << 1,
       ONLINE_E_GALE = 0x08,        // Revali's Gale charging ("Kago_Wind" on him)
       ONLINE_E_DEMO = 0x80 };  // (DEMO: he is at a Sheikah pedestal: the slate's, a tower's)

typedef struct {
    bool used;
    char name[ONLINE_NAME_MAX];
    float pos[3];       // Link's feet, in the world (interpolated: a tenth of a second behind)
    float yaw;          // heading, radians (0: +z), from where he has been moving
    uint8_t flags;      // ONLINE_F_*
    uint8_t anim;       // ONLINE_A_*
    char armor[3][48];  // what he wears: head, upper body, lower body (the game's actor names, "" none)
    char gear[ONLINE_GEAR][48];  // what he carries: weapon, shield, bow, sheath, quiver (actor names, "" none)
    OnlinePose pose;    // (pose.valid: from his game; else only anim)
    uint32_t ping_ms;   // (the local player's own slot: the round trip to the server)
} OnlinePlayer;

typedef struct {
    char name[ONLINE_NAME_MAX];
    char host[64];      // the last address joined
    int port;           // the port to host on and to join
    char password[ONLINE_NAME_MAX];  // "" = none
    bool show_names;    // names over the other players
    bool show_markers;  // the others' figures in the world (off: the list only)
    bool pvp;           // damage between players: this player's blows hurt the others who have it on, theirs him
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

// The player in that slot is an actor in the world (runtime/online_actors.c): the overlay names him, draws no figure
bool online_has_actor(int slot);
// The local Link's pose, from the frame loop (runtime/online_actors.c): sent with his state
void online_set_local_pose(const OnlinePose *p);
// What the local Link wears (head, upper, lower) and carries (weapon, shield, bow, sheath, quiver): actor names, ""
// none; from the frame loop, sent when it changes and every 5 s
void online_set_local_equip(const char armor[3][48], const char gear[ONLINE_GEAR][48]);
// The local Link's skeleton (each bone's local rotation, a quaternion x y z w, and translation; from the frame loop),
// n of his bones and then nbow of his bow's (its string): sent 20 times a second; a remote player's, as it was 100 ms
// ago (interpolated), false when none came lately
#define ONLINE_BONES 160      // in all
#define ONLINE_BOW_BONES 32   // of them, at most, the bow's
void online_set_local_skeleton(int n, int nbow, const float (*q)[4], const float (*t)[3]);
bool online_skeleton(int slot, int *n, int *nbow, float (*q)[4], float (*t)[3]);
// the host's hour of the day (0..24), once each time it comes; false: none new (the host's own game never has one)
bool online_host_hour(float *hour);

// The host's enemies (runtime/online_actors.c): those near any player in his game, each by its id there, as it is
// (its actor's name, its matrix, the AS it plays); the others show them instead of their own around his Link
#define ONLINE_ENEMIES 32
#pragma pack(push, 1)
typedef struct {
    uint32_t id; char name[40]; float m[12]; char as[32]; char weapon[40]; float wm[12]; int32_t life;
    char shield[40]; float sm[12];  // (its shield, as its weapon)
    uint8_t wbone, sbone;  // (where wm / sm are: 0 the world; 1 its Weapon_R bone's space, 2 its Weapon_L's)
    uint8_t owner;  // (the slot of the player whose game has it for real: the others show it; -1 life: gone there)
    uint32_t hash;  // (its map object's HashId, 0 none: once it died, its dead flag in every game)
} OnlineEnemy;  // (weapon: what he holds, "" none)
#pragma pack(pop)
void online_set_local_enemies(const float centre[3], const OnlineEnemy *e, int n);  // (the host's frame loop)
// a hit on one of the host's enemies here (the others: its id, the life it took), for his game; his game takes them
// (element: ONLINE_ELEM_*, the element arrow of the local player's that made it: its owner's game gives the enemy the
// element's state, burning, frozen or shocked, which a puppet, without a chemical, doesn't take itself)
enum { ONLINE_ELEM_NONE, ONLINE_ELEM_FIRE, ONLINE_ELEM_ICE, ONLINE_ELEM_ELECTRIC, ONLINE_ELEM_COUNT };
void online_send_hit(uint32_t id, int32_t damage, uint8_t element);
int online_take_hits(uint32_t *id, int32_t *damage, uint8_t *element, int max);
// the enemies this game has for real for the others (ESTATE), to the host's; his game takes them, each with the
// slot it came from
// What enemies dropped (loot), everyone's: the host's game keeps the list (each one by an id of its own, the game it
// came from and its id there), the others make each one where it is; one picked up anywhere goes for everyone
#define ONLINE_LOOT 64
#pragma pack(push, 1)
typedef struct { uint32_t gid, lid; uint8_t origin; char name[40]; float pos[3]; } OnlineLoot;
#pragma pack(pop)
void online_send_loot_add(const OnlineLoot *l);           // (a game not the host's: a drop of its, for the list)
int online_take_loot_adds(OnlineLoot *out, int max);       // (the host's: those come, origin set)
void online_send_loot_take(uint32_t gid);                  // (a game not the host's: it was picked up there)
int online_take_loot_takes(uint32_t *gid, int max);        // (the host's: those come)
void online_set_loots(const OnlineLoot *l, int n);         // (the host's: the list, for the others)
int online_loots(OnlineLoot *out);                         // (the others: the list as it came last; -1 none yet)
// The quests' progress (main, shrine and side quests), everyone's: a bit per flag of online_mod/quests.txt; each game sends its own, the host's
// game keeps the union (what any of them did) and sends it, and each sets what it lacks
#define ONLINE_QUEST_BYTES 4096  // (32768 flags)
void online_send_quests(const uint8_t m[ONLINE_QUEST_BYTES]);   // (a game not the host's)
bool online_take_quests(uint8_t m[ONLINE_QUEST_BYTES]);         // (the host's game: the others', ORed in)
void online_set_quests(const uint8_t m[ONLINE_QUEST_BYTES]);    // (the host's game: the union, for the others)
bool online_quests(uint8_t m[ONLINE_QUEST_BYTES]);              // (the others: the union as it came last)
// A weapon or shield picked up off the ground (its name, where it was): it goes in the others' games too
void online_send_pick(const char *name, const float pos[3]);
int online_take_picks(char (*name)[40], float (*pos)[3], int max);
void online_send_estates(const OnlineEnemy *e, int n);
int online_take_estates(OnlineEnemy *e, uint8_t *from, int max);
// the host's, as they were 0.1 s ago (between his last two); false: none ever came. *age_ns: how long since the last
int online_enemies(float centre[3], OnlineEnemy *out, uint64_t *age_ns);
void online_set_local_hour(float hour);  // (the frame loop: the local game's hour, -1 none)

// A dedicated server (main.c --server): runs until the process ends
int online_server_main(int port, const char *password);

#ifdef __cplusplus
}
#endif
