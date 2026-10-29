// Host input state only: no game code, assets or window are required.
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/pc_cursor.c"

Settings g_settings = {.pc_cursor = true};
GameSites g_site;
static uint64_t now=1000000000,frame=1;
bool menu_cursor_free(void){return false;}
uint64_t plat_now_ns(void){return now;}
uint64_t renderer_present_count(void){return frame;}
bool framepace_menu_ui(void){return true;}
bool framepace_title_ui(void){return false;}
uint64_t framepace_script_frame(uint64_t value){return value;}
void input_pc_escape(bool down){(void)down;abort();}
void input_pc_map_axes(float out[2]){(void)out;abort();}
static unsigned actions,refreshes,last_action,scroll_updates,activations,help_updates;
static uint64_t activated_node;
static uint64_t navigation;
static Cpu test_cpu;
Cpu *kernel_current_cpu(void){return &test_cpu;}
uint64_t input_buttons_now(void){return navigation;}
int rt_opt_pointer_kind(uint64_t row){return (int)LD32(row+0x50);}
void guest_call(Cpu *cpu,uint64_t address){
    if(address==0x7000e30700ull){actions++;last_action=LD32(LD64(cpu->x[0])+0x36c0);}
    else if(address==0x7000e2eed8ull)refreshes++;
    else if(address==0x7000d671d4ull)scroll_updates++;
    else if(address==0x7000f6e2c8ull){assert(LD8(cpu->x[0]+0xfc)==1);cpu->x[0]=0;}
    else if(address==0x7000f57da8ull){
        activations++;activated_node=cpu->x[1];ST64(cpu->x[0]+0x48,cpu->x[1]);
        uint64_t owner=LD64(cpu->x[1]+0x20);ST64(owner+0xe0,cpu->x[1]);
        *(float*)(owner+0x3628+0x30)=90;cpu->x[19]=0xdead;
    }else if(address==0x7000e3047cull){
        help_updates++;ST64(cpu->x[0]+0x36c8,LD64(LD64(cpu->x[0]+0x36b8)));
    }else abort();
}

int main(void) {
    void *memory;
#ifdef _WIN32
    memory=VirtualAlloc((void*)GUEST_LO,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    memory=mmap((void*)GUEST_LO,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
#endif
    assert(memory==(void*)GUEST_LO);
    uint64_t screen=GUEST_LO+0x100,rows=GUEST_LO+0x500,row=GUEST_LO+0x600;
    uint64_t table=GUEST_LO+0x800,entries=GUEST_LO+0x900,button=GUEST_LO+0xa00;
    g_site.opt_screen_rows=0x36b0;g_site.opt_table=table;
    ST8(screen+0xfe,2);ST32(screen+0x36b0,1);ST64(screen+0x36b8,rows);ST64(rows,row);
    ST32(table,1);ST64(table+8,entries);ST32(row+0x50,0);
    active=true;focused=true;sampled_frame=frame;

    uint64_t view=screen+0x3628;
    *(float*)(view+0x28)=450;*(float*)(view+0x40)=-170;*(float*)(view+0x48)=190;
    ST64(screen+0x36c8,123);point[1]=300;
    wheel=-1;pc_cursor_options_tick(screen);
    assert(*(float*)(view+0x2c)==50&&*(float*)(view+0x30)==50);
    assert(pc_cursor_buttons(0)==0&&LD64(screen+0x36c8)==123&&scroll_updates==1);
    wheel=-3;pc_cursor_options_tick(screen);assert(*(float*)(view+0x30)==90);
    wheel=10;pc_cursor_options_tick(screen);assert(*(float*)(view+0x30)==0);
    navigation=1ull<<15;pc_cursor_options_tick(screen);assert(!options_scroll_valid);navigation=0;

    // The persistent setting gates both click and wheel pulses immediately.
    g_settings.pc_cursor=false;assert(pc_cursor_buttons(0)==0);
    wheel=2;pc_cursor_options_tick(screen);assert(wheel==2);
    g_settings.pc_cursor=true;wheel=0;

    float box[4]={0,0,200,40};Cpu cpu={0};ST64(row,screen);
    ST32(screen+0x36c0,3);ST8(row+0x54,0);
    point[0]=50;options_click(&cpu,screen,row,0,box);assert(actions==1&&refreshes==1&&last_action==0);
    assert(LD32(screen+0x36c0)==3&&LD64(screen+0x36c8)==row);
    ST8(row+0x54,1);options_click(&cpu,screen,row,0,box);assert(actions==1);
    point[0]=150;options_click(&cpu,screen,row,0,box);assert(actions==2);
    point[0]=10;options_click(&cpu,screen,row,1,box);assert(last_action==1);
    point[0]=190;options_click(&cpu,screen,row,1,box);assert(last_action==2);
    point[0]=100;options_click(&cpu,screen,row,1,box);assert(last_action==0);
    options_click(&cpu,screen,row,2,box);assert(actions==6&&refreshes==6);
    // Native row rectangles come from the complete affine transform, at any window resolution.
    uint64_t pane=GUEST_LO+0xb00;ST64(row+0x18,button);ST64(button+0x60,pane);
    *(float*)(pane+0x50)=276;*(float*)(pane+0x54)=40;
    *(float*)(pane+0x70)=*(float*)(pane+0x84)=1;
    *(float*)(pane+0x7c)=130;*(float*)(pane+0x8c)=200;
    ST8(row+0x54,0);position_inside=true;point[0]=100;point[1]=200;
    pc_cursor_options_tick(screen);assert(LD64(screen+0x36c8)==row);
    assert(!pc_cursor_hover_box(box)); // Options never draws a competing PC focus rectangle.
    // The screen reports cursor type 1 but its native resolver chooses
    // manager slot 0. Type and array index must not be confused.
    uint64_t ui=GUEST_LO+0x4000,manager=GUEST_LO+0x5000,control=GUEST_LO+0x5300,node=GUEST_LO+0x5500;
    ST64(screen+0x28,ui);ST64(ui+0xb18,manager);ST64(manager+0x18,control);ST8(screen+0xfc,1);
    ST32(screen+0x9c,0x40);ST64(screen+0x90,node+0x40);ST64(node+0x48,screen+0x88);
    ST64(node+0x18,button);ST64(node+0x20,screen);ST64(screen+0x36c8,0);
    test_cpu.x[19]=42;*(float*)(view+0x2c)=25;options_scroll_valid=false;
    pc_cursor_options_tick(screen);
    assert(activations==1&&activated_node==node&&test_cpu.x[19]==42&&help_updates==1);
    assert(LD64(screen+0xe0)==node&&LD64(screen+0x36c8)==row);
    assert(*(float*)(view+0x2c)==25&&*(float*)(view+0x30)==25&&!pc_cursor_hover_box(box));
    pc_cursor_options_tick(screen);assert(activations==1);
    // Navigation owns the shared highlight until new mouse input; no stale overlay.
    ST64(control+0x48,0);navigation=1ull<<15;
    pc_cursor_options_tick(screen);navigation=0;
    pc_cursor_options_tick(screen);assert(activations==1&&!pc_cursor_hover_box(box));
    point[0]+=2;pc_cursor_options_tick(screen);assert(activations==2);
    click_pending=true;pc_cursor_options_tick(screen);assert(!click_pending&&actions==7);
    // Native Options can capture Npad input: keyboard ownership still has
    // to work when input_buttons_now() reports zero, including after key-up.
    ST64(control+0x48,0);navigation=0;
    SDL_Event event={0};event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_DOWN;
    assert(!pc_cursor_event(&event));pc_cursor_options_tick(screen);assert(activations==2);
    event.type=SDL_EVENT_KEY_UP;assert(!pc_cursor_event(&event));
    pc_cursor_options_tick(screen);assert(activations==2);
    event.type=SDL_EVENT_MOUSE_MOTION;event.motion.xrel=1;
    assert(pc_cursor_event(&event));pc_cursor_options_tick(screen);assert(activations==3);
    // A page rebuild may reuse both guest pointers. Its old long-list offset
    // must not displace a shorter parent page, even while keyboard owns focus.
    event.type=SDL_EVENT_KEY_DOWN;assert(!pc_cursor_event(&event));
    options_scroll_valid=true;options_scroll=900;
    *(float*)(view+0x2c)=*(float*)(view+0x30)=900;
    pc_cursor_options_page_changed();unsigned updates=scroll_updates;
    pc_cursor_options_tick(screen);
    assert(*(float*)(view+0x2c)==0&&*(float*)(view+0x30)==0);
    assert(!options_scroll_valid&&scroll_updates==updates+1&&activations==3);
    // Mouse reclaim on click does not require moving the stationary pointer.
    event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;event.button.button=SDL_BUTTON_LEFT;
    assert(pc_cursor_event(&event));ST64(control+0x48,0);pc_cursor_options_tick(screen);assert(activations==4);
    g_settings.pc_cursor=false;click_pending=true;pc_cursor_options_tick(screen);assert(click_pending&&actions==7);
    pc_cursor_reset();assert(!options_scroll_valid&&!options_screen_active&&!options_rows_active);
    puts("PASS: Options viewport wheel without selection/d-pad, clamps, hover rectangles, click actions, native cursor-type resolution, single shared focus, input ownership, captured-key ownership, same-address page rebuild, setting gate and reboot reset (synthetic host state).");
#ifdef _WIN32
    VirtualFree(memory,0,MEM_RELEASE);
#else
    munmap(memory,65536);
#endif
    return 0;
}
