#include <assert.h>
#include <stdio.h>
#include "../runtime/pc_gyro_scene.c"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
Settings g_settings={.gyro_scene=true};
GameSites g_site;
const char *g_game_version="1.9.0";
static uint64_t clock_ns=1000000000;
static int ended;
uint64_t plat_now_ns(void){return clock_ns;}
void motion_virtual_end(void){ended++;}
int main(void) {
    void *at=(void*)0x7001e10000ull;
#ifdef _WIN32
    void *mem=VirtualAlloc(at,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    _putenv_s("SAVAGEWIND_GYRO_SCENE","");
#else
    void *mem=mmap(at,0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    unsetenv("SAVAGEWIND_GYRO_SCENE");
#endif
    assert(mem==at);
    g_site.main_end=0x7001e98000ull;g_site.pause_state=0x7001e11000ull;
    uint64_t actor=0x7100000000ull;
    ST64(0x7001e14d18ull,actor);
    pc_gyro_scene_tick();assert(!pc_gyro_scene_active());
    rt_gyro_scene_actor(NULL,actor,actor);pc_gyro_scene_tick();assert(pc_gyro_scene_active());
    unsigned generation=scene.generation;
    ST32(g_site.pause_state,1);clock_ns+=1000000000;pc_gyro_scene_tick();
    assert(!pc_gyro_scene_active()&&interaction&&ended==0);
    ST32(g_site.pause_state,0);pc_gyro_scene_tick();
    assert(pc_gyro_scene_active()&&scene.generation!=generation);
    clock_ns+=200000000;pc_gyro_scene_tick();
    assert(!pc_gyro_scene_active()&&ended==1&&scene.generation!=generation);
    pc_gyro_scene_tick();assert(ended==1);
    rt_gyro_scene_actor(NULL,actor,actor);pc_gyro_scene_tick();assert(pc_gyro_scene_active());
    ST64(0x7001e14d18ull,0);pc_gyro_scene_tick();assert(!pc_gyro_scene_active()&&ended==2);
    ST64(0x7001e14d18ull,actor);rt_gyro_scene_actor(NULL,actor,actor);pc_gyro_scene_tick();assert(pc_gyro_scene_active());
    generation=scene.generation;
    g_settings.gyro_scene=false;pc_gyro_scene_tick();assert(!pc_gyro_scene_active()&&pc_gyro_keyboard_active()&&scene.generation!=generation);
    g_settings.gyro_scene=true;
    gyro_scene_reset();assert(!scene.active&&!interaction&&!last_actor_ns);
    g_game_version="1.8.2";ST64(0x7001e14d18ull,actor);
    rt_gyro_scene_actor(NULL,actor,actor);pc_gyro_scene_tick();assert(!pc_gyro_scene_active());
    puts("activation, pause/resume, timeout/exit/reset and unsupported version PASS");
}
