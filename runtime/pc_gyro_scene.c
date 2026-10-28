// BOTW 1.9.0: DungeonRotateGyro owns the mechanism, and the current gyro
// interaction supplies its input. Guest memory is read only on the game thread.
// Mouse rotation uses the click point, never projected model bounds.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "pc_gyro_scene.h"
#include "motion.h"
#include "game_sites.h"
#include "hle.h"

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static GyroScene scene;
static _Atomic bool active;
static bool interaction;
static uint64_t last_actor_ns;
static bool paused_before;
static bool enabled(void) {
    const char *v=getenv("SAVAGEWIND_GYRO_SCENE");
    return v && atoi(v) && g_game_version && !strcmp(g_game_version,"1.9.0");
}
static bool ptr(uint64_t p){return p>=GUEST_LO && p<GUEST_HI-0x1000;}
// Native apparatus activation owns input; no model projection is involved.
bool pc_gyro_scene_active(void){return active;}
bool pc_gyro_scene_view(GyroScene *out) {
    pthread_mutex_lock(&lock);*out=scene;pthread_mutex_unlock(&lock);
    return out->active;
}
void pc_gyro_scene_tick(void) {
    uint64_t now=plat_now_ns();
    bool paused=g_site.pause_state && LD32(g_site.pause_state);
    if(interaction && paused_before && !paused)last_actor_ns=now;
    paused_before=paused;
    bool recent=last_actor_ns && now>=last_actor_ns && now-last_actor_ns<150000000ull;
    bool on=enabled() && g_site.main_end>0x7001e14d20ull && ptr(LD64(0x7001e14d18ull)) && (recent || (interaction && paused));
    bool show=on && !paused;
    if(interaction && !on)motion_virtual_end();
    if(on!=interaction) {
        pthread_mutex_lock(&lock);
        scene.generation++;
        pthread_mutex_unlock(&lock);
        LOG("[gyro-scene] interaction %s\n",on?"enter":"leave");
    }
    interaction=on;active=show;
    pthread_mutex_lock(&lock);scene.active=show;pthread_mutex_unlock(&lock);
}
void rt_gyro_scene_actor(Cpu *c,uint64_t actor,uint64_t control) {
    (void)c;
    if(enabled() && ptr(actor) && ptr(control))last_actor_ns=plat_now_ns();
}
RT_ON_RESET(gyro_scene_reset) {
    active=interaction=false;last_actor_ns=0;paused_before=false;
    pthread_mutex_lock(&lock);memset(&scene,0,sizeof scene);pthread_mutex_unlock(&lock);
}
