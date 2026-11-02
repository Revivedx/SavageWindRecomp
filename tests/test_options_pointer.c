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
static bool test_menu_ui=true;
bool framepace_menu_ui(void){return test_menu_ui;}
bool framepace_title_ui(void){return false;}
uint64_t framepace_script_frame(uint64_t value){return value;}
void input_pc_escape(bool down){(void)down;abort();}
void input_pc_map_axes(float out[2]){(void)out;abort();}
static unsigned actions,refreshes,last_action,scroll_updates,activations,help_updates;
static uint64_t activated_node;
static uint64_t quest_screen,quest_widget,quest_control,quest_nodes[2],quest_icons[4];
static uint64_t quest_master;
static unsigned quest_selections,quest_categories_clicked,last_category;
static unsigned page_changes;
static uint64_t quick_test_screen,quick_test_list;
static unsigned quick_test_moves;
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
    }else if(address==0x7000123400ull){assert(cpu->x[1]==1);cpu->x[0]=0;}
    else if(address==0x7000d6d710ull){
        // The detail list precedes the real row list in the screen registry.
        cpu->x[0]=cpu->x[0]!=quest_widget?UINT32_MAX:
            cpu->x[1]==quest_nodes[0]?0:cpu->x[1]==quest_nodes[1]?1:UINT32_MAX;
    }else if(address==0x7000d6c54cull){
        assert(quick_test_list&&cpu->x[0]==quick_test_list&&cpu->x[1]<3);quick_test_moves++;
    }else if(address==0x7000e2785cull){
        assert(cpu->x[0]==quick_test_screen&&cpu->x[1]<3);
        ST32(quick_test_screen+0x387c,LD32(quick_test_screen+0x3884+4*cpu->x[1]));
    }
    else if(address==0x7000d6c870ull){
        assert(cpu->x[0]==quest_widget&&cpu->x[1]<2);
        unsigned index=(unsigned)cpu->x[1];quest_selections++;
        ST32(quest_widget+0x30c,index);ST32(quest_widget+0x310,index);
        // Leave the manager unchanged: the hook must also reconcile visible
        // focus when a native list's router has not applied it yet.
    }else if(address==0x7000f6f800ull){
        unsigned category;assert(cpu->x[0]==quest_screen);
        assert(sscanf((char*)(uintptr_t)cpu->x[1],"Pa_ChallengeCategory_%u",&category)==1&&category<4);
        cpu->x[0]=quest_icons[category];
    }else if(address==0x7000d6c5c4ull){abort();} // Immediate rebuilding is unsafe inside the manager hook.
    else if(address==0x7000d6c244ull){
        assert(cpu->x[0]==quest_master&&cpu->x[1]==0);
        quest_categories_clicked++;last_category=(unsigned)cpu->x[2];cpu->x[19]=0xdead;
    }else if(address==0x7000d6c9b0ull){
        assert(cpu->x[0]==quest_widget&&cpu->x[1]==0&&cpu->x[2]==last_category);
    }else if(address==0x7000123404ull){
        assert(options_box(cpu->x[1],(float*)(uintptr_t)cpu->x[0]));
    }else if(address==0x7001403a88ull){
        assert(cpu->x[0]==quest_screen+0x178&&cpu->x[1]==0x7001e2f4a8ull);page_changes++;
    }else if(address==0x7000123408ull){abort();} // Native clipping mutates focus; never scan through it.
    else if(address==0x700012340cull){cpu->x[0]=1;}
    else if(address==0x70013f59f8ull){assert(cpu->x[0]==quantity_screen);}
    else abort();
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
    uint64_t router=GUEST_LO+0x5900,router_vt=GUEST_LO+0x5a00;
    ST64(ui+0x48,router);ST64(router,router_vt);ST64(router_vt+0x28,0x7000123400ull);
    assert(hover_target(&cpu,screen,0)&&!hover_target(&cpu,screen,1));
    // Generic hover callbacks run only on entering a different element.
    g_settings.pc_cursor=true;unsigned before=activations;hover_leave();ST64(control+0x48,0);
    assert(hover_enter(&cpu,control,screen,node,0x7000f57da8));
    for(int i=0;i<120;i++){ST64(control+0x48,0);assert(!hover_enter(&cpu,control,screen,node,0x7000f57da8));}
    assert(activations==before+1);
    uint64_t other=GUEST_LO+0x5800;ST64(other+0x20,screen);
    assert(hover_enter(&cpu,control,screen,other,0x7000f57da8));assert(activations==before+2);
    ST64(control+0x48,0);
    assert(!hover_enter(&cpu,control,screen,other,0x7000f57da8));assert(activations==before+2);
    hover_leave();ST64(control+0x48,0);
    assert(hover_enter(&cpu,control,screen,node,0x7000f57da8));assert(activations==before+3);
    // Adventure Log must change the virtualized list itself, not just cache a
    // hover node while the list keeps restoring the previous keyboard row.
    uint64_t quest=GUEST_LO+0x6000;quest_widget=GUEST_LO+0x6400;
    quest_screen=screen;quest_control=control;quest_nodes[0]=node;quest_nodes[1]=other;
    ST64(screen,0x7001cf9ad8ull);ST64(screen+0x3bb0,quest);ST64(quest,0x7001ce1a30ull);
    // A real pause screen registers detail, quest and memory lists separately.
    // quest+0x300 is the detail list and does not own quest row nodes.
    uint64_t auxiliary=GUEST_LO+0xb000;quest_master=auxiliary;
    ST64(quest+0x300,auxiliary);ST64(auxiliary,0x7001cdbb58ull);
    ST32(screen+0x388,3);ST64(screen+0x390,screen+0x398);
    ST64(screen+0x398,auxiliary);ST64(screen+0x3a0,quest_widget);
    ST64(quest_widget,0x7001cdbb58ull);ST32(quest_widget+0x30c,1);ST32(quest_widget+0x310,1);
    ST32(quest_widget+0x2dc,0); // All visible items are on the same scroll page.
    ST32(screen+0x3a48,2);ST32(screen+0x3a4c,2);
    for(unsigned i=0;i<6;i++)((float*)(uintptr_t)(quest_widget+0x2c0))[i]=i*5.f;
    float quest_view[6];memcpy(quest_view,(void*)(uintptr_t)(quest_widget+0x2c0),sizeof quest_view);
    assert(quest_focus(&cpu,screen,node));assert(quest_selections==1);
    assert(LD32(quest_widget+0x30c)==0&&LD32(quest_widget+0x310)==0);
    assert(memcmp(quest_view,(void*)(uintptr_t)(quest_widget+0x2c0),sizeof quest_view)==0);
    for(int i=0;i<120;i++)assert(quest_focus(&cpu,screen,node));
    assert(quest_selections==1); // No repeated selection/sound while resting.
    assert(quest_focus(&cpu,screen,other)&&quest_selections==2);
    assert(LD32(quest_widget+0x2dc)==0); // Moving focus never changes that page.
    assert(!quest_focus(&cpu,screen,GUEST_LO+0x7770)); // Other inventory buttons are untouched.
    // Category icons dispatch their real list action; locked categories do not.
    uint64_t columns=GUEST_LO+0x6900;ST64(quest_master+0x290,columns);ST32(quest_master+0x288,4);
    for(unsigned i=0;i<4;i++) {
        uint64_t column=GUEST_LO+0x7000+0x400*i;quest_icons[i]=GUEST_LO+0x6a00+0x100*i;
        ST64(columns+8*i,column);ST32(column+0x29c,3);ST32(column+0x2f4,1);ST32(quest+0x30c+4*i,0);
        uint64_t icon=quest_icons[i];*(float*)(icon+0x50)=50;*(float*)(icon+0x54)=50;
        *(float*)(icon+0x70)=*(float*)(icon+0x84)=1;
        *(float*)(icon+0x7c)=-347+58.f*i;*(float*)(icon+0x8c)=191;
        uint64_t ni=GUEST_LO+0xa000+0x100*i,bi=ni+0x60,layout=ni+0xb0;
        uint64_t parent=GUEST_LO+0xd000+0x100*i;
        ST64(ni+0x18,bi);ST64(bi+0x20,layout);ST64(layout+0x18,parent);ST64(bi+0x60,icon);
        // The oversized containing layout must not swallow neighboring icons.
        snprintf((char*)(uintptr_t)(parent+0xb8),40,"Pa_ChallengeCategory_%02u",i);
        *(float*)(parent+0x50)=211.2f;*(float*)(parent+0x54)=126;
        ST64(ni+0x48,i==3?screen+0x88:ni+0x140);
    }
    ST64(screen+0x90,GUEST_LO+0xa040);ST32(screen+0x9c,0x40);
    point[1]=191;cpu.x[19]=42;
    for(unsigned i=0;i<4;i++) {
        ST32(quest_master+0x2e8,(i+1)%4);
        point[0]=-347+58.f*i;click_pending=true;
        assert(quest_categories(&cpu,screen)&&!click_pending&&last_category==i&&cpu.x[19]==42);
    }
    assert(quest_categories_clicked==4);ST8(LD64(columns+8)+0x38d,1);
    point[0]=-289;click_pending=true;assert(quest_categories(&cpu,screen)&&!click_pending);
    assert(quest_categories_clicked==4);
    // Ability Controls uses the same native list registry, not a quest widget.
    ST64(screen,0x7001ceb808ull);ST32(quest_widget+0x30c,1);ST32(quest_widget+0x310,1);
    assert(quest_focus(&cpu,screen,node)&&quest_selections==3);
    for(int i=0;i<120;i++)assert(quest_focus(&cpu,screen,node));
    assert(quest_selections==3);ST64(screen,0x7001cf9ad8ull);
    // Exercise the actual manager hook and SDL ownership, including a mouse
    // left stationary for more than the former 300 ms cutoff.
#ifdef _WIN32
    void *vt_memory=VirtualAlloc((void*)0x7001cf0000ull,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    void *vt_memory=mmap((void*)0x7001cf0000ull,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
#endif
    assert(vt_memory==(void*)0x7001cf0000ull);ST64(0x7001cf9ad8ull+0x38,0x7000123408ull);
    uint64_t sm=GUEST_LO+0x9000,sa=GUEST_LO+0x9400,nvt=GUEST_LO+0x9800;
    g_site.screen_mgr=GUEST_LO+0x9900;ST64(g_site.screen_mgr,sm);ST32(sm+0x28,0x2f);ST64(sm+0x30,sa);
    ST64(sa+8*0x2e,screen);ST64(node,nvt);ST64(nvt+0x28,0x700012340cull);
    ST64(screen+0x90,node+0x40);ST64(node+0x48,screen+0x88);
    point[0]=100;point[1]=200;click_pending=false;navigation=0;g_settings.pc_cursor=true;
    ST32(quest_widget+0x30c,1);ST32(quest_widget+0x310,1);ST64(control+0x48,other);ST64(screen+0xe0,other);
    cpu.x[0]=manager;cpu.x[1]=0;
    event.type=SDL_EVENT_KEY_DOWN;assert(!pc_cursor_event(&event));
    rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);assert(quest_selections==3);
    assert(pc_cursor_hide_system());
    event.type=SDL_EVENT_KEY_UP;assert(!pc_cursor_event(&event));
    rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);assert(quest_selections==3);
    event.type=SDL_EVENT_MOUSE_MOTION;event.motion.xrel=1;assert(pc_cursor_event(&event));
    now+=1000000000;rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);
    assert(!pc_cursor_hide_system());
    assert(quest_selections==4&&LD64(screen+0xe0)==node);
    for(int i=0;i<120;i++){now+=16666667;rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);}
    assert(quest_selections==4&&LD64(control+0x48)==node);
    // Header hit areas use actual transformed text panes and the normal page
    // transition. Modal submenus must retain input over the paused screen.
    uint64_t header=GUEST_LO+0xc000,layout=GUEST_LO+0xc400,root=GUEST_LO+0xc800;
    uint64_t left=GUEST_LO+0xcc00,right=GUEST_LO+0xce00;
    ST64(header+0x30,layout);ST64(layout+0x18,root);ST8(header+0xfe,2);
    ST64(root+0x28,left+8);ST64(left+0x10,right+8);ST64(right+0x10,root+0x20);
    ST64(left+0x28,left+0x20);ST64(right+0x28,right+0x20);
    strcpy((char*)(uintptr_t)(left+0xb8),"T_LeftText_00");
    strcpy((char*)(uintptr_t)(right+0xb8),"T_RightText_00");
    for(unsigned i=0;i<2;i++) {
        uint64_t p=i?right:left;*(float*)(p+0x50)=130;*(float*)(p+0x54)=23;
        *(float*)(p+0x70)=*(float*)(p+0x84)=1;*(float*)(p+0x7c)=i?150:-150;*(float*)(p+0x8c)=287;
    }
    ST64(sa+8*0x2f,header);point[1]=287;
    for(unsigned current=0;current<3;current++)for(unsigned side=0;side<2;side++) {
        ST32(screen+0x3a48,current);ST32(screen+0x3a4c,current);
        point[0]=side?150:-150;click_pending=true;
        bool possible=!(current==1&&side==1)&&!(current==2&&side==0);
        unsigned before=page_changes;
        assert(pause_headers(&cpu,screen,sa,0x45)==possible);
        assert(page_changes==before+possible&&click_pending==!possible);
        if(possible)assert(LD32(screen+0x3a4c)==(current==0?(side?1:2):0));
    }
    ST64(sa+8*0x37,header);click_pending=true;
    assert(!pause_headers(&cpu,screen,sa,0x45)&&click_pending);
    // Responses on the other cursor channel must still block text advance.
    dialogue_active=true;ST64(header,0x7001d4ed48ull);ST64(sa+8*0x4a,header);
    ST64(header+0x90,node+0x40);ST32(header+0x9c,0x40);ST64(node+0x48,header+0x88);
    *(float*)(pane+0x7c)=950;*(float*)(pane+0x8c)=-133;
    *(float*)(pane+0x50)=*(float*)(pane+0x54)=50;
    *(float*)(pane+0x70)=*(float*)(pane+0x84)=1;*(float*)(pane+0x74)=*(float*)(pane+0x80)=0;
    assert(dialogue_choices(&cpu,sa,0x4b));
    ST8(header+0xfe,0);assert(!dialogue_choices(&cpu,sa,0x4b));dialogue_active=false;
    // Real NPCs use MessageDialog at 4a, not the spirit-orb screen at 1a.
    // Detect the screen type independently of its registry position.
    g_site.pause_state=GUEST_LO+0x9980;ST32(g_site.pause_state,0);
    ST32(sm+0x28,0x4b);ST8(header+0xfe,2);
    pc_cursor_frame();assert(dialogue_active&&pc_cursor_active());
    ST64(sa+8*0x4a,0);ST64(sa+8*0x49,header);
    pc_cursor_frame();assert(dialogue_active);
    ST64(header,0x7001ce59b8ull);pc_cursor_frame();assert(!dialogue_active);
    ST64(header,0x7001d4ed48ull);pc_cursor_frame();assert(dialogue_active);
    // With no response buttons, one release advances text through the ordinary
    // confirm button. Holding the mouse does not generate repeated clicks.
    ST64(header+0x90,header+0x88);click_pending=true;click_deadline=now+100000000;
    pointer_keyboard_input=false;motion_at=now;cpu.x[0]=manager;cpu.x[1]=0;
    rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);
    assert(!click_pending&&(pc_cursor_buttons(0)&1));
    now+=60000000;assert(!(pc_cursor_buttons(0)&1));
    ST8(header+0xfe,0);pc_cursor_frame();assert(!dialogue_active);
    // Preview gestures must never inject ZL or directional buttons.
    held=true;pointer_keyboard_input=false;
    assert(pc_cursor_buttons(0)==0);
    // Quantity controls only update the native number, never the purchase.
    uint64_t qtyroot=GUEST_LO+0xd200,minus=GUEST_LO+0xd400,plus=GUEST_LO+0xd600;
    ST64(pane+0x18,qtyroot);ST64(qtyroot+0x28,minus+8);
    ST64(minus+0x10,plus+8);ST64(plus+0x10,qtyroot+0x20);
    ST64(minus+0x28,minus+0x20);ST64(plus+0x28,plus+0x20);
    strcpy((char*)(uintptr_t)(minus+0xb8),"Pa_ArrowL_00");
    strcpy((char*)(uintptr_t)(plus+0xb8),"Pa_ArrowR_00");
    *(float*)(minus+0x7c)=358;*(float*)(plus+0x7c)=442;
    ST64(header,0x7001d4ed48ull);ST8(header+0xfe,2);ST64(sa+8*0x49,header);
    ST32(header+0x750,56);ST32(header+0x754,1);ST64(header+0x650,pane);
    *(float*)(pane+0x7c)=400;*(float*)(pane+0x8c)=-157;
    dialogue_active=true;quantity_until=0;point[0]=400;point[1]=-157;click_pending=true;
    unsigned old_actions=actions;
    assert(quantity_pointer(&cpu,sa,0x4b)&&quantity_edit&&!click_pending);
    event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_9;
    assert(pc_cursor_event(&event));assert(pc_cursor_event(&event));
    assert(quantity_request==56);quantity_pointer(&cpu,sa,0x4b);
    assert(LD32(header+0x754)==56&&actions==old_actions);
    point[0]=358;click_pending=true;quantity_pointer(&cpu,sa,0x4b);
    assert(LD32(header+0x754)==55&&!quantity_edit&&actions==old_actions);
    ST32(header+0x754,1);click_pending=true;quantity_pointer(&cpu,sa,0x4b);
    assert(LD32(header+0x754)==1&&actions==old_actions);
    // Holding an actual native arrow repeats after its delay and stops exactly
    // on release; the release must not add a second click or confirm a purchase.
    point[0]=442;gesture_down=true;frame_press=true;frame_release=false;
    quantity_pointer(&cpu,sa,0x4b);assert(LD32(header+0x754)==2);
    frame_press=false;now+=200000000;quantity_pointer(&cpu,sa,0x4b);
    assert(LD32(header+0x754)==2);
    now+=160000000;quantity_pointer(&cpu,sa,0x4b);assert(LD32(header+0x754)==3);
    now+=70000000;quantity_pointer(&cpu,sa,0x4b);assert(LD32(header+0x754)==4);
    gesture_down=false;frame_release=true;click_pending=true;
    quantity_pointer(&cpu,sa,0x4b);assert(LD32(header+0x754)==4&&!click_pending&&!quantity_hold);
    frame_release=false;now+=70000000;quantity_pointer(&cpu,sa,0x4b);
    assert(LD32(header+0x754)==4&&actions==old_actions);
    assert(*(float*)(plus+0x48)>1.f&&*(float*)(minus+0x48)==1.f);
    point[0]=358;gesture_down=true;frame_press=true;
    quantity_pointer(&cpu,sa,0x4b);assert(LD32(header+0x754)==3);
    frame_press=false;now+=350000000;quantity_pointer(&cpu,sa,0x4b);
    assert(LD32(header+0x754)==2);
    now+=70000000;quantity_pointer(&cpu,sa,0x4b);assert(LD32(header+0x754)==1);
    now+=70000000;quantity_pointer(&cpu,sa,0x4b);assert(LD32(header+0x754)==1);
    gesture_down=false;frame_release=true;quantity_pointer(&cpu,sa,0x4b);frame_release=false;
    // Buying uses a derived five-row list. It must update its own selection
    // just like the selling grid, without recentering or replaying hover.
    shop_screen=screen;ST64(screen,0x7001cfc958ull);ST64(quest_widget,0x7001cfcf78ull);
    ST32(quest_widget+0x30c,1);ST32(quest_widget+0x310,1);
    unsigned before_shop=quest_selections;
    assert(quest_focus(&cpu,screen,node)&&quest_selections==before_shop+1);
    for(unsigned i=0;i<120;i++)assert(quest_focus(&cpu,screen,node));
    assert(quest_selections==before_shop+1);
    assert(quest_focus(&cpu,screen,other)&&quest_selections==before_shop+2);
    assert(LD32(quest_widget+0x2dc)==0);
    shop_screen=0;ST64(screen,0x7001cf9ad8ull);ST64(quest_widget,0x7001cdbb58ull);
    // Native yaw changes must leave the actor translation and UI mode intact.
#ifdef _WIN32
    void *yaw_memory=VirtualAlloc((void*)0x7001e10000ull,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    void *yaw_memory=mmap((void*)0x7001e10000ull,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
#endif
    assert(yaw_memory==(void*)0x7001e10000ull);
    uint64_t owner=GUEST_LO+0xe000,actor=GUEST_LO+0xe400;
    ST64(0x7001e15fb8ull,owner);ST64(owner+0x48,actor);ST8(owner+0x1f4,0x20);
    float original[12]={1,0,0,12,0,1,0,34,0,0,1,56};
    memcpy((void*)(uintptr_t)(actor+0x398),original,sizeof original);
    *(float*)(uintptr_t)0x7001e15fa8ull=-32.5f;*(float*)(uintptr_t)0x7001e15facull=-2.5f;
    inventory_rotate(&cpu,100);
    assert(LD8(owner+0x1f4)==0x20&&!memcmp(original,(void*)(uintptr_t)(actor+0x398),sizeof original));
    assert(fabsf(*(float*)(uintptr_t)0x7001e15fa8ull-13.5f)<.001f);
    pc_cursor_frame();assert(!preview_defaults_saved);
    assert(*(float*)(uintptr_t)0x7001e15fa8ull==-32.5f&&*(float*)(uintptr_t)0x7001e15facull==-2.5f);
#ifdef _WIN32
    VirtualFree(yaw_memory,0,MEM_RELEASE);
#else
    munmap(yaw_memory,65536);
#endif
    // A controller can open the map before pointer mode is active. It must
    // keep the native map helpers in charge, with no stale mouse restoration.
    active=false;quantity_edit=false;pointer_gamepad_input=false;
    event.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;event.gbutton.button=SDL_GAMEPAD_BUTTON_START;
    assert(!pc_cursor_event(&event)&&pointer_gamepad_input);
    active=true;map_canvas_active=true;map_view_valid=map_sent_valid=true;
    map_down=map_anchor_active=map_picker_seen=true;click_pending=true;
    held=true;confirm_until=now+100000000;wheel=3;
    event.gbutton.button=SDL_GAMEPAD_BUTTON_SOUTH;
    assert(!pc_cursor_event(&event)&&!held&&!wheel&&!confirm_until);
    assert(!pc_cursor_map_navigation()&&rt_pc_map_focus(&cpu,0)==0);
    uint64_t map_screen=GUEST_LO+0xf000;
    ST32(g_site.pause_state,2);ST64(sa+8*0x2a,map_screen);ST64(sa+8*0x2f,0);
    ST64(map_screen,0x7001ce8298ull);ST8(map_screen+0xfe,2);
    ST32(map_screen+0x880,12345);special_frame=frame;
    assert(!native_special(&cpu,sa,0x7000123404));
    assert(LD32(map_screen+0x880)==12345&&!map_view_valid&&!map_canvas_active&&!map_down&&!map_anchor_active&&!click_pending);
    // No axis event at rest or within the configured dead zone steals focus.
    pointer_gamepad_input=pointer_keyboard_input=false;g_settings.stick_deadzone[0]=.25f;
    event.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;event.gaxis.axis=SDL_GAMEPAD_AXIS_LEFTX;
    event.gaxis.value=2000;assert(!pc_cursor_event(&event)&&!pointer_gamepad_input);
    event.gaxis.value=10000;assert(!pc_cursor_event(&event)&&pointer_gamepad_input&&pointer_keyboard_input);
    map_view_valid=map_sent_valid=map_canvas_active=true;position_inside=false;
    pc_cursor_frame();assert(!map_view_valid&&!map_sent_valid&&!map_canvas_active);
    // Stationary/synthetic mouse motion is not a takeover; real movement is.
    event.type=SDL_EVENT_MOUSE_MOTION;event.motion.xrel=event.motion.yrel=0;
    assert(pc_cursor_event(&event)&&pointer_gamepad_input);
    event.motion.xrel=1;assert(pc_cursor_event(&event)&&!pointer_gamepad_input&&!pointer_keyboard_input);
    map_canvas_active=true;assert(pc_cursor_map_navigation()&&rt_pc_map_focus(&cpu,0)==1);
    // WASD's pointer-map navigation remains available after controller use.
    pointer_gamepad_input=true;event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_W;
    assert(!pc_cursor_event(&event)&&!pointer_gamepad_input&&pc_cursor_map_navigation());
    pc_cursor_reset();assert(!options_scroll_valid&&!options_screen_active&&!options_rows_active);
    assert(!pointer_gamepad_input);
    puts("PASS: controller opens/owns map, native helper bypass, cursor-cache release outside window, stick dead zone, mouse reclaim and WASD preservation");
    // Quick equipment: movement/click/drag cannot select; wheel moves once and
    // clamps. The cursor is hidden even if its old absolute position is outside.
    quick_test_screen=GUEST_LO+0xa000;quick_test_list=GUEST_LO+0xe000;
    ST64(sa+8*0x2d,quick_test_screen);ST64(quick_test_screen,0x7001cf51d8ull);
    ST8(quick_test_screen+0xfe,2);ST32(g_site.pause_state,4);
    ST64(quick_test_screen+0x37a8,quick_test_list);ST32(quick_test_screen+0x3880,3);
    ST32(quick_test_list+0x2ac,3);
    for(int i=0;i<3;i++)ST32(quick_test_screen+0x3884+4*i,10+10*i);
    ST32(quick_test_screen+0x387c,20);focused=true;position_inside=false;
    pc_cursor_frame();assert(quick_wheel_only&&pc_cursor_hide_system());
    event.type=SDL_EVENT_MOUSE_MOTION;event.motion.xrel=400;event.motion.yrel=200;
    assert(pc_cursor_event(&event));
    event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;event.button.button=SDL_BUTTON_LEFT;
    assert(pc_cursor_event(&event)&&!held);
    gesture_down=gesture_dragged=true;gesture_delta[0]=500;click_pending=true;
    frame++;assert(native_special(&cpu,sa,0));assert(!quick_test_moves&&!click_pending);
    event.type=SDL_EVENT_MOUSE_WHEEL;event.wheel.y=-1;
    assert(pc_cursor_event(&event));frame++;assert(native_special(&cpu,sa,0));
    assert(quick_test_moves==1&&LD32(quick_test_screen+0x387c)==30);
    assert(pc_cursor_event(&event));frame++;native_special(&cpu,sa,0);assert(quick_test_moves==1);
    event.wheel.y=1;assert(pc_cursor_event(&event));frame++;native_special(&cpu,sa,0);
    assert(quick_test_moves==2&&LD32(quick_test_screen+0x387c)==20);
    event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_RIGHT;
    assert(!pc_cursor_event(&event));assert(pc_cursor_buttons(1ull<<14)==(1ull<<14));
    ST32(g_site.pause_state,0);ST64(sa+8*0x2d,0);pc_cursor_frame();assert(!quick_wheel_only);
    // Game Over has pause state zero and is neither title nor pause UI. Its
    // native cursor nodes must get hover/help and one confirm, without touching
    // loaded background screens or replaying stationary hover callbacks.
    pc_cursor_reset();test_menu_ui=false;ST8(header+0xfe,0);
    ST64(screen,0x7001cef0a8ull);ST8(screen+0xfe,2);ST8(screen+0xfc,1);
    ST64(sa+8*0x30,screen);ST32(sm+0x28,0x4b);
    ST64(screen+0x90,node+0x40);ST32(screen+0x9c,0x40);
    ST64(node+0x48,other+0x40);ST64(other+0x48,screen+0x88);
    uint64_t second_button=GUEST_LO+0x6000,second_pane=GUEST_LO+0x6200;
    ST64(node,nvt);ST64(node+0x18,button);ST64(node+0x20,screen);ST64(button+0x60,pane);
    ST64(other,nvt);ST64(other+0x18,second_button);ST64(other+0x20,screen);ST64(second_button+0x60,second_pane);
    for(int i=0;i<2;i++) {
        uint64_t p=i?second_pane:pane;
        *(float*)(p+0x7c)=0;*(float*)(p+0x8c)=i?-154:-96;
        *(float*)(p+0x50)=264;*(float*)(p+0x54)=54;
        *(float*)(p+0x70)=*(float*)(p+0x84)=1;
        *(float*)(p+0x74)=*(float*)(p+0x80)=0;
    }
    pc_cursor_frame();assert(game_over_screen==screen&&pc_cursor_active()&&!dialogue_active);
    position_inside=true;point[0]=0;point[1]=-96;motion_at=now;sampled_frame=frame;
    cpu.x[0]=manager;cpu.x[1]=0;ST64(control+0x48,0);
    unsigned old_activations=activations;
    rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);
    assert(activated_node==node&&activations==old_activations+1);
    for(int i=0;i<60;i++)rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);
    assert(activations==old_activations+1);
    point[1]=-154;click_pending=true;
    rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);
    assert(activated_node==other&&LD32(manager+0x10)==1&&!click_pending);
    // Quit opens a different screen over Game Over. Its nodes must own hover
    // and clicks; the underlying Continue/Quit controls must not steal focus.
    uint64_t modal=GUEST_LO+0xa000,modal_node=GUEST_LO+0xe000,modal_button=GUEST_LO+0xe200,modal_pane=GUEST_LO+0xe400;
    ST64(sa+8*0x39,modal);ST64(modal,0x7001cfffe0ull);ST8(modal+0xfe,2);ST8(modal+0xfc,1);ST64(modal+0x28,ui);
    ST32(modal+0x9c,0x40);ST64(modal+0x90,modal_node+0x40);ST64(modal_node+0x48,modal+0x88);
    ST64(modal_node,nvt);ST64(modal_node+0x18,modal_button);ST64(modal_node+0x20,modal);ST64(modal_button+0x60,modal_pane);
    memcpy((void*)(uintptr_t)modal_pane,(void*)(uintptr_t)second_pane,0x100);
    *(float*)(modal_pane+0x8c)=-154;pc_cursor_frame();assert(game_over_screen==modal);
    click_pending=true;rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);
    assert(activated_node==modal_node&&LD32(manager+0x10)==1&&!click_pending);
    ST64(sa+8*0x39,0);pc_cursor_frame();assert(game_over_screen==screen);
    rt_pc_cursor_manager(&cpu,0x7000f57da8,0x7000123404);assert(activated_node==other);
    g_settings.pc_cursor=false;pc_cursor_frame();assert(!pc_cursor_active());
    g_settings.pc_cursor=true;ST64(sa+8*0x30,0);pc_cursor_frame();assert(!game_over_screen&&!pc_cursor_active());
    puts("PASS: Options scroll/click/shared focus, SDL ownership/cursor visibility, page rebuild, Adventure Log/Ability Controls list registry, stationary hover, nested category clicks/locks, viewport preservation, pause header transitions/modal priority, setting gate and reboot reset (synthetic host state).");
#ifdef _WIN32
    VirtualFree(vt_memory,0,MEM_RELEASE);
    VirtualFree(memory,0,MEM_RELEASE);
#else
    munmap(vt_memory,65536);
    munmap(memory,65536);
#endif
    return 0;
}
