#include <cassert>
#include <cstdio>
#include "../runtime/ui/gyro_drag.h"
static int arc_calls,axis_calls;
static float last_q[4];
extern "C" void motion_gizmo_arcball(float w,float x,float y,float z){arc_calls++;last_q[0]=w;last_q[1]=x;last_q[2]=y;last_q[3]=z;}
extern "C" void motion_gizmo_rotate(float,float,float){axis_calls++;}
static GyroScene s;
static void frame(int w,int h,ImVec2 p,bool down){
 auto &io=ImGui::GetIO();io.DisplaySize=ImVec2((float)w,(float)h);io.DeltaTime=1.f/60;
 io.AddMousePosEvent(p.x,p.y);io.AddMouseButtonEvent(0,down);
 ImGui::NewFrame();gyro_scene_draw(io,h/1080.f,s);ImGui::Render();
 assert(ImGui::GetDrawData()->TotalVtxCount==0); // no mesh, rings, labels or panel
}
int main(){
 ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;
 io.Fonts->AddFontDefault();unsigned char *pixels;int fw,fh;io.Fonts->GetTexDataAsRGBA32(&pixels,&fw,&fh);
 const int sizes[][2]={{1280,720},{1920,1080},{3840,2160},{2560,1080}};
 for(const auto &size:sizes){
  int w=size[0],h=size[1];float k=h/1080.f;
  s={};s.active=true;s.generation++;
  ImVec2 p(w*.75f,h*.6f);
  frame(w,h,p,false);arc_calls=axis_calls=0;
  frame(w,h,p,true);assert(arc_calls==0&&axis_calls==0); // no initial click jump
  frame(w,h,p,true);assert(arc_calls==0);
  p.x+=40*k;p.y-=25*k;frame(w,h,p,true);
  assert(arc_calls==1&&axis_calls==0);double norm=0;for(float q:last_q)norm+=q*q;
  assert(fabs(norm-1)<1e-5&&fabs(last_q[1])>1e-3&&fabs(last_q[3])>1e-3);
  frame(w,h,p,true);assert(arc_calls==1); // stationary mouse retains the grip
  p.x+=20*k;frame(w,h,p,true);assert(arc_calls==2&&axis_calls==0);
  arc_calls=1;
  frame(w,h,p,true);assert(arc_calls==1); // stationary mouse retains pose
  frame(w,h,p,false);p.x+=20*k;frame(w,h,p,false);assert(arc_calls==1);
  frame(w,h,p,true);assert(arc_calls==1); // re-grip is also jump-free
  s.active=false;p.y+=20*k;frame(w,h,p,true);assert(arc_calls==1); // leaving stops drag
  frame(w,h,p,false);
  printf("%dx%d: drag, hold/re-grip, exit, no visible geometry PASS\n",w,h);
 }
 ImGui::DestroyContext();
}
