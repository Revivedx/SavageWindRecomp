// The shrine apparatus (BOTW 1.9.0): whether the player is controlling one of the motion-controlled mechanisms
// (DungeonRotateGyro). The game code calls rt_gyro_scene_actor each frame the mechanism reads its input (the
// gyro_scene_actor site, tools/patches.py); pc_gyro_scene_tick turns that heartbeat into "in the apparatus" for the
// virtual gyro: its keys (runtime/hle/input.c), the drag (runtime/ui/gyro_drag.h) and a gyroscope-less controller's
// sticks. Guest memory is read on the game thread only. The drag uses the click point, not the model's place on screen.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "game_sites.h"
#include "hle.h"
#include "motion.h"
#include "pc_gyro_scene.h"
#include "settings.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static GyroScene scene;           // (the render thread's copy, under lock)
static _Atomic bool active;       // in the apparatus with the mouse drag on (Controls: Shrine mouse drag)
static _Atomic bool apparatus;    // in the apparatus, not paused
static bool interaction;          // in the apparatus, paused or not (game thread)
static uint64_t last_actor_ns;    // the mechanism's last heartbeat (game thread)
static bool paused_before;

static bool enabled(void) { return g_game_version && !strcmp(g_game_version, "1.9.0"); }
static bool ptr(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI - 0x1000; }

bool pc_gyro_scene_active(void) { return active; }
bool pc_gyro_keyboard_active(void) { return apparatus; }
bool pc_gyro_scene_view(GyroScene *out) {
    pthread_mutex_lock(&lock);
    *out = scene;
    pthread_mutex_unlock(&lock);
    return out->active;
}

// Game thread, once a frame (runtime/hle/input.c input_cam_frame). The heartbeat stops while the game is paused: the
// interaction holds through the pause and its clock starts again after it. 0x7001e14d18 (1.9.0) holds the mechanism
// manager while a shrine with one is loaded.
void pc_gyro_scene_tick(void) {
    uint64_t now = plat_now_ns();
    bool paused = g_site.pause_state && LD32(g_site.pause_state);
    if (interaction && paused_before && !paused) last_actor_ns = now;
    paused_before = paused;
    bool recent = last_actor_ns && now >= last_actor_ns && now - last_actor_ns < 150000000ull;
    bool on = enabled() && g_site.main_end > 0x7001e14d20ull && ptr(LD64(0x7001e14d18ull)) &&
              (recent || (interaction && paused));
    bool show = on && !paused, drag = show && g_settings.gyro_scene;
    if (interaction && !on) motion_virtual_end();
    if (on != interaction || active != drag) {
        pthread_mutex_lock(&lock);
        scene.generation++;  // (a drag in progress ends)
        pthread_mutex_unlock(&lock);
    }
    if (on != interaction) LOG("[gyro-scene] interaction %s\n", on ? "enter" : "leave");
    interaction = on;
    apparatus = show;
    active = drag;
    pthread_mutex_lock(&lock);
    scene.active = drag;
    pthread_mutex_unlock(&lock);
}

// The gyro_scene_actor site: the mechanism (actor) asked for its input (control) this frame
void rt_gyro_scene_actor(Cpu *c, uint64_t actor, uint64_t control) {
    (void)c;
    if (enabled() && ptr(actor) && ptr(control)) last_actor_ns = plat_now_ns();
}

RT_ON_RESET(gyro_scene_reset) {
    active = apparatus = interaction = false;
    last_actor_ns = 0;
    paused_before = false;
    pthread_mutex_lock(&lock);
    memset(&scene, 0, sizeof scene);
    pthread_mutex_unlock(&lock);
}
