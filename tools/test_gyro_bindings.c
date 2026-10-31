// Exercise production keyboard mapping without a window, controller or game assets.
#include <assert.h>
#include <stdio.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/input.c"
Settings g_settings;
static int pitch,roll,yaw;static bool center,captured,paused,apparatus;
void motion_keyboard(int p,int r,int y,bool c){pitch=p;roll=r;yaw=y;center=c;}
void motion_tilt_held(bool h){(void)h;}
bool pc_gyro_keyboard_active(void){return apparatus;}
void pc_gyro_scene_tick(void){}
uint64_t plat_now_ns(void){return 1000000000;}
Cpu *kernel_current_cpu(void){abort();}
void vibration_update(SDL_Gamepad *p,bool f){(void)p;(void)f;abort();}
bool menu_cursor_free(void){return false;}
void motion_pad_opened(SDL_Gamepad*p){(void)p;abort();}
void motion_pad_closed(void){abort();}
void motion_sensor_event(const SDL_Event*e){(void)e;abort();}
void motion_mouse(float x,float y){(void)x;(void)y;abort();}
void motion_mouse_yaw(float y){(void)y;abort();}
void motion_script(float a,float b,float c,float d){(void)a;(void)b;(void)c;(void)d;abort();}
void watch_arm(void){abort();}
bool menu_input_captured(void){return captured;}
bool framepace_menu_ui(void){return paused;}
bool framepace_title_ui(void){return false;}
// (the control endpoint and the DSU poll: off here; no controller, so no dual-stick)
bool control_enabled(void){return false;}
bool control_fly_toggle(void){abort();}
void control_fly_wheel(float n){(void)n;abort();}
void control_input_frame(uint64_t f){(void)f;}
void control_input_apply(uint64_t *b,int32_t *lx,int32_t *ly,int32_t *rx,int32_t *ry){(void)b;(void)lx;(void)ly;(void)rx;(void)ry;}
bool motion_has_sensors(void){return false;}
void motion_dualstick(float lx,float ly,float rx,float ry,double dt,bool r){(void)lx;(void)ly;(void)rx;(void)ry;(void)dt;(void)r;abort();}
void motion_poll(void){}
uint64_t renderer_present_count(void){return 0;}
static void clear(void){memset(keys_down,0,sizeof keys_down);memset(mouse_down,0,sizeof mouse_down);}
int main(void){
    bind_reset(BIND_DEV_KEY);win_focused=true;
    keys_down[SDL_SCANCODE_I]=1;recompute_keyboard();assert(key_ry==32767&&pitch==0);
    g_settings.pc_gyro=true;recompute_keyboard();assert(pitch==0&&key_ry==32767);
    apparatus=true;recompute_keyboard();assert(pitch==-1&&key_ry==0);  // (up: negative pitch)
    apparatus=false;recompute_keyboard();assert(pitch==0&&key_ry==32767);
    apparatus=true;
    clear();keys_down[SDL_SCANCODE_K]=1;recompute_keyboard();assert(pitch==1);
    clear();keys_down[SDL_SCANCODE_U]=1;recompute_keyboard();assert(yaw==-1);
    clear();keys_down[SDL_SCANCODE_O]=1;recompute_keyboard();assert(yaw==1&&!center);
    clear();keys_down[SDL_SCANCODE_P]=1;recompute_keyboard();assert(center&&yaw==0);
    // Real saved bindings, including mouse buttons, replace rather than supplement the defaults.
    assert(bind_settings_load("bind_key_gyro_up","H"));
    clear();keys_down[SDL_SCANCODE_I]=1;recompute_keyboard();assert(pitch==0&&key_ry==32767);
    clear();keys_down[SDL_SCANCODE_H]=1;recompute_keyboard();assert(pitch==-1);
    g_settings.pc_gyro=false;recompute_keyboard();assert(pitch==0&&!center);
    g_settings.pc_gyro=true;captured=true;recompute_keyboard();assert(!pitch);
    captured=false;paused=true;recompute_keyboard();assert(!pitch);
    paused=false;win_focused=false;recompute_keyboard();assert(!pitch);
    win_focused=true;
    bind_settings_load("bind_key_gyro_up","Mouse Back");clear();mouse_down[SDL_BUTTON_X1]=1;
    recompute_keyboard();assert(pitch==-1);
    puts("PASS: gyro defaults, pitch direction, U/O/P, remapping, camera ownership, disabling, pause/capture/focus and mouse bindings");
}
