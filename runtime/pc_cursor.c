// Native menu pointer input. The specialized map/shortcut adapters target BOTW 1.9.0.
// All guest pointers are accessed on the game thread. SDL runs on the window thread.
#include <stdatomic.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "pc_cursor.h"
#include "pc_cursor_math.h"
#include "game_sites.h"
#include "framepace.h"
#include "hle.h"
#include "settings.h"

static _Atomic bool active, focused = true;
static _Atomic uint64_t position;
static _Atomic bool position_inside;
static _Atomic unsigned presses, releases;
static _Atomic bool held;
static bool right_back_held; // window thread: release must clear Back even after the menu closes
static float point[2];
static bool frame_press, frame_release, frame_held;
static unsigned last_press, last_release;
static uint64_t next_log;
static bool click_pending;
static uint64_t click_deadline;
static uint64_t injected_manager;
static uint32_t saved_action;
static _Atomic int wheel;
static _Atomic unsigned pointer_motion;
static _Atomic uint64_t options_buttons,options_buttons_until;
static int options_wheel_pending;
static unsigned options_navigation_motion;
static bool options_navigation;
static uint64_t options_screen_active,options_next_pulse;
static _Atomic bool quick_confirmed;
static _Atomic uint64_t confirm_until, hover_until, hover_rect[2], motion_at;
static bool map_down, map_dragged;
static float map_origin[2],map_last[2];
static bool map_anchor_active,map_picker_seen;
static float map_anchor[2];
static uint64_t map_anchor_deadline;
static _Atomic bool map_canvas_active;
static uint64_t map_nav_ns,map_snap_button;
static bool map_view_valid;
static float map_view_center[2];
static float map_sent_world[2];
static bool map_sent_valid,map_native_snapped;
static bool map_zooming;
static uint64_t map_select_until;
static uint64_t special_frame=UINT64_MAX;
static uint64_t sampled_frame=UINT64_MAX;
static _Atomic int window_width,window_height;
static bool gesture_down,gesture_dragged;
static float gesture_start[2],gesture_last[2],gesture_delta[2];
static void sample_pointer(void);
static void inventory_pointer(Cpu *c);
static bool pointer_test(void) { return getenv("SAVAGEWIND_PC_POINTER_SCRIPT") != NULL; }

static bool enabled(void) {
    return g_settings.pc_cursor;
}
bool pc_cursor_enabled(void) { return enabled(); }


extern bool menu_cursor_free(void);
bool pc_cursor_active(void) { return enabled() && active && (focused || pointer_test()) && !menu_cursor_free(); }
bool pc_cursor_map_navigation(void) { return pc_cursor_active() && map_canvas_active; }
bool pc_cursor_hide_system(void) { return pc_cursor_map_navigation() && position_inside; }

void pc_cursor_frame(void) {
    bool ui = g_site.pause_state &&
        (framepace_menu_ui() || framepace_title_ui() || *(uint32_t *)(uintptr_t)g_site.pause_state == 4);
    active = enabled() && ui;
    if(!pc_cursor_active()){
        options_buttons=options_buttons_until=0;options_wheel_pending=0;
        options_navigation=false;options_screen_active=options_next_pulse=0;
        map_canvas_active=false;map_nav_ns=0;map_view_valid=false;
        held=false;last_press=presses;last_release=releases;wheel=0;click_pending=false;
        gesture_down=map_down=false;map_anchor_active=map_picker_seen=false;
    }
    if(getenv("SAVAGEWIND_PC_DUMP_UI")&&ui) {
        static uint8_t seen[256];
        uint64_t mgr=LD64(g_site.screen_mgr);
        unsigned pause=LD32(g_site.pause_state),mask=pause==4?4:pause==2?2:1;
        if(mgr)for(unsigned id=0;id<LD32(mgr+0x28)&&id<256;id++) {
            uint64_t arr=LD64(mgr+0x30),s=arr?LD64(arr+8*id):0;
            if(!s||LD8(s+0xfe)!=2||(seen[id]&mask))continue;
            seen[id]|=mask;
            uint64_t vt=LD64(s);
            LOG("[pc-screen] pause %u id %x object %llx vtable %llx update %llx button %llx\n",pause,id,s,vt,LD64(vt+0xf8),LD64(vt+0x190));
            for(unsigned off=0x40;off<0xd0;off+=8) {
                uint64_t p=LD64(s+off);
                if(p<MAIN_BASE+0x1000000||p>=g_site.main_data)continue;
                unsigned n=0;while(n<64&&LD8(p+n)>=32&&LD8(p+n)<127)n++;
                if(n&&n<64&&LD8(p+n)==0)LOG("[pc-screen-name] +%x %.64s\n",off,(char*)p);
            }
        }
    }
    const char *spec = getenv("SAVAGEWIND_PC_POINTER_SCRIPT");
    if (spec) {
        extern uint64_t renderer_present_count(void);
        uint64_t f = framepace_script_frame(renderer_present_count());
        char buf[2048]; snprintf(buf, sizeof buf, "%s", spec);
        char *save = NULL;
        static unsigned long last_event, released_event;
        unsigned long event=0,next_event=0;unsigned action=0,next_action=0;float from[2]={0},to[2]={0};
        for (char *s = strtok_r(buf, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
            unsigned long at; float x,y; unsigned click=0;
            if (sscanf(s,"%lu:%f/%f:%u",&at,&x,&y,&click) >= 3 && f >= at) {
                point[0]=x; point[1]=y;event=at;action=click;from[0]=x;from[1]=y;
            } else if(at>f) {
                next_event=at;next_action=click;to[0]=x;to[1]=y;break;
            }
        }
        if(next_event>event&&(next_action==6||next_action==7)) {
            float t=(float)(f-event)/(next_event-event);
            point[0]=from[0]+(to[0]-from[0])*t;point[1]=from[1]+(to[1]-from[1])*t;
        }
        bool fresh=event&&last_event!=event;
        bool down=fresh&&(action==1||action==2);
        bool up=(fresh&&action==3)||(action==1&&f>=event+6&&released_event!=event);
        held=action==2||action==6||(action==1&&f<event+6);
        if(down)presses++;
        if(up){releases++;released_event=event;}
        if(fresh) {
            if(action!=4&&action!=5)pointer_motion++;
            if(action==4)wheel++;
            if(action==5)wheel--;
            motion_at=plat_now_ns();last_event=event;
        }
    }
}

// Consume the SDL edges at the UI update, not at VFR updates which may run several times before the UI.
static void sample_pointer(void) {
    extern uint64_t renderer_present_count(void);
    uint64_t f=renderer_present_count();
    if(sampled_frame==f)return;
    sampled_frame=f;
    if(!pointer_test()) {
        uint64_t p=position;uint32_t xy[2]={(uint32_t)p,(uint32_t)(p>>32)};memcpy(point,xy,sizeof point);
    }
    unsigned a=presses,b=releases;
    frame_press=a!=last_press;frame_release=b!=last_release;frame_held=held;
    last_press=a;last_release=b;
    gesture_delta[0]=gesture_delta[1]=0;
    if(frame_press){gesture_down=true;gesture_dragged=false;memcpy(gesture_start,point,8);memcpy(gesture_last,point,8);}
    if(gesture_down) {
        if(hypotf(point[0]-gesture_start[0],point[1]-gesture_start[1])>6)gesture_dragged=true;
        gesture_delta[0]=point[0]-gesture_last[0];gesture_delta[1]=point[1]-gesture_last[1];
        memcpy(gesture_last,point,8);
    }
    if(frame_release) {
        if(gesture_down&&!gesture_dragged){click_pending=true;click_deadline=plat_now_ns()+100000000;}
        gesture_down=false;
    }
    if(!pc_cursor_active()||plat_now_ns()>click_deadline)click_pending=false;
}

static void window_position(float x,float y) {
    if(window_width<=0||window_height<=0)return;
    float p[2];position_inside=pc_cursor_to_layout(window_width,window_height,x,y,p);
    uint32_t xy[2];memcpy(xy,p,8);position=(uint64_t)xy[0]|(uint64_t)xy[1]<<32;
}

void pc_cursor_window(SDL_Window *w) {
    if (!enabled() || !w) return;
    int width, height;
    float x, y;
    SDL_GetWindowSize(w, &width, &height);
    window_width=width;window_height=height;
    SDL_GetMouseState(&x, &y);
    if (width <= 0 || height <= 0) return;
    // eui HitTest uses layout space, centered, Y up. Full pause screens use a fitted 16:9 view.
    float p[2];
    position_inside=pc_cursor_to_layout(width,height,x,y,p);
    uint32_t xy[2]; memcpy(xy, p, sizeof xy);
    position = (uint64_t)xy[0] | (uint64_t)xy[1] << 32;
}

bool pc_cursor_event(const SDL_Event *e) {
    extern void input_pc_escape(bool down);
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        focused = false; held = false; releases++;
        if(right_back_held){input_pc_escape(false);right_back_held=false;}
    }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused = true;
    if(e->type==SDL_EVENT_MOUSE_BUTTON_UP&&e->button.button==SDL_BUTTON_RIGHT&&right_back_held) {
        input_pc_escape(false);right_back_held=false;return true;
    }
    if (!enabled()) return false;
    if (!pc_cursor_active()) return false;
    if (e->type == SDL_EVENT_MOUSE_MOTION) { pointer_motion++;window_position(e->motion.x,e->motion.y);motion_at=plat_now_ns(); return true; }
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        wheel+=(e->wheel.y>0)-(e->wheel.y<0); motion_at=plat_now_ns();return true;
    }
    if ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        e->button.button == SDL_BUTTON_RIGHT) {
        right_back_held=e->type==SDL_EVENT_MOUSE_BUTTON_DOWN;
        input_pc_escape(e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return true;
    }
    if ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        e->button.button == SDL_BUTTON_LEFT) {
        held = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        window_position(e->button.x,e->button.y);
        motion_at=plat_now_ns();
        if (held) presses++; else releases++;
        return true;
    }
    return false;
}

// Entry hook, before the original prologue: preserve the game's group and provide its native pointer arguments.
void rt_pc_button_group(Cpu *c) {
    if (!pc_cursor_active()) return;
    sample_pointer();
    inventory_pointer(c);
    extern uint64_t renderer_present_count(void);
    if (getenv("SAVAGEWIND_PC_DUMP_UI") && renderer_present_count() >= 180) {
        static uint64_t seen[512]; static unsigned ns;
        uint64_t group=c->x[0]; unsigned i=0;
        while(i<ns && seen[i]!=group)i++;
        if(i==ns && ns<512) {
            seen[ns++]=group;
            uint64_t mgr=LD64(g_site.screen_mgr),screen=0;unsigned id=0;
            if(mgr) for(id=0;id<LD32(mgr+0x28)&&id<256;id++) {
                uint64_t a=LD64(mgr+0x30);screen=a?LD64(a+8*id):0;
                if(screen && LD64(screen+0x38)==group)break;
            }
            LOG("[pc-ui] group %llx screen %llx id %x flags %x\n",group,screen,id,LD32(group+0x38));
            uint64_t head=group+8,node=LD64(head);
            for(unsigned b=0;node!=head && node>=GUEST_LO+8 && node<GUEST_HI && b<256;b++) {
                uint64_t button=node-8,pane=LD64(button+0x58),cur=LD64(button+0x60);
                LOG("[pc-ui] button %llx flags %x hit %llx cursor %llx",button,LD16(button+0x3a),pane,cur);
                if(pane>=GUEST_LO && pane<GUEST_HI)LOG(" hit-rect %.1f %.1f size %.1f %.1f",*(float*)(pane+0x7c),*(float*)(pane+0x8c),*(float*)(pane+0x50),*(float*)(pane+0x54));
                if(cur>=GUEST_LO && cur<GUEST_HI)LOG(" cursor-rect %.1f %.1f size %.1f %.1f",*(float*)(cur+0x7c),*(float*)(cur+0x8c),*(float*)(cur+0x50),*(float*)(cur+0x54));
                LOG("\n");node=LD64(node);
            }
        }
    }
    // ButtonGroup's old hit areas are tiny markers in several menus; selection is handled at BoxCursorMgr below.
    if (getenv("SAVAGEWIND_PC_CURSOR_LOG") && (frame_press || plat_now_ns() >= next_log)) {
        next_log = plat_now_ns() + 2000000000;
        LOG("[pc-cursor] group %llx point %.1f %.1f press %d held %d release %d hover %llx\n",
            (unsigned long long)c->x[0], point[0], point[1], frame_press, frame_held, frame_release,
            *(unsigned long long *)(uintptr_t)(c->x[0] + 0x28));
    }
}

static uint64_t call(Cpu *c, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t x2) {
    Cpu saved=*c;
    c->x[0]=x0;c->x[1]=x1;c->x[2]=x2;
    guest_call(c,fn);
    uint64_t result=c->x[0];
    *c=saved;
    return result;
}
static bool ptr(uint64_t p) { return p>=GUEST_LO && p<GUEST_HI-0x100; }

int rt_pc_map_focus(Cpu *c, int position) {
    if(!pc_cursor_map_navigation() || map_zooming || plat_now_ns()<map_select_until)return 0;
    // Preserve the native marker candidate for magnetic targeting, but leave the
    // viewport alone. Explicit selection and zoom still use the original helper.
    if(position) {
        uint64_t data=c->x[0],p=c->x[1];
        call(c,0x7000d8c780,data,p,0);
        call(c,0x7000d8c6bc,data,p,0);
    }
    return 1;
}

// Options reads normal Npad edges after cursor selection; BoxCursorMgr's
// confirm action alone does not reach its value handlers. Keep each wheel step
// separated by a release, and do not let a stationary hover undo navigation.
void pc_cursor_options_tick(uint64_t screen) {
    if(!pc_cursor_active()||!screen||LD8(screen+0xfe)!=2)return;
    sample_pointer();
    if(options_screen_active!=screen){options_screen_active=screen;options_navigation=false;options_wheel_pending=0;}
    int scroll=atomic_exchange(&wheel,0);
    if(scroll) {
        options_wheel_pending+=scroll;
        if(options_wheel_pending>32)options_wheel_pending=32;
        if(options_wheel_pending< -32)options_wheel_pending=-32;
        options_navigation=true;options_navigation_motion=pointer_motion;
    }
    uint64_t now=plat_now_ns();
    if(options_wheel_pending&&now>=options_next_pulse) {
        bool up=options_wheel_pending>0;options_wheel_pending+=up?-1:1;
        options_buttons=1ull<<(up?13:15);options_buttons_until=now+50000000;
        options_next_pulse=now+100000000;
    }
}
static void options_click(Cpu *c,uint64_t screen,uint64_t row,unsigned kind,const float box[4]) {
    unsigned action=0;
    if(kind==0) {
        bool want=point[0]<(box[0]+box[2])*.5f;
        if(want==(LD8(row+0x54)!=0))return;
    } else if(kind==1) {
        float edge=fminf(36,(box[2]-box[0])*.2f);
        if(point[0]<box[0]+edge)action=1;
        else if(point[0]>box[2]-edge)action=2;
    }
    // The options rows are nested buttons, not the screen's cursor-node list.
    // Run the same row action and refresh as keyboard confirmation, once.
    uint64_t saved_action=LD32(screen+0x36c0);
    ST64(screen+0x36c8,row);ST32(screen+0x36c0,action);
    call(c,0x7000e30700,row,0,0);
    ST32(screen+0x36c0,saved_action);
    call(c,0x7000e2eed8,row,0,0);
    if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-options-click] row %llx kind %u action %u\n",row,kind,action);
}
static bool options_pointer(Cpu *c,uint64_t screens,uint64_t bounds) {
    if(g_site.opt_screen_rows!=0x36b0)return false; // inspected 1.9.0 row handlers
    uint64_t screen=LD64(screens+8*0x34);
    if(!ptr(screen)||LD8(screen+0xfe)!=2)return false;
    // The footer still uses the ordinary Back / Restore buttons. The list must
    // not follow hover, otherwise the pointer fights wheel/keyboard navigation.
    if(point[1]<-260||point[1]>230)return false;
    if(!click_pending)return true;
    click_pending=false;
    uint64_t rows=LD64(screen+g_site.opt_screen_rows+8),entries=LD64(g_site.opt_table+8);
    unsigned count=LD32(screen+g_site.opt_screen_rows),total=LD32(g_site.opt_table);
    if(!ptr(rows)||!ptr(entries))return true;
    for(unsigned i=0;i<count&&i<64;i++) {
        uint64_t row=LD64(rows+i*8);if(!ptr(row))continue;
        unsigned index=LD32(row+0x50);if(index>=total)continue;
        unsigned kind=LD32(entries+index*0x30+0x10);
        unsigned offset=kind==0?0x18:kind==1?0x20:kind==2?0x28:0;
        uint64_t button=offset?LD64(row+offset):0,pane=ptr(button)?LD64(button+0x60):0;
        if(!ptr(pane))continue;
        float box[4];call(c,bounds,(uintptr_t)box,pane,0);
        if(!isfinite(box[0])||!isfinite(box[1])||!isfinite(box[2])||!isfinite(box[3]))continue;
        if(point[0]>=box[0]&&point[0]<=box[2]&&point[1]>=box[1]&&point[1]<=box[3]) {
            options_click(c,screen,row,kind,box);return true;
        }
    }
    return true;
}

uint64_t pc_cursor_buttons(uint64_t b) {
    if(pc_cursor_active()&&plat_now_ns()<options_buttons_until)b|=options_buttons;
    if(quick_confirmed) {
        if(!(b&(1ull<<14)))quick_confirmed=false;
        b&=~(1ull<<14);
    }
    if(plat_now_ns()<confirm_until)b|=1;
    return b;
}
bool pc_cursor_hover_box(float box[4]) {
    if(!pc_cursor_active()||plat_now_ns()>hover_until)return false;
    uint64_t p[2]={hover_rect[0],hover_rect[1]};memcpy(box,p,16);return true;
}
static void set_hover(float x,float y,float half) {
    float b[4]={x-half,y-half,x+half,y+half};uint64_t p[2];memcpy(p,b,16);
    hover_rect[0]=p[0];hover_rect[1]=p[1];hover_until=plat_now_ns()+50000000;
}
static void inventory_shift(Cpu *c,uint64_t s,float dx) {
    Cpu saved=*c;c->x[0]=s;memcpy(&c->v[0],&dx,4);guest_call(c,0x7000e3e398);*c=saved;
}

static void inventory_pointer(Cpu *c) {
    if(LD32(g_site.pause_state)!=2)return;
    uint64_t mgr=LD64(g_site.screen_mgr),arr=ptr(mgr)?LD64(mgr+0x30):0;
    uint64_t s=ptr(arr)?LD64(arr+8*0x2e):0;
    if(!ptr(s)||LD64(s)!=0x7001cf9ad8ull||LD8(s+0xfe)!=2||LD64(s+0x38)!=c->x[0])return;
    uint64_t modal=LD64(arr+8*0x39);
    if(ptr(modal)&&LD8(modal+0xfe)==2)return;
    static uint64_t frame=UINT64_MAX;
    extern uint64_t renderer_present_count(void);
    uint64_t f=renderer_present_count();if(frame==f)return;frame=f;
    static int target=-1;
    int total=LD32(s+0x363c),current=LD32(s+0x365c),scroll=atomic_exchange(&wheel,0);
    if(getenv("SAVAGEWIND_PC_CURSOR_LOG")&&(scroll||f%120==0))LOG("[pc-inventory-state] frame %llu page %d selected-page %u ring %u selected %u scroll %d target %d\n",f,current,LD32(s+0x3660),LD8(s+0x3a30),LD32(s+0x3650),scroll,target);
    if(total<1||total>100)return;
    for(unsigned i=0;i<7;i++) {
        char name[32];snprintf(name,sizeof name,"Pa_CategoryS_%02u",i);
        uint64_t pane=call(c,0x7000f6f800,s,(uintptr_t)name,0);
        if(!ptr(pane))continue;
        float x=*(float*)(pane+0x7c),y=*(float*)(pane+0x8c);
        if(fabsf(point[0]-x)<28&&fabsf(point[1]-y)<28) {
            set_hover(x,y,25);
            if(click_pending) {
                int page=LD32(s+0x3708+0x80*i);
                if(page>=0&&page<total&&(unsigned)call(c,0x7000e71080,page,0,0)==i)target=page;
                click_pending=false;
                if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-category] %u page %d current %d\n",i,target,current);
            }
        }
    }
    if(scroll){target=current-(scroll>0?1:-1);if(target<0)target=0;if(target>=total)target=total-1;}
    for(unsigned i=0;i<2;i++) {
        const char *name=i?"Pa_ArrowR_00":"Pa_ArrowL_00";
        uint64_t pane=call(c,0x7000f6f800,s,(uintptr_t)name,0);
        if(ptr(pane)&&fabsf(point[0]-*(float*)(pane+0x7c))<24&&fabsf(point[1]-*(float*)(pane+0x8c))<36&&click_pending) {
            target=current+(i?1:-1);if(target<0)target=0;if(target>=total)target=total-1;
            click_pending=false;
        }
    }
    bool dragging=gesture_down&&gesture_dragged&&gesture_start[0]<70&&gesture_start[1]<155;
    if(dragging) {
        target=-1;click_pending=false;
        inventory_shift(c,s,gesture_delta[0]);
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG")&&gesture_delta[0])LOG("[pc-inventory-drag] %.1f page %u\n",gesture_delta[0],LD32(s+0x365c));
    } else if(target>=0&&target!=current) {
        inventory_shift(c,s,target>current?-500:500);
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-inventory-page] %d -> %u target %d\n",current,LD32(s+0x365c),target);
    } else target=-1;
    int now=LD32(s+0x365c);
    if(now!=current) {
        ST32(s+0x3660,now);
        call(c,0x7000e3b20c,s,now,0);
        call(c,0x7000e3b550,s,0,0);
        // Native drag paging swaps the three cached pages but does not enable
        // the new page's item buttons. The normal navigation state transition
        // does that separately; wheel/category/drag input must do it as well.
        unsigned ring=LD8(s+0x3a30);
        for(unsigned i=0;i<3;i++) {
            uint64_t page=LD64(s+0x3a18+8*i);
            if(ptr(page))call(c,0x7000dc7040,page,i==ring&&LD32(s+0x368c)==0,0);
        }
        call(c,0x7000e3e284,s,now,now==0?1:0);
    }
    if(!dragging&&(target<0||target==now)) {
        unsigned ring=LD8(s+0x3a30);
        uint64_t page=ring<3?LD64(s+0x3a18+8*ring):0,layout=ptr(page)?LD64(page+0x20):0;
        uint64_t pane=ptr(layout)?LD64(layout+0x18):0;
        if(ptr(pane)) {
            float dx=*(float*)(s+0x366c)-*(float*)(pane+0x30);
            if(isfinite(dx)&&fabsf(dx)>.25f&&fabsf(dx)<1000)inventory_shift(c,s,dx*.5f);
        }
    }
}

static bool native_special(Cpu *c,uint64_t screens,uint64_t bounds) {
    extern uint64_t renderer_present_count(void);
    uint64_t f=renderer_present_count();
    unsigned pause=LD32(g_site.pause_state);
    uint64_t quick=LD64(screens+8*0x2d),screen=LD64(screens+8*0x2a);
    bool is_quick=pause==4&&ptr(quick)&&LD64(quick)==0x7001cf51d8ull;
    bool inventory=ptr(LD64(screens+8*0x2f));
    bool is_map=pause==2&&!inventory&&ptr(screen)&&LD8(screen+0xfe)==2&&LD64(screen)==0x7001ce8298ull;
    if(!is_map&&!is_quick){map_down=false;map_anchor_active=map_picker_seen=false;map_canvas_active=false;map_view_valid=map_sent_valid=map_native_snapped=false;map_nav_ns=0;return false;}
    if(special_frame==f)return true;
    special_frame=f;
    uint64_t nav_now=plat_now_ns();
    float nav_dt=map_nav_ns&&nav_now>map_nav_ns?(float)(nav_now-map_nav_ns)/1e9f:0;
    map_nav_ns=nav_now;map_canvas_active=false;
    if(is_quick) {
        bool clicked=click_pending;click_pending=false;
        uint64_t list=LD64(quick+0x37a8);
        if(!ptr(list)||quick_confirmed)return true;
        int count=(int)LD32(quick+0x3880),hit=-1;
        if(count<1||count>20)return true;
        int selected=-1,total=(int)LD32(list+0x2ac);
        if(total<1||total>21)total=count+1;
        for(int i=0;i<total;i++)if(LD32(quick+0x3884+4*i)==LD32(quick+0x387c))selected=i;
        int scroll=atomic_exchange(&wheel,0);
        static float drag;
        if(frame_press)drag=0;
        if(gesture_down&&gesture_dragged) {
            drag+=gesture_delta[0];
            if(fabsf(drag)>=47){scroll=(int)roundf(drag/94);drag-=scroll*94;}
        }
        if(scroll&&selected>=0) {
            int next=selected-scroll;if(next<0)next=0;if(next>=total)next=total-1;
            if(next!=selected) {
                call(c,0x7000d6c54c,list,next,1);
                call(c,0x7000e2785c,quick,next,0);
                if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-quick-scroll] slot %d -> %d pending %d\n",selected,next,(int)LD32(quick+0x387c));
            }
        }
        for(int i=0;i<=20;i++) {
            uint64_t entry=call(c,0x7000d6fe80,list,i,0);
            uint64_t child=ptr(entry)?LD64(entry+0x10):0;
            uint64_t pane=ptr(child)?LD64(child+0x60):0;
            if(!ptr(pane)&&ptr(child))pane=LD64(child+0x58);
            if(!ptr(pane))continue;
            float x=*(float*)(pane+0x7c),y=*(float*)(pane+0x8c);
            if(getenv("SAVAGEWIND_PC_CURSOR_LOG")&&clicked)LOG("[pc-quick-rect] index %d name %.24s center %.1f %.1f\n",i,(char*)(pane+0xb8),x,y);
            if(isfinite(x)&&isfinite(y)&&fabsf(point[0]-x)<42&&fabsf(point[1]-y)<42){hit=i;set_hover(x,y,40);}
        }
        if(hit>=0&&clicked) {
            // Preserve the old selection: release of Right invokes the game's normal equip path using pending.
            call(c,0x7000e2785c,quick,hit,0);
            quick_confirmed=true;
            if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-quick-click] slot %d pending %d previous %d\n",hit,(int)LD32(quick+0x387c),(int)LD32(quick+0x3878));
        }
        return true;
    }
    // A modal over the map owns normal hover/confirmation.
    uint64_t modal=LD64(screens+8*0x39);
    if(ptr(modal)&&LD8(modal+0xfe)==2){map_down=false;map_anchor_active=map_picker_seen=false;map_view_valid=false;return false;}
    // Editing an existing stamp opens a separate ScreenAppSystemWindow (0x3e).
    // Its Edit/Delete/Cancel nodes must use normal dialog navigation, not canvas input.
    modal=LD64(screens+8*0x3e);
    if(ptr(modal)&&LD8(modal+0xfe)==2){map_down=false;map_anchor_active=map_picker_seen=false;map_view_valid=false;return false;}
    click_pending=false;
    uint64_t map=LD64(screen+0x3628),d=ptr(map)?LD64(map+0x8d8):0;
    if(!ptr(d))return true;
    // The stamp palette has nine square figure buttons (plus pin colors).
    // Map markers also use AnimButtons: one visible marker is not a palette.
    // Their bounds move into the viewport as the map zooms and pans.
    uint64_t group=LD64(screen+0x38),head=group+8,link=ptr(group)?LD64(head):0;
    unsigned palette_buttons=0;uint64_t hit_button=0;float hit_box[4];
    for(unsigned i=0;link!=head&&ptr(link)&&i<256;i++) {
        uint64_t b=link-8,pane=LD64(b+0x58);
        if((LD16(b+0x3a)&0x10)&&ptr(pane)) {
            float box[4];call(c,bounds,(uintptr_t)box,pane,0);
            float width=box[2]-box[0],height=box[3]-box[1];
            if(fabsf(width-64)<1&&fabsf(height-64)<1&&box[0]>=-640&&box[2]<=640&&box[1]>=-360&&box[3]<=360) {
                palette_buttons++;
                if(point[0]>=box[0]&&point[0]<=box[2]&&point[1]>=box[1]&&point[1]<=box[3]){hit_button=b;memcpy(hit_box,box,16);}
            }
        }
        link=LD64(link);
    }
    bool picker=palette_buttons>=9;
    if(picker&&!map_anchor_active) {
        map_anchor[0]=*(float*)(d+0x3868);map_anchor[1]=*(float*)(d+0x386c);map_anchor_active=true;
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-anchor] picker lock %.2f %.2f\n",map_anchor[0],map_anchor[1]);
    }
    if(picker)map_picker_seen=true;
    if(map_anchor_active&&(picker||(!map_picker_seen&&plat_now_ns()<map_anchor_deadline))) {
        map_sent_valid=map_native_snapped=false;
        // Choosing a figure owns the pointer but retains the original viewport.
        // Otherwise the first hover after closing the palette can capture a
        // partially recentered view from the game's controller navigation.
        if(!map_view_valid) {
            map_view_center[0]=*(float*)(d+0x880);map_view_center[1]=*(float*)(d+0x884);map_view_valid=true;
        }
        *(float*)(d+0x880)=*(float*)(d+0x890)=map_view_center[0];
        *(float*)(d+0x884)=*(float*)(d+0x894)=map_view_center[1];
        *(float*)(d+0x870)=map_view_center[0]*(*(float*)(d+0x878));
        *(float*)(d+0x874)=map_view_center[1]*(*(float*)(d+0x878));
        // The chosen world location belongs to the picker until it confirms or cancels.
        // Mouse movement in the palette must never be sent to the map canvas underneath it.
        call(c,0x7000d8c780,d,(uintptr_t)map_anchor,0);
        call(c,0x7000d8c6bc,d,(uintptr_t)map_anchor,0);
        atomic_exchange(&wheel,0);map_down=false;
        if(hit_button) {
            set_hover((hit_box[0]+hit_box[2])*.5f,(hit_box[1]+hit_box[3])*.5f,fminf(32,(hit_box[2]-hit_box[0])*.5f));
            if(frame_release&&!gesture_dragged) {
                call(c,LD64(LD64(hit_button)+0x130),hit_button,0,0);
                call(c,LD64(LD64(hit_button)+0x38),hit_button,0,0);
                if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-button] %llx point %.1f %.1f anchor %.2f %.2f\n",hit_button,point[0],point[1],map_anchor[0],map_anchor[1]);
            }
        }
        return true;
    }
    if(map_anchor_active) {
        map_anchor_active=map_picker_seen=false;
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-anchor] unlock\n");
    }
    float zoom=*(float*)(d+0x878);
    if(!isfinite(zoom)||zoom<=0)return true;
    bool area=fabsf(point[0])<600&&fabsf(point[1])<285;
    int scroll=atomic_exchange(&wheel,0);
    map_canvas_active=true;
    bool zooming=scroll||fabsf(zoom-*(float*)(d+0x888))>.001f;
    map_zooming=zooming;
    if(zooming)map_view_valid=false;
    else if(!map_view_valid) {
        map_view_center[0]=*(float*)(d+0x880);map_view_center[1]=*(float*)(d+0x884);map_view_valid=true;
    } else {
        // Native marker hover may request controller-style recentring. A PC hover
        // owns only the cursor: restore the viewport unless the player navigates.
        *(float*)(d+0x880)=*(float*)(d+0x890)=map_view_center[0];
        *(float*)(d+0x884)=*(float*)(d+0x894)=map_view_center[1];
        *(float*)(d+0x870)=map_view_center[0]*zoom;*(float*)(d+0x874)=map_view_center[1]*zoom;
    }
    if(area&&scroll) {
        call(c,scroll>0?0x7000dc24a4:0x7000dc25e8,map,0,0);
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-wheel] %d zoom level %u\n",scroll,LD32(d+0x898));
    }
    if(frame_press&&area) {
        map_down=true;map_dragged=false;
        memcpy(map_origin,point,sizeof point);memcpy(map_last,point,sizeof point);
    }
    if(map_down&&frame_held) {
        if(hypotf(point[0]-map_origin[0],point[1]-map_origin[1])>5)map_dragged=true;
        if(map_dragged) {
            float dx=(point[0]-map_last[0])/zoom,dy=(point[1]-map_last[1])/zoom;
            *(float*)(d+0x880)=fmaxf(*(float*)(d+0x8d0),fminf(*(float*)(d+0x8d8),*(float*)(d+0x880)+dx));
            *(float*)(d+0x884)=fmaxf(*(float*)(d+0x8d4),fminf(*(float*)(d+0x8dc),*(float*)(d+0x884)+dy));
            *(float*)(d+0x890)=*(float*)(d+0x880);*(float*)(d+0x894)=*(float*)(d+0x884);
        }
        memcpy(map_last,point,sizeof point);
    }
    float cursor[2]={point[0],point[1]},best_snap=1e9f;
    uint64_t snapped=0;
    // Native map markers need not be registered in the screen's AnimButton list.
    // A target changed from the point we sent last frame is the game's marker
    // candidate, not a delayed echo of the physical pointer.
    bool native_snap=false;
    if(map_sent_valid&&!frame_held&&!zooming&&area) {
        float wx=*(float*)(d+0x3870),wy=*(float*)(d+0x3874);
        float changed=hypotf(wx-map_sent_world[0],wy-map_sent_world[1])*zoom;
        float x=wx*zoom+*(float*)(d+0x870),y=wy*zoom+*(float*)(d+0x874);
        float distance=hypotf(x-point[0],y-point[1]);
        if((changed>.1f||map_native_snapped)&&distance<(map_native_snapped?14:10)) {
            cursor[0]=x;cursor[1]=y;best_snap=distance;native_snap=true;
        }
    }
    link=ptr(group)?LD64(head):0;
    if(!frame_held&&!zooming&&area)for(unsigned i=0;link!=head&&ptr(link)&&i<256;i++) {
        uint64_t button=link-8,pane=LD64(button+0x58);
        if((LD16(button+0x3a)&0x10)&&ptr(pane)) {
            float box[4];call(c,bounds,(uintptr_t)box,pane,0);
            float w=box[2]-box[0],h=box[3]-box[1];
            float x=(box[0]+box[2])*.5f,y=(box[1]+box[3])*.5f;
            float distance=hypotf(point[0]-x,point[1]-y),radius=button==map_snap_button?14:10;
            if(w>=10&&w<=80&&h>=10&&h<=80&&fabsf(x)<600&&fabsf(y)<285&&distance<radius&&distance<best_snap) {
                best_snap=distance;snapped=button;cursor[0]=x;cursor[1]=y;
            }
        }
        link=LD64(link);
    }
    if(snapped!=map_snap_button&&snapped&&getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-snap] frame %llu point %.1f %.1f cursor %.1f %.1f radius %.1f\n",f,point[0],point[1],cursor[0],cursor[1],best_snap);
    map_snap_button=snapped;
    if(native_snap&&!map_native_snapped&&getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-snap-native] frame %llu point %.1f %.1f cursor %.1f %.1f distance %.1f\n",f,point[0],point[1],cursor[0],cursor[1],best_snap);
    map_native_snapped=native_snap;
    float keys[2],pan[2];extern void input_pc_map_axes(float out[2]);input_pc_map_axes(keys);
    pc_map_navigation(point,keys[0],keys[1],!frame_held&&!snapped&&!native_snap&&(pointer_test()||position_inside),nav_dt,zoom,pan);
    if(!frame_held&&(pan[0]||pan[1])) {
        *(float*)(d+0x880)=fmaxf(*(float*)(d+0x8d0),fminf(*(float*)(d+0x8d8),*(float*)(d+0x880)+pan[0]));
        *(float*)(d+0x884)=fmaxf(*(float*)(d+0x8d4),fminf(*(float*)(d+0x8dc),*(float*)(d+0x884)+pan[1]));
        *(float*)(d+0x890)=*(float*)(d+0x880);*(float*)(d+0x894)=*(float*)(d+0x884);
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG")&&f%30==0)LOG("[pc-map-pan] frame %llu keys %.0f %.0f delta %.2f %.2f center %.2f %.2f\n",f,keys[0],keys[1],pan[0],pan[1],*(float*)(d+0x880),*(float*)(d+0x884));
    }
    if(map_view_valid) {map_view_center[0]=*(float*)(d+0x880);map_view_center[1]=*(float*)(d+0x884);}
    if((!frame_held&&(pan[0]||pan[1]))||(map_down&&frame_held&&map_dragged)) {
        *(float*)(d+0x870)=*(float*)(d+0x880)*zoom;*(float*)(d+0x874)=*(float*)(d+0x884)*zoom;
    }
    if(area) {
        float p[2],translation[2]={*(float*)(d+0x870),*(float*)(d+0x874)};
        pc_cursor_map_world(cursor,translation,zoom,p);
        memcpy(map_sent_world,p,sizeof p);map_sent_valid=true;
        call(c,0x7000d8c780,d,(uintptr_t)p,0);
        call(c,0x7000d8c6bc,d,(uintptr_t)p,0);
        if(getenv("SAVAGEWIND_PC_TRACK_LOG")&&f%10==0) {
            uint64_t pane=call(c,0x7000f6f800,screen,(uintptr_t)"Pa_Cursor_00",0);
            if(ptr(pane))LOG("[pc-map-track] frame %llu input %.2f %.2f drawn %.2f %.2f zoom %.3f\n",f,point[0],point[1],*(float*)(pane+0x7c),*(float*)(pane+0x8c),zoom);
        }
    }
    if(map_down&&frame_release) {
        if(!map_dragged&&area) {
            confirm_until=plat_now_ns()+80000000;
            map_select_until=plat_now_ns()+160000000;
            map_anchor[0]=*(float*)(d+0x3868);map_anchor[1]=*(float*)(d+0x386c);
            map_anchor_active=true;map_picker_seen=false;map_anchor_deadline=plat_now_ns()+400000000;
            if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-anchor] click lock %.2f %.2f\n",map_anchor[0],map_anchor[1]);
        }
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-map-release] drag %d select %d point %.1f %.1f\n",map_dragged,!map_dragged&&area,point[0],point[1]);
        map_down=false;
    }
    return true;
}

static void special_diagnostics(Cpu *c,uint64_t screens) {
    if(!getenv("SAVAGEWIND_PC_DUMP_UI"))return;
    static uint64_t next;
    if(plat_now_ns()<next)return;
    next=plat_now_ns()+1000000000;
    uint64_t s=LD64(screens+8*0x2a);
    if(ptr(s)&&LD8(s+0xfe)==2&&LD64(s)==0x7001ce8298ull) {
        uint64_t map=LD64(s+0x3628),d=ptr(map)?LD64(map+0x8d8):0;
        if(ptr(d))LOG("[pc-map-data] map %llx data %llx trans %.2f %.2f zoom %.3f target %.3f center %.2f %.2f ctarget %.2f %.2f cursor %.2f %.2f target %.2f %.2f state %u\n",map,d,*(float*)(d+0x870),*(float*)(d+0x874),*(float*)(d+0x878),*(float*)(d+0x888),*(float*)(d+0x880),*(float*)(d+0x884),*(float*)(d+0x890),*(float*)(d+0x894),*(float*)(d+0x3868),*(float*)(d+0x386c),*(float*)(d+0x3870),*(float*)(d+0x3874),LD32(map+0xb350));
    }
    if(LD32(g_site.pause_state)!=4)return;
    s=LD64(screens+8*0x2d);
    if(!ptr(s)||LD64(s)!=0x7001cf51d8ull)return;
    uint64_t list=LD64(s+0x37a8);
    LOG("[pc-quick-data] count %u kind %u selected %d pending %d list %llx\n",LD32(s+0x3880),LD32(s+0x3738),(int)LD32(s+0x3878),(int)LD32(s+0x387c),list);
    const char *names[]={"N_ShortCutPos_00","N_ShortCutNow_00","N_ChangeLR_00"};
    for(unsigned i=0;i<3;i++) {
        uint64_t p=call(c,0x7000f6f800,s,(uintptr_t)names[i],0);
        if(ptr(p))LOG("[pc-quick-pane] %s %llx pos %.2f %.2f scale %.3f %.3f\n",names[i],p,*(float*)(p+0x7c),*(float*)(p+0x8c),*(float*)(p+0x70),*(float*)(p+0x84));
    }
    if(ptr(list))for(unsigned i=0;i<10;i++) {
        uint64_t item=call(c,0x7000d6fe80,list,i,0);
        if(ptr(item)) {
            uint64_t p=LD64(item+0x10);
            LOG("[pc-quick-item] %u entry %llx ptr %llx fields %llx %llx %llx pos %.2f %.2f\n",i,item,p,ptr(p)?LD64(p+0x10):0,ptr(p)?LD64(p+0x18):0,ptr(p)?LD64(p+0x20):0,*(float*)(item+0x50),*(float*)(item+0x54));
        }
    }
}

void rt_pc_cursor_manager(Cpu *c, uint64_t activate, uint64_t bounds) {
    // Restore only our one-call confirm pulse. Some screens retain the last action while input is idle.
    if(injected_manager) {
        if(ptr(injected_manager)&&LD32(injected_manager+0x10)==1)ST32(injected_manager+0x10,saved_action);
        injected_manager=0;
    }
    if (!pc_cursor_active()) return;
    sample_pointer();
    if (!pointer_test() && !position_inside) {
        float keys[2];extern void input_pc_map_axes(float out[2]);input_pc_map_axes(keys);
        if(keys[0]==0&&keys[1]==0)return;
    }
    uint64_t manager=c->x[0];unsigned target=(unsigned)c->x[1];
    if(target>1)return;
    uint64_t control=LD64(manager+0x18+8*target);
    uint64_t screen_mgr=LD64(g_site.screen_mgr);
    if(!ptr(control)||!ptr(screen_mgr))return;
    uint64_t screens=LD64(screen_mgr+0x30),n=LD32(screen_mgr+0x28);
    if(n>0x2d&&ptr(screens))special_diagnostics(c,screens);
    if(n>0x34&&ptr(screens)&&options_pointer(c,screens,bounds))return;
    if(n>0x39&&ptr(screens)&&native_special(c,screens,bounds))return;
    if(!click_pending&&!pointer_test()&&plat_now_ns()-motion_at>300000000)return;
    uint64_t hit=0,hit_screen=0;float best=1e30f;
    extern uint64_t renderer_present_count(void);
    static bool dumped_screens[256];
    for(unsigned id=0;id<n&&id<256&&ptr(screens);id++) {
        uint64_t screen=LD64(screens+8*id);
        if(!ptr(screen)||LD8(screen+0xfe)!=2)continue;
        if(id==0x34&&options_navigation&&!click_pending&&options_navigation_motion==pointer_motion)continue;
        bool dump=getenv("SAVAGEWIND_PC_DUMP_UI")&&!dumped_screens[id]&&renderer_present_count()>300;
        uint64_t link_offset=(int32_t)LD32(screen+0x9c);
        uint64_t head=screen+0x88,link=LD64(head+8);
        for(unsigned i=0;link!=head&&ptr(link)&&i<256;i++) {
            uint64_t node=link-link_offset;
            if(!ptr(node))break;
            uint64_t button=LD64(node+0x18),vtable=LD64(node);
            if(!ptr(button)||!ptr(vtable))break;
            {
                uint64_t pane=LD64(button+0x60);
                if(ptr(pane)) {
                    float box[4];
                    call(c,bounds,(uintptr_t)box,pane,0);
                    call(c,LD64(LD64(screen)+0x38),screen,(uintptr_t)box,node);
                    if(dump)LOG("[pc-node] screen %x node %llx button %llx rect %.1f %.1f %.1f %.1f\n",id,node,button,box[0],box[1],box[2],box[3]);
                    uint64_t layout=LD64(button+0x20),parent=ptr(layout)?LD64(layout+0x18):0;
                    if(dump&&ptr(layout)) {
                        LOG("[pc-layout] %llx fields %llx %llx %llx %llx\n",layout,LD64(layout+8),LD64(layout+0x10),parent,LD64(layout+0x20));
                        if(ptr(parent)) LOG("[pc-parent] name %.24s pos %.1f %.1f size %.1f %.1f\n",(char*)(parent+0xb8),*(float*)(parent+0x7c),*(float*)(parent+0x8c),*(float*)(parent+0x50),*(float*)(parent+0x54));
                    }
                    // Title buttons use a 20x20 arrow as cursor geometry. The text occupies the rest of its row.
                    if(id==1 && box[2]-box[0]<32 && ptr(parent)) {
                        float x=*(float*)(parent+0x7c),y=*(float*)(parent+0x8c);
                        box[0]=x-25;box[2]=640;box[1]=y-18;box[3]=y+18;
                    }
                    if(isfinite(box[0])&&isfinite(box[1])&&isfinite(box[2])&&isfinite(box[3])&&
                        point[0]>=box[0]&&point[0]<=box[2]&&point[1]>=box[1]&&point[1]<=box[3]&&
                        (call(c,LD64(vtable+0x28),node,target,0)&1)) {
                        float dx=point[0]-(box[0]+box[2])*.5f,dy=point[1]-(box[1]+box[3])*.5f;
                        float score=dx*dx+dy*dy;
                        if(score<best){best=score;hit=node;hit_screen=screen;}
                    }
                }
            }
            link=LD64(link+8);
        }
        if(dump)dumped_screens[id]=true;
    }
    if(!hit)return;
    // The PC hover is drawn this frame; the game's own highlight may still be playing its transition.
    if(hit&&best<1e29f) {
        uint64_t button=LD64(hit+0x18),pane=LD64(button+0x60);
        if(ptr(pane)&&*(float*)(pane+0x50)>40&&*(float*)(pane+0x50)<150)
            set_hover(*(float*)(pane+0x7c),*(float*)(pane+0x8c),*(float*)(pane+0x50)*.5f);
    }
    if(LD64(control+0x48)!=hit) {
        call(c,activate,control,hit,0);
        ST8(hit_screen+0x106,1);
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-hover] node %llx point %.1f %.1f\n",hit,point[0],point[1]);
    }
    if(click_pending) {
        injected_manager=manager;saved_action=LD32(manager+0x10);
        ST32(manager+0x10,1); // The exact action used by the game's confirm button, processed by its own code next.
        click_pending=false;
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-click] confirm node %llx\n",hit);
    }
}

RT_ON_RESET(pc_cursor_reset) {
    options_buttons=options_buttons_until=0;options_wheel_pending=0;pointer_motion=0;
    options_navigation_motion=0;options_navigation=false;options_screen_active=options_next_pulse=0;
    active = false; held = false; frame_press = frame_release = frame_held = false;
    click_pending=false;
    injected_manager=0;
    quick_confirmed=false;confirm_until=hover_until=0;wheel=0;map_down=false;special_frame=UINT64_MAX;
    sampled_frame=UINT64_MAX;gesture_down=gesture_dragged=false;
    map_anchor_active=map_picker_seen=false;map_anchor_deadline=0;
    map_canvas_active=false;map_nav_ns=map_snap_button=0;map_view_valid=false;
    map_sent_valid=map_native_snapped=false;
    presses = releases = last_press = last_release = 0;
}
