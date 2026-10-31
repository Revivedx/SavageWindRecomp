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
static _Atomic bool dialogue_active;
static _Atomic uint64_t quantity_until;
static _Atomic int quantity_value,quantity_max,quantity_request=-1;
static _Atomic bool quantity_edit,quantity_replace;
static uint64_t quantity_screen;
static uint64_t shop_screen;
static bool ptr(uint64_t p);
static bool preview_defaults_saved;
static float preview_defaults[2];
static int quantity_hold;
static uint64_t quantity_repeat_at;
static uint64_t sound_hover_key,sound_hover_until;
static void pointer_sound(Cpu *c,uint64_t screen,const char *cue);
static void pointer_hover_sound(Cpu *c,uint64_t screen,uint64_t key);

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
static uint64_t hover_control,hover_screen,hover_node;
static void hover_leave(void){hover_control=hover_screen=hover_node=0;}
static uint64_t options_screen_active,options_rows_active;
static bool options_scroll_valid;
static float options_scroll;
static bool options_keyboard;
static float options_keyboard_point[2];
static _Atomic bool pointer_keyboard_input;
static _Atomic uint64_t pointer_mouse_events;
static uint64_t options_keyboard_mouse_events;
static bool options_page_pending;
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
bool pc_cursor_hide_system(void) {
    return position_inside&&(pc_cursor_map_navigation()||(pc_cursor_active()&&pointer_keyboard_input));
}

void pc_cursor_frame(void) {
    bool dialogue=false;shop_screen=0;
    if(g_site.screen_mgr) {
        uint64_t mgr=LD64(g_site.screen_mgr),arr=mgr?LD64(mgr+0x30):0;
        unsigned count=mgr?LD32(mgr+0x28):0;
        // MessageDialog owns both NPC text and responses. Screen indices are
        // not stable identifiers (AkashNum is the spirit-orb counter).
        if(arr)for(unsigned i=0;i<count&&i<256;i++) {
            uint64_t text=LD64(arr+8*i);
            if(!text||LD8(text+0xfe)!=2)continue;
            if(LD64(text)==0x7001d4ed48ull)dialogue=true;
            uint64_t name=LD64(text+0xd0);
            if(name>=GUEST_LO&&name<GUEST_HI-32&&!strncmp((char*)(uintptr_t)name,"ShopBtnList",11))shop_screen=text;
        }
    }
    if(preview_defaults_saved) {
        uint64_t m=LD64(g_site.screen_mgr),a=ptr(m)?LD64(m+0x30):0;
        uint64_t inv=ptr(a)?LD64(a+8*0x2e):0;
        if(!enabled()||!g_site.pause_state||LD32(g_site.pause_state)!=2||!ptr(inv)||LD32(inv+0x3a48)!=0) {
            memcpy((void*)(uintptr_t)0x7001e15fa8ull,preview_defaults,sizeof preview_defaults);
            preview_defaults_saved=false;
        }
    }
    if(dialogue_active&&!dialogue)confirm_until=0;
    dialogue_active=dialogue;
    bool ui = g_site.pause_state &&
        (dialogue || shop_screen || framepace_menu_ui() || framepace_title_ui() || *(uint32_t *)(uintptr_t)g_site.pause_state == 4);
    active = enabled() && ui;
    if(!pc_cursor_active()){
        hover_leave();
        options_scroll_valid=false;options_screen_active=options_rows_active=0;
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
    if(!w)return;
    if(!enabled()){SDL_SetWindowMouseRect(w,NULL);return;}
    int width, height;
    float x, y;
    SDL_GetWindowSize(w, &width, &height);
    window_width=width;window_height=height;
    if(pc_cursor_map_navigation()&&focused) {
        float scale=fminf(width/1280.f,height/720.f);
        SDL_Rect rect={(int)(width*.5f-632*scale),(int)(height*.5f-280*scale),(int)(1264*scale),(int)(560*scale)};
        const SDL_Rect *old=SDL_GetWindowMouseRect(w);
        if(!old||memcmp(old,&rect,sizeof rect))SDL_SetWindowMouseRect(w,&rect);
    } else if(SDL_GetWindowMouseRect(w))SDL_SetWindowMouseRect(w,NULL);
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
    if(quantity_edit&&plat_now_ns()<quantity_until&&(e->type==SDL_EVENT_KEY_DOWN||e->type==SDL_EVENT_KEY_UP)) {
        if(e->type==SDL_EVENT_KEY_UP)return true;
        SDL_Scancode sc=e->key.scancode;int digit=-1;
        if(sc>=SDL_SCANCODE_1&&sc<=SDL_SCANCODE_9)digit=sc-SDL_SCANCODE_1+1;
        if(sc==SDL_SCANCODE_0||sc==SDL_SCANCODE_KP_0)digit=0;
        if(sc>=SDL_SCANCODE_KP_1&&sc<=SDL_SCANCODE_KP_9)digit=sc-SDL_SCANCODE_KP_1+1;
        int value=quantity_value;
        if(digit>=0){value=(quantity_replace?0:value)*10+digit;quantity_replace=false;}
        else if(sc==SDL_SCANCODE_BACKSPACE){value/=10;quantity_replace=false;}
        else if(sc==SDL_SCANCODE_RETURN||sc==SDL_SCANCODE_KP_ENTER||sc==SDL_SCANCODE_ESCAPE){quantity_edit=false;return true;}
        else return true;
        if(value>quantity_max)value=quantity_max;
        if(value<1)value=1;
        quantity_value=value;quantity_request=value;return true;
    }
    // Native Options captures game buttons, so input_buttons_now() may be
    // zero even while a navigation key is down. Track the real SDL source.
    if(e->type==SDL_EVENT_KEY_DOWN||e->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN||
       (e->type==SDL_EVENT_GAMEPAD_AXIS_MOTION&&
        e->gaxis.axis<=SDL_GAMEPAD_AXIS_RIGHTY&&abs(e->gaxis.value)>16000)) {
        pointer_keyboard_input=true;hover_until=0;
    }
    if((e->type==SDL_EVENT_MOUSE_MOTION&&(e->motion.xrel!=0||e->motion.yrel!=0))||
       e->type==SDL_EVENT_MOUSE_WHEEL||e->type==SDL_EVENT_MOUSE_BUTTON_DOWN) {
        pointer_keyboard_input=false;pointer_mouse_events++;
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) { window_position(e->motion.x,e->motion.y);motion_at=plat_now_ns(); return true; }
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
static void pointer_sound(Cpu *c,uint64_t screen,const char *cue) {
    uint64_t ref=ptr(screen)?LD64(screen+0xf0):0,emitter=ptr(ref)?LD64(ref+8):0;
    if(!ptr(emitter)||!ptr(LD64(emitter+0x30)))return;
    uint64_t handle[2]={0,0};
    // One-shot cues are owned by the native sound manager. This handle is
    // non-owning; stopping it here would cut the sound off immediately.
    call(c,0x7000f50e80,emitter,(uintptr_t)cue,(uintptr_t)handle);
}
static void pointer_hover_sound(Cpu *c,uint64_t screen,uint64_t key) {
    uint64_t now=plat_now_ns();
    if(sound_hover_key!=key||now>sound_hover_until)pointer_sound(c,screen,"mc_CursorMove");
    sound_hover_key=key;sound_hover_until=now+100000000;
}


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

extern int rt_opt_pointer_kind(uint64_t row);
static void options_click(Cpu *c,uint64_t screen,uint64_t row,unsigned kind,const float box[4]);
static bool options_box(uint64_t pane,float box[4]) {
    if(!ptr(pane))return false;
    float w=*(float*)(pane+0x50),h=*(float*)(pane+0x54);
    float x=*(float*)(pane+0x7c),y=*(float*)(pane+0x8c);
    float hw=(fabsf(*(float*)(pane+0x70))*w+fabsf(*(float*)(pane+0x74))*h)*.5f;
    float hh=(fabsf(*(float*)(pane+0x80))*w+fabsf(*(float*)(pane+0x84))*h)*.5f;
    box[0]=x-hw;box[1]=y-hh;box[2]=x+hw;box[3]=y+hh;
    return isfinite(x)&&isfinite(y)&&isfinite(hw)&&isfinite(hh)&&hw>0&&hh>0;
}
// Navigation keys own focus until the pointer moves again. A stationary mouse
// must not pull keyboard/controller selection back to its previous hover row.
static bool options_mouse_owns_focus(bool wheel_input) {
    extern uint64_t input_buttons_now(void);
    if(pointer_keyboard_input||(input_buttons_now()&((15ull<<12)|(15ull<<16)))) {
        options_keyboard=true;options_keyboard_mouse_events=pointer_mouse_events;memcpy(options_keyboard_point,point,sizeof(point));
        options_scroll_valid=false;hover_until=0;return false;
    }
    if(options_keyboard) {
        if(options_keyboard_mouse_events==pointer_mouse_events&&!wheel_input&&!click_pending&&fabsf(point[0]-options_keyboard_point[0])<.5f&&fabsf(point[1]-options_keyboard_point[1])<.5f)return false;
        options_keyboard=false;
    }
    return true;
}
static uint64_t options_row_at(uint64_t screen,float box[4],unsigned *kind_out) {
    if((!position_inside&&!pointer_test())||point[1]<-260||point[1]>230)return 0;
    uint64_t rows=LD64(screen+0x36b8);unsigned count=LD32(screen+0x36b0);
    if(!ptr(rows)||!count||count>64)return 0;
    for(unsigned i=0;i<count;i++) {
        uint64_t row=LD64(rows+i*8);if(!ptr(row))continue;
        int kind=rt_opt_pointer_kind(row);if(kind<0||kind>2)continue;
        uint64_t button=LD64(row+(kind==0?0x18:kind==1?0x20:0x28));
        if(!ptr(button)||!options_box(LD64(button+0x60),box))continue;
        if(point[0]>=-280&&point[0]<=box[2]&&point[1]>=box[1]&&point[1]<=box[3]) {*kind_out=(unsigned)kind;return row;}
    }
    return 0;
}
static void options_focus(Cpu *c,uint64_t screen,uint64_t row,unsigned kind);
// Options owns a native scroll view. Wheel input changes its view offset;
// it never synthesizes d-pad presses or walks the selection through rows.
void pc_cursor_options_page_changed(void) {
    // A page rebuild can reuse the exact same screen and row-array addresses.
    // Do not carry the previous page's locked offset into the rebuilt list.
    options_page_pending=true;options_scroll_valid=false;
    options_screen_active=options_rows_active=0;
}
void pc_cursor_options_tick(uint64_t screen) {
    if(!pc_cursor_active()||!ptr(screen)||LD8(screen+0xfe)!=2||g_site.opt_screen_rows!=0x36b0)return;
    sample_pointer();
    // Options uses one native highlight for both input methods, never a second overlay.
    hover_until=0;
    uint64_t rows=LD64(screen+g_site.opt_screen_rows+8),view=screen+0x3628;
    unsigned count=LD32(screen+g_site.opt_screen_rows);
    if(!ptr(rows)||!count||count>64)return;
    if(options_screen_active!=screen||options_rows_active!=rows) {
        options_screen_active=screen;options_rows_active=rows;options_scroll_valid=false;
    }
    bool page_changed=options_page_pending;options_page_pending=false;
    if(page_changed) {
        *(float*)(view+0x2c)=*(float*)(view+0x30)=0;
        options_scroll=0;options_scroll_valid=false;
        Cpu*c=kernel_current_cpu();if(c){Cpu saved=*c;c->x[0]=view;float ratio=1;memcpy(&c->v[0],&ratio,4);guest_call(c,0x7000d671d4);*c=saved;}
    }
    int scroll=atomic_exchange(&wheel,0);
    if(!options_mouse_owns_focus(scroll!=0))return;
    float current=*(float*)(view+0x2c),height=*(float*)(view+0x48)-*(float*)(view+0x40);
    float content=*(float*)(view+0x28),maximum=fmaxf(0,content-height);
    if(isfinite(current)&&isfinite(maximum)&&maximum<10000) {
        if(scroll) {options_scroll=fmaxf(0,fminf(maximum,current-scroll*50.f));options_scroll_valid=true;}
        // Hover changes the highlighted row, never the view offset. Explicit
        // keyboard/controller navigation can take ownership of scrolling again.
        if(options_scroll_valid) {
            *(float*)(view+0x2c)=*(float*)(view+0x30)=options_scroll;
            if(scroll) {
                Cpu *c=kernel_current_cpu();if(c){Cpu saved=*c;c->x[0]=view;float ratio=1;memcpy(&c->v[0],&ratio,4);guest_call(c,0x7000d671d4);*c=saved;}
            }
        }
    }
    float box[4];unsigned kind;
    uint64_t row=options_row_at(screen,box,&kind);
    if(row) {
        if(!options_scroll_valid&&isfinite(current)){options_scroll=current;options_scroll_valid=true;}
        Cpu *focus_cpu=kernel_current_cpu();if(focus_cpu)options_focus(focus_cpu,screen,row,kind);
        if(click_pending) {click_pending=false;Cpu*c=kernel_current_cpu();if(c)options_click(c,screen,row,kind,box);}
    }
}

static void options_click(Cpu *c,uint64_t screen,uint64_t row,unsigned kind,const float box[4]) {
    unsigned action=0;
    if(kind==0) {
        bool want=point[0]<box[0] ? LD8(row+0x54)==0 : point[0]<(box[0]+box[2])*.5f;
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
static void options_focus(Cpu *c,uint64_t screen,uint64_t row,unsigned kind) {
    // Screen's cursor type (+fc) is translated by its UI controller; it is
    // not the BoxCursorMgr array index. Resolve the actual active control.
    uint64_t ui=LD64(screen+0x28);if(!ptr(ui))return;
    uint64_t manager=LD64(ui+0xb18);if(!ptr(manager))return;
    unsigned target=(unsigned)call(c,0x7000f6e2c8,screen,0,0);if(target>1)return;
    uint64_t control=LD64(manager+0x18+8*target);if(!ptr(control))return;
    uint64_t button=LD64(row+(kind==0?0x18:kind==1?0x20:0x28));
    int32_t offset=(int32_t)LD32(screen+0x9c);
    uint64_t head=screen+0x88,link=LD64(head+8);
    for(unsigned i=0;i<256&&link!=head&&ptr(link);i++) {
        uint64_t node=link-offset;if(!ptr(node))break;
        if(LD64(node+0x18)==button&&LD64(node+0x20)==screen) {
            if(LD64(control+0x48)!=node||LD64(screen+0xe0)!=node||LD64(screen+0x36c8)!=row) {
                uint64_t view=screen+0x3628;float current=*(float*)(view+0x2c);
                // Activation changes the shared highlight and calls the native
                // Options help callback. No independent PC rectangle is drawn.
                call(c,0x7000f57da8,control,node,0);
                if(LD64(screen+0x36c8)!=row)call(c,0x7000e3047c,screen,button,0);
                ST8(screen+0x106,1);
                if(isfinite(current)) {
                    options_scroll=current;options_scroll_valid=true;
                    *(float*)(view+0x2c)=*(float*)(view+0x30)=current;
                }
            }
            break;
        }
        link=LD64(link+8);
    }
}
static bool options_pointer(Cpu *c,uint64_t screens,uint64_t bounds) {
    (void)c;(void)bounds;
    uint64_t screen=LD64(screens+8*0x34);
    // Options tick handles both hover focus and direct row actions.
    return ptr(screen)&&LD8(screen+0xfe)==2&&point[1]>=-260&&point[1]<=230;
}

uint64_t pc_cursor_buttons(uint64_t b) {
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
bool pc_cursor_hover_pressed(void) { return pc_cursor_active()&&held; }
static void set_hover_rect(const float box[4]) {
    uint64_t p[2];memcpy(p,box,16);
    hover_rect[0]=p[0];hover_rect[1]=p[1];hover_until=plat_now_ns()+50000000;
}
static void set_hover(float x,float y,float half) {
    float b[4]={x-half,y-half,x+half,y+half};set_hover_rect(b);
}
static void inventory_shift(Cpu *c,uint64_t s,float dx) {
    Cpu saved=*c;c->x[0]=s;memcpy(&c->v[0],&dx,4);guest_call(c,0x7000e3e398);*c=saved;
}

// Change only the native preview yaw target. Do not rotate translation or
// enter Rotate mode: that mode also hides the inventory information panes.
static void inventory_rotate(Cpu *c,float dx) {
    (void)c;
    uint64_t owner=LD64(0x7001e15fb8ull),actor=ptr(owner)?LD64(owner+0x48):0;
    if(!ptr(actor)||!isfinite(dx)||dx==0)return;
    float *yaw=(float*)(uintptr_t)0x7001e15fa8ull;
    if(!isfinite(yaw[0])||!isfinite(yaw[1]))return;
    if(!preview_defaults_saved) {
        memcpy(preview_defaults,yaw,sizeof preview_defaults);preview_defaults_saved=true;
    }
    for(unsigned i=0;i<2;i++)yaw[i]=remainderf(yaw[i]+dx*.46f,360.f);
}

static void inventory_pointer(Cpu *c) {
    if(LD32(g_site.pause_state)!=2)return;
    uint64_t mgr=LD64(g_site.screen_mgr),arr=ptr(mgr)?LD64(mgr+0x30):0;
    uint64_t s=ptr(arr)?LD64(arr+8*0x2e):0;
    if(!ptr(s)||LD64(s)!=0x7001cf9ad8ull||LD8(s+0xfe)!=2||LD32(s+0x3a48)!=0||LD64(s+0x38)!=c->x[0]||pointer_keyboard_input)return;
    uint64_t modal=LD64(arr+8*0x39);
    if(ptr(modal)&&LD8(modal+0xfe)==2)return;
    static uint64_t frame=UINT64_MAX;
    extern uint64_t renderer_present_count(void);
    uint64_t f=renderer_present_count();if(frame==f)return;frame=f;
    static int target=-1;
    // A gesture keeps its original owner until release, even when it crosses
    // the item grid. Do not let pending page motion or settling consume it.
    bool preview=gesture_down&&gesture_start[0]>=70&&gesture_start[1]<155&&gesture_start[1]>-260;
    if(preview) {
        target=-1;click_pending=false;hover_until=0;atomic_exchange(&wheel,0);
        if(gesture_dragged)inventory_rotate(c,gesture_delta[0]);
        return;
    }
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
            pointer_hover_sound(c,s,s+0x200+i);
            if(click_pending) {
                pointer_sound(c,s,"mc_Decide");
                int page=LD32(s+0x3708+0x80*i);
                if(page>=0&&page<total&&(unsigned)call(c,0x7000e71080,page,0,0)==i&&page!=current) {
                    // Refill the existing three-page ring directly. A global
                    // inventory refresh also restores unrelated pouch state.
                    unsigned ring=LD8(s+0x3a30);
                    if(ring<3) {
                        ST32(s+0x365c,page);ST32(s+0x3660,page);
                        for(int d=-1;d<=1;d++)if(page+d>=0&&page+d<total)
                            call(c,0x7000e3c424,s,(ring+d+3)%3,page+d);
                    }
                    target=-1;
                }
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
    bool area=fabsf(point[0])<=633&&fabsf(point[1])<=285;
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
    float cursor[2]={fmaxf(-632,fminf(632,point[0])),fmaxf(-280,fminf(280,point[1]))},best_snap=1e9f;
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

// The screen's cursor type is translated by the UI controller, not used as
// a manager-array index. Only that manager channel may focus this screen.
static bool hover_target(Cpu *c,uint64_t screen,unsigned target) {
    uint64_t ui=LD64(screen+0x28),router=ptr(ui)?LD64(ui+0x48):0;
    uint64_t vt=ptr(router)?LD64(router):0,resolve=ptr(vt)?LD64(vt+0x28):0;
    if(!ptr(resolve))return false;
    return call(c,resolve,router,LD8(screen+0xfc),0)==target;
}

static uint64_t quest_list(uint64_t screen) {
    if(!ptr(screen)||LD64(screen)!=0x7001cf9ad8ull||LD8(screen+0xfe)!=2)return 0;
    uint64_t lists=LD64(screen+0x390);
    if(LD32(screen+0x388)!=3||!ptr(lists))return 0;
    uint64_t list=LD64(lists+8);
    return ptr(list)&&LD64(list)==0x7001cdbb58ull?list:0;
}

// Adventure Log owns a virtualized list. Changing only BoxCursorControl leaves
// that list on the keyboard's old row; its next update restores the old focus.
// Select through the list's own node lookup and selection path instead. The
// native list then updates the highlight, quest details and sound together.
static bool native_item_list(uint64_t list) {
    if(!ptr(list))return false;
    uint64_t vt=LD64(list);
    // The five-row purchase list derives from the same native list class as
    // the selling grid, with its own vtable but the same selection contract.
    return vt==0x7001cdbb58ull||vt==0x7001cfcf78ull;
}
static bool quest_focus(Cpu *c,uint64_t screen,uint64_t node) {
    uint64_t vt=LD64(screen);
    if(vt!=0x7001cf9ad8ull&&vt!=0x7001ceb808ull&&screen!=shop_screen)return false;
    unsigned count=LD32(screen+0x388);uint64_t lists=LD64(screen+0x390);
    if(!ptr(lists)||count>8)return false;
    for(unsigned i=0;i<count;i++) {
        uint64_t list=LD64(lists+8*i);
        if(!native_item_list(list))continue;
        int index=(int)call(c,0x7000d6d710,list,node,0);
        if(index<0)continue;
        // 2dc is a scroll-page index, not the focused item. Selecting that
        // page rebuilds positions and makes a stationary pointer chase rows.
        // The cursor-only path updates 30c/310 and the native highlight.
        if((int)LD32(list+0x30c)!=index||(int)LD32(list+0x310)!=index)
            call(c,0x7000d6c870,list,(unsigned)index,0);
        return true;
    }
    return false;
}

// These category panes belong to nested button layouts, not the screen root.
static uint64_t button_parent_named(uint64_t screen,const char *name) {
    uint64_t head=screen+0x88,link=LD64(head+8);int offset=(int)LD32(screen+0x9c);
    for(unsigned i=0;ptr(link)&&link!=head&&i<256;i++,link=LD64(link+8)) {
        uint64_t node=link-offset,button=LD64(node+0x18);
        uint64_t layout=ptr(button)?LD64(button+0x20):0,parent=ptr(layout)?LD64(layout+0x18):0;
        if(ptr(parent)&&strncmp((char*)(uintptr_t)(parent+0xb8),name,40)==0)return LD64(button+0x60);
    }
    return 0;
}

static bool quest_categories(Cpu *c,uint64_t screen) {
    if(!quest_list(screen)||LD32(screen+0x3a48)!=2)return false;
    uint64_t quest=LD64(screen+0x3bb0),list=ptr(quest)?LD64(quest+0x300):0;
    // The outer list switches categories and creates their nested row lists.
    // It has no row cursor nodes; those belong to the registered child lists.
    if(!native_item_list(list))return false;
    for(unsigned category=0;category<4;category++) {
        char name[40];snprintf(name,sizeof name,"Pa_ChallengeCategory_%02u",category);
        uint64_t pane=button_parent_named(screen,name);float box[4];
        bool valid=ptr(pane)&&options_box(pane,box);
        if(!valid)continue;
        // A category layout is 211 px wide although icons are 58 px apart.
        // Its 32 px cursor pane identifies the actual icon without overlap.
        float x=*(float*)(pane+0x7c),y=*(float*)(pane+0x8c);
        box[0]=x-(category==0?31:25);box[1]=y-25;box[2]=x+(category==0?31:25);box[3]=y+25;
        if(point[0]<box[0]||point[0]>box[2]||point[1]<box[1]||point[1]>box[3])continue;
        set_hover_rect(box);
        pointer_hover_sound(c,screen,screen+0x300+category);
        if(click_pending) {
            click_pending=false;
            pointer_sound(c,screen,"mc_Decide");
            unsigned count=LD32(list+0x288);uint64_t columns=LD64(list+0x290);
            uint64_t column=category<count&&ptr(columns)?LD64(columns+8*category):0;
            // Locked/empty categories retain the game's disabled behavior.
            if(ptr(column)&&LD32(column+0x29c)>0&&!LD8(column+0x38c)&&!LD8(column+0x38d)&&LD32(list+0x2e8)!=category) {
                // Schedule the normal transition; immediate mode rebuilds
                // nested controls while BoxCursorMgr is still using them.
                call(c,0x7000d6c244,list,0,category);
            }
        }
        return true;
    }
    return false;
}

static uint64_t pane_named(uint64_t pane,const char *name,unsigned depth,unsigned *budget) {
    if(!ptr(pane)||depth>32||!*budget)return 0;
    --*budget;
    if(strncmp((char*)(uintptr_t)(pane+0xb8),name,40)==0)return pane;
    uint64_t head=pane+0x20,link=LD64(head+8);
    while(ptr(link)&&link!=head&&*budget) {
        uint64_t found=pane_named(link-8,name,depth+1,budget);
        if(found)return found;
        link=LD64(link+8);
    }
    return 0;
}

static bool pause_headers(Cpu *c,uint64_t screen,uint64_t screens,unsigned count) {
    if(!quest_list(screen)||count<=0x2f)return false;
    // Submenus retain the pause screen underneath them, but own the input.
    for(unsigned i=0x30;i<=0x3a&&i<count;i++) {
        uint64_t modal=LD64(screens+8*i);
        if(ptr(modal)&&LD8(modal+0xfe)==2)return false;
    }
    uint64_t header=LD64(screens+8*0x2f);
    if(!ptr(header)||LD8(header+0xfe)!=2)return false;
    unsigned current=LD32(screen+0x3a48);
    if(current>2||LD32(screen+0x3a4c)!=current)return false;
    for(unsigned side=0;side<2;side++) {
        float half=current==0?(side?40.f:70.f):46.f;
        float center=side?150.f:-150.f;
        float box[4]={center-half,272,center+half,327};
        // Both animated text copies share one fixed hit area with the key
        // hint above them. Never let hover geometry follow their animation.
        if(point[0]<box[0]||point[0]>box[2]||point[1]<box[1]||point[1]>box[3])continue;
        int next=side?(current==0?1:current==2?0:-1):(current==0?2:current==1?0:-1);
        if(next<0)return false;
        set_hover_rect(box);
        pointer_hover_sound(c,screen,screen+0x100+side);
        if(click_pending) {
            click_pending=false;
            pointer_sound(c,screen,"mc_Decide");
            ST32(screen+0x3a4c,next);
            // The normal page-change state performs exit/enter and animation.
            call(c,0x7001403a88,screen+0x178,0x7001e2f4a8ull,0);
        }
        return true;
    }
    return false;
}

// Other menus enter a button once per hover, rather than replaying its focus
// callback whenever the screen rebuilds a transient cursor field.
static bool hover_enter(Cpu *c,uint64_t control,uint64_t screen,uint64_t node,uint64_t activate) {
    if(hover_control==control&&hover_screen==screen&&hover_node==node)return false;
    hover_control=control;hover_screen=screen;hover_node=node;
    if(LD64(control+0x48)==node)return false;
    call(c,activate,control,node,0);ST8(screen+0x106,1);
    return true;
}

static bool dialogue_choices(Cpu *c,uint64_t screens,unsigned count) {
    if(!dialogue_active)return false;
    uint64_t s=0;
    for(unsigned i=0;i<count&&i<256;i++) {
        uint64_t candidate=LD64(screens+8*i);
        if(ptr(candidate)&&LD64(candidate)==0x7001d4ed48ull&&LD8(candidate+0xfe)==2){s=candidate;break;}
    }
    if(!s)return false;
    uint64_t head=s+0x88,link=LD64(head+8);int offset=(int)LD32(s+0x9c);
    for(unsigned i=0;ptr(link)&&link!=head&&i<8;i++,link=LD64(link+8)) {
        uint64_t node=link-offset,vt=LD64(node),button=LD64(node+0x18);
        uint64_t pane=ptr(button)?LD64(button+0x60):0;float box[4];
        if(!ptr(vt)||!options_box(pane,box)||box[0]<840||box[0]>1265||box[1]<-320||box[3]>320)continue;
        // The two manager channels run separately. Text advance must never
        // confirm a response merely because its channel has not run yet.
        if((call(c,LD64(vt+0x28),node,0,0)|call(c,LD64(vt+0x28),node,1,0))&1)return true;
    }
    return false;
}

static bool shop_pointer(Cpu *c) {
    uint64_t s=shop_screen;
    if(!ptr(s)||pointer_keyboard_input)return false;
    uint64_t lists=LD64(s+0x390),list=ptr(lists)&&LD32(s+0x388)?LD64(lists):0;
    if(!native_item_list(list))return false;
    unsigned columns=LD32(list+0x288);uint64_t array=LD64(list+0x290);
    if(columns>16||!ptr(array))return false;
    for(unsigned i=0;i<7;i++) {
        char name[32];snprintf(name,sizeof name,"Pa_CategoryS_%02u",i);
        uint64_t pane=call(c,0x7000f6f800,s,(uintptr_t)name,0);float box[4];
        if(!options_box(pane,box))continue;
        float x=*(float*)(pane+0x7c),y=*(float*)(pane+0x8c);
        if(fabsf(point[0]-x)>28||fabsf(point[1]-y)>28)continue;
        set_hover(x,y,27);
        if(click_pending) {
            click_pending=false;
            uint64_t column=i<columns?LD64(array+8*i):0;
            if(ptr(column)&&LD32(column+0x29c)&&!LD8(column+0x38c)&&!LD8(column+0x38d))
                call(c,0x7000d6c244,list,0,i);
        }
        return true;
    }
    int scroll=atomic_exchange(&wheel,0);
    if(scroll) {
        unsigned col=LD32(list+0x2e8);uint64_t column=col<columns?LD64(array+8*col):0;
        unsigned pages=ptr(column)?LD32(column+0x2f4):0;
        int page=(int)LD32(list+0x2dc)-(scroll>0?1:-1);
        if(pages&&pages<1000){if(page<0)page=0;if(page>=(int)pages)page=pages-1;call(c,0x7000d6c54c,list,page,0);}
    }
    return false;
}

static bool quantity_pointer(Cpu *c,uint64_t screens,unsigned count) {
    if(!dialogue_active)return false;
    for(unsigned i=0;i<count&&i<256;i++) {
        uint64_t screen=LD64(screens+8*i);
        if(!ptr(screen)||LD64(screen)!=0x7001d4ed48ull||LD8(screen+0xfe)!=2)continue;
        int max=(int)LD32(screen+0x750),value=(int)LD32(screen+0x754);
        if(max<1||max>999999||value<1||value>max||!ptr(LD64(screen+0x650)))continue;
        uint64_t pane=LD64(screen+0x650);float cx=*(float*)(pane+0x7c),cy=*(float*)(pane+0x8c);
        if(!isfinite(cx)||!isfinite(cy)||fabsf(cx)>640||fabsf(cy)>360)continue;
        if(quantity_screen!=screen||(plat_now_ns()>quantity_until&&!quantity_hold)){quantity_edit=false;quantity_request=-1;quantity_hold=0;}
        quantity_screen=screen;quantity_max=max;quantity_value=value;quantity_until=plat_now_ns()+100000000;
        // 640/648 are animation controllers, not panes. Resolve the real
        // arrow panes alongside T_Num_00 and hit-test their rendered locations.
        uint64_t root=LD64(pane+0x18);unsigned budget=80;
        uint64_t left=pane_named(root,"Pa_ArrowL_00",0,&budget);budget=80;
        uint64_t right=pane_named(root,"Pa_ArrowR_00",0,&budget);
        bool row=fabsf(point[1]-cy)<=22;
        bool dec=ptr(left)&&row&&fabsf(point[0]-*(float*)(left+0x7c))<=18;
        bool inc=ptr(right)&&row&&fabsf(point[0]-*(float*)(right+0x7c))<=18;
        bool number=row&&fabsf(point[0]-cx)<=28,over=dec||inc||number;
        // Feedback belongs to the original arrow panes. Keep their disabled
        // animation at the limits; hover scales only an available arrow.
        bool mouse=!pointer_keyboard_input;
        if(ptr(left)) {
            float scale=mouse&&dec&&value>1?(quantity_hold<0?1.12f:1.20f):1.f;
            *(float*)(left+0x48)=*(float*)(left+0x4c)=scale;
        }
        if(ptr(right)) {
            float scale=mouse&&inc&&value<max?(quantity_hold>0?1.12f:1.20f):1.f;
            *(float*)(right+0x48)=*(float*)(right+0x4c)=scale;
        }
        if(mouse) {
            if(dec&&value>1)pointer_hover_sound(c,screen,left);
            else if(inc&&value<max)pointer_hover_sound(c,screen,right);
            else if(number)pointer_hover_sound(c,screen,pane);
        }
        int request=atomic_exchange(&quantity_request,-1);
        uint64_t now=plat_now_ns();
        if(frame_press&&(dec||inc)&&!quantity_hold) {
            quantity_hold=inc?1:-1;quantity_repeat_at=now+350000000;
            request=value+quantity_hold;quantity_edit=false;click_pending=false;
        }
        if(quantity_hold) {
            if(!gesture_down||frame_release||pointer_keyboard_input) {
                quantity_hold=0;click_pending=false;
            } else if(now>=quantity_repeat_at) {
                request=value+quantity_hold;quantity_repeat_at=now+70000000;
            }
        }
        if(over&&click_pending) {
            click_pending=false;hover_until=0;
            if(dec){request=value-1;quantity_edit=false;}
            else if(inc){request=value+1;quantity_edit=false;}
            else {
                quantity_edit=true;quantity_replace=true;
                pointer_sound(c,screen,"mc_Decide");
            }
        } else if(click_pending)quantity_edit=false;
        if(request>=0) {
            if(request<1)request=1;if(request>max)request=max;
            ST32(screen+0x754,request);quantity_value=request;
            call(c,0x70013f59f8,screen,0,0);
            if(request!=value)pointer_sound(c,screen,"mc_CursorMove");
        }
        // Render editing feedback with the game's TextBox and font. No host
        // overlay: hover tints the number; editing adds a blinking text caret.
        if(LD64(pane)==0x7001d0e878ull) {
            uint32_t color=quantity_edit?0xff8ce8ffu:number?0xffffe0a0u:0xffffffffu;
            ST32(pane+0xf0,color);ST32(pane+0xf4,color);
            char digits[24];snprintf(digits,sizeof digits,"%d%s",(int)quantity_value,
                quantity_edit&&(now/500000000)%2==0?"|":"");
            uint16_t text[24];unsigned len=0;
            while(digits[len]&&len<23){text[len]=(unsigned char)digits[len];len++;}text[len]=0;
            call(c,LD64(LD64(pane)+0x140),pane,(uintptr_t)text,len);
        }
        return over||quantity_edit||quantity_hold;
    }
    return false;
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
    if(n>0x34&&ptr(screens)&&options_pointer(c,screens,bounds)){hover_leave();return;}
    if(n>0x39&&ptr(screens)&&native_special(c,screens,bounds)){hover_leave();return;}
    if(quantity_pointer(c,screens,n))return;
    if(pointer_keyboard_input){hover_leave();hover_until=0;return;}
    if(!click_pending&&!pointer_test()&&!motion_at)return;
    if(n>0x2e&&ptr(screens)) {
        uint64_t screen=LD64(screens+8*0x2e);
        if(ptr(screen)&&hover_target(c,screen,target)&&
           (pause_headers(c,screen,screens,n)||quest_categories(c,screen))){hover_leave();return;}
    }
    uint64_t hit=0,hit_screen=0;float best=1e30f;bool choices=dialogue_choices(c,screens,n);
    if(shop_screen&&!choices&&shop_pointer(c))return;
    extern uint64_t renderer_present_count(void);
    static bool dumped_screens[256];
    for(unsigned id=0;id<n&&id<256&&ptr(screens);id++) {
        uint64_t screen=LD64(screens+8*id);
        if(!ptr(screen)||LD8(screen+0xfe)!=2)continue;
        if(dialogue_active&&choices&&LD64(screen)!=0x7001d4ed48ull)continue;
        if(shop_screen&&!choices&&screen!=shop_screen)continue;
        if(dialogue_active&&!shop_screen&&LD64(screen)!=0x7001d4ed48ull)continue;
        if(!hover_target(c,screen,target))continue;
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
                    if(dialogue_active&&LD64(screen)==0x7001d4ed48ull) {
                        // Choice arrows use left-origin X, center-origin Y.
                        box[0]-=640;box[2]-=640;box[2]=fminf(625,box[0]+255);
                    }
                    // The native "clip" callback clears the active cursor for
                    // an offscreen row. Calling it while scanning every node
                    // repeatedly cancels real focus. Clip without side effects.
                    if(LD64(screen)==0x7001cf9ad8ull&&LD32(screen+0x368c)) {
                        box[0]=fmaxf(box[0],*(float*)(screen+0x367c));
                        box[1]=fmaxf(box[1],*(float*)(screen+0x3680));
                        box[2]=fminf(box[2],*(float*)(screen+0x3684));
                        box[3]=fminf(box[3],*(float*)(screen+0x3688));
                    }
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
                    bool eligible=(call(c,LD64(vtable+0x28),node,target,0)&1)!=0;
                    if(dialogue_active&&LD64(screen)==0x7001d4ed48ull&&eligible&&box[0]>200&&box[0]<625&&box[1]>-320&&box[3]<320)choices=true;
                    if(isfinite(box[0])&&isfinite(box[1])&&isfinite(box[2])&&isfinite(box[3])&&
                        point[0]>=box[0]&&point[0]<=box[2]&&point[1]>=box[1]&&point[1]<=box[3]&&
                        eligible) {
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
    if(!hit){
        if(dialogue_active&&!choices&&click_pending){confirm_until=plat_now_ns()+50000000;click_pending=false;}
        if(hover_control==control)hover_leave();return;
    }
    // The PC hover is drawn this frame; the game's own highlight may still be playing its transition.
    if(hit&&best<1e29f) {
        uint64_t button=LD64(hit+0x18),pane=LD64(button+0x60);
        if(ptr(pane)&&*(float*)(pane+0x50)>40&&*(float*)(pane+0x50)<150)
            set_hover(*(float*)(pane+0x7c),*(float*)(pane+0x8c),*(float*)(pane+0x50)*.5f);
    }
    bool quest=quest_focus(c,hit_screen,hit);
    if(quest&&LD64(control+0x48)!=hit)call(c,activate,control,hit,0);
    if(!quest&&hover_enter(c,control,hit_screen,hit,activate)) {
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-hover] node %llx point %.1f %.1f\n",hit,point[0],point[1]);
    }
    if(click_pending) {
        // A list may have rebuilt the manager cursor after hover entered.
        // Confirmation must still target the row under the pointer.
        if(LD64(control+0x48)!=hit)call(c,activate,control,hit,0);
        injected_manager=manager;saved_action=LD32(manager+0x10);
        ST32(manager+0x10,1); // The exact action used by the game's confirm button, processed by its own code next.
        click_pending=false;
        if(getenv("SAVAGEWIND_PC_CURSOR_LOG"))LOG("[pc-click] confirm node %llx\n",hit);
    }
}

RT_ON_RESET(pc_cursor_reset) {
    dialogue_active=false;shop_screen=quantity_screen=0;quantity_until=0;quantity_edit=false;quantity_request=-1;
    quantity_hold=0;quantity_repeat_at=0;preview_defaults_saved=false;sound_hover_key=sound_hover_until=0;
    hover_leave();
    options_scroll_valid=false;options_scroll=0;options_keyboard=false;options_page_pending=false;pointer_keyboard_input=false;pointer_mouse_events=0;options_keyboard_mouse_events=0;options_screen_active=options_rows_active=0;
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
