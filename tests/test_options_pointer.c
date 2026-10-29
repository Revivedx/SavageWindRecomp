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
void guest_call(Cpu *cpu,uint64_t address){(void)cpu;(void)address;abort();}

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
    g_site.opt_screen_rows=0x200;g_site.opt_table=table;
    ST8(screen+0xfe,2);ST32(screen+0x200,1);ST64(screen+0x208,rows);ST64(rows,row);
    ST32(table,1);ST64(table+8,entries);ST32(row+0x50,0);
    active=true;focused=true;sampled_frame=frame;

    // Three detents produce three distinct press/release pairs, not one hold.
    wheel=-3;pc_cursor_options_tick(screen);
    assert(pc_cursor_buttons(0)==(1ull<<15));
    assert(options_wheel_pending==-2&&options_navigation);
    now+=60000000;assert(pc_cursor_buttons(0)==0);
    pc_cursor_options_tick(screen);assert(pc_cursor_buttons(0)==0);
    now+=40000000;pc_cursor_options_tick(screen);assert(pc_cursor_buttons(0)==(1ull<<15));
    now+=100000000;pc_cursor_options_tick(screen);assert(options_wheel_pending==0);
    now+=60000000;assert(pc_cursor_buttons(0)==0);
    wheel=1;now+=40000000;pc_cursor_options_tick(screen);assert(pc_cursor_buttons(0)==(1ull<<13));

    // The persistent setting gates both click and wheel pulses immediately.
    g_settings.pc_cursor=false;assert(pc_cursor_buttons(0)==0);
    wheel=2;pc_cursor_options_tick(screen);assert(wheel==2);
    g_settings.pc_cursor=true;wheel=0;options_wheel_pending=0;

    float box[4]={0,0,200,40};
    ST32(entries+0x10,0);ST64(row+0x18,button);ST8(row+0x54,0);
    point[0]=50;options_click(screen,button,box);assert(options_buttons==1);
    ST8(row+0x54,1);options_buttons=0;options_click(screen,button,box);assert(options_buttons==0);
    point[0]=150;options_click(screen,button,box);assert(options_buttons==1);
    ST32(entries+0x10,1);ST64(row+0x20,button);
    point[0]=10;options_click(screen,button,box);assert(options_buttons==(1ull<<12));
    point[0]=190;options_click(screen,button,box);assert(options_buttons==(1ull<<14));
    point[0]=100;options_click(screen,button,box);assert(options_buttons==1);
    ST32(entries+0x10,2);ST64(row+0x28,button);options_click(screen,button,box);assert(options_buttons==1);
    pc_cursor_reset();assert(!options_buttons&&!options_buttons_until&&!options_wheel_pending&&!options_navigation&&!options_screen_active);
    puts("PASS: Options wheel edges, setting gate, toggle halves, arrows, button confirm and reboot reset (synthetic host state).");
#ifdef _WIN32
    VirtualFree(memory,0,MEM_RELEASE);
#else
    munmap(memory,65536);
#endif
    return 0;
}
