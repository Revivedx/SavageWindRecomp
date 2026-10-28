#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include "imgui.h"
#include "../motion.h"
#include "../pc_gyro_scene.h"

static void gyro_ball_vector(ImVec2 mouse,ImVec2 center,float radius,float out[3]) {
    out[0]=(mouse.x-center.x)/radius;out[1]=(center.y-mouse.y)/radius;
    float d=out[0]*out[0]+out[1]*out[1];
    out[2]=d<=1?sqrtf(1-d):0;
    if(d>1){float k=1/sqrtf(d);out[0]*=k;out[1]*=k;}
}
static bool gyro_ball_delta(const float from[3],const float to[3],float q[4]) {
    float dot=std::clamp(from[0]*to[0]+from[1]*to[1]+from[2]*to[2],-1.f,1.f);
    if(dot>1-1e-7f)return false;
    q[0]=1+dot;
    q[1]=from[1]*to[2]-from[2]*to[1];q[2]=from[2]*to[0]-from[0]*to[2];q[3]=from[0]*to[1]-from[1]*to[0];
    float n=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if(n<1e-6f){q[0]=0;q[1]=0;q[2]=-from[2];q[3]=from[1];n=hypotf(q[2],q[3]);
        if(n<1e-6f){q[1]=-from[1];q[2]=from[0];q[3]=0;n=hypotf(q[1],q[2]);}}
    for(int i=0;i<4;i++)q[i]/=n;
    return true;
}
static void gyro_scene_hidden_drag(ImGuiIO &io,const GyroScene &scene,bool enabled) {
    static bool dragging;
    static unsigned generation;
    static ImVec2 origin;static float radius,previous[3];
    if(!enabled || !scene.active || generation!=scene.generation || !ImGui::IsMouseDown(0))dragging=false;
    generation=scene.generation;
    if(!enabled || !scene.active)return;
    float w=io.DisplaySize.x,h=io.DisplaySize.y;
    bool inside=io.MousePos.x>=0&&io.MousePos.x<=w&&io.MousePos.y>=0&&io.MousePos.y<=h;
    bool clicked=ImGui::IsMouseClicked(0);
    if(clicked && inside) {
        // Re-grip at the actual click. No dependency on a model's debug pivot,
        // projected sphere, or invisible axis rings, which can differ by shrine.
        dragging=true;origin=io.MousePos;radius=fmaxf(1.f,fminf(w,h)*.38f);
        gyro_ball_vector(io.MousePos,origin,radius,previous);
    }
    if(dragging && ImGui::IsMouseDown(0) && !clicked) {
        float v[3],q[4];gyro_ball_vector(io.MousePos,origin,radius,v);
        if(gyro_ball_delta(previous,v,q))motion_gizmo_arcball(q[0],q[1],-q[3],q[2]);
        memcpy(previous,v,sizeof v);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
}
static void gyro_scene_draw(ImGuiIO &io,float scale,const GyroScene &scene) {
    (void)scale;
    gyro_scene_hidden_drag(io,scene,true);
}
