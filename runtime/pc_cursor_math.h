#pragma once
#include <math.h>
#include <stdbool.h>
#include <float.h>
// The native map leaves one vertical pan bound at FLT_MAX: controller cursor
// navigation normally constrains it elsewhere. Direct pan must close that end
// using the opposite, zoom-dependent extent without changing finite bounds.
static inline float pc_map_clamp_center(float value,float lower,float upper) {
    bool lo=isfinite(lower)&&fabsf(lower)<FLT_MAX*.5f;
    bool hi=isfinite(upper)&&fabsf(upper)<FLT_MAX*.5f;
    if(!lo)lower=hi?-upper:0;
    if(!hi)upper=lo?-lower:0;
    if(lower>upper){float mid=(lower+upper)*.5f;lower=upper=mid;}
    if(!isfinite(value))value=0;
    return fmaxf(lower,fminf(upper,value));
}
// SDL window size and mouse position use the same logical coordinate system, including on a high-DPI display.
// BOTW's menu layouts use 1280x720. The presented pause/title image is fitted to the window, with bars if needed.
static inline bool pc_cursor_to_layout(float width,float height,float x,float y,float out[2]) {
    if(width<=0||height<=0)return false;
    float scale=fminf(width/1280.0f,height/720.0f);
    out[0]=(x-width*.5f)/scale;
    out[1]=(height*.5f-y)/scale;
    return fabsf(out[0])<=640&&fabsf(out[1])<=360;
}
// The map matrix translation is already zoomed. Invert translation BEFORE dividing by scale.
static inline void pc_cursor_map_world(const float point[2],const float translation[2],float zoom,float out[2]) {
    out[0]=(point[0]-translation[0])/zoom;
    out[1]=(point[1]-translation[1])/zoom;
}

// Navigation direction, not drag direction: looking right moves the map image left.
// Edge zones belong to the map canvas, below the top tabs and above the footer.
static inline void pc_map_navigation(const float point[2],float x,float y,bool edges,float dt,float zoom,float out[2]) {
    if(edges&&fabsf(point[0])<=640&&fabsf(point[1])<=285&&x==0&&y==0) {
        float ex=fmaxf(0,fminf(1,(fabsf(point[0])-600)/32));
        float ey=fmaxf(0,fminf(1,(fabsf(point[1])-250)/30));
        x=copysignf(ex*ex,point[0]);y=copysignf(ey*ey,point[1]);
    }
    float n=hypotf(x,y);if(n>1){x/=n;y/=n;}
    dt=fmaxf(0,fminf(.05f,dt));
    float speed=isfinite(zoom)&&zoom>0?360*dt/zoom:0;
    out[0]=-x*speed;out[1]=-y*speed;
}
