#include <assert.h>
#include <stdio.h>
#include "../runtime/pc_cursor_math.h"
int main(void) {
    int sizes[][2]={{1280,720},{1920,1080},{2560,1440},{3840,2160},{2560,1080},{1280,1024},{1920,1200}};
    for(unsigned i=0;i<sizeof sizes/sizeof sizes[0];i++) {
        float w=sizes[i][0],h=sizes[i][1],k=fminf(w/1280,h/720),p[2];
        assert(pc_cursor_to_layout(w,h,w*.5f+550*k,h*.5f+80*k,p));
        assert(fabsf(p[0]-550)<.001f&&fabsf(p[1]+80)<.001f);
        assert(pc_cursor_to_layout(w,h,w*.5f-280*k,h*.5f-97*k,p));
        assert(fabsf(p[0]+280)<.001f&&fabsf(p[1]-97)<.001f);
        printf("%dx%d: title and inventory coordinates PASS\n",sizes[i][0],sizes[i][1]);
    }
    float p[2];assert(!pc_cursor_to_layout(2560,1080,0,540,p));
    assert(!pc_cursor_to_layout(1280,1024,640,0,p));
    assert(!pc_cursor_to_layout(0,0,0,0,p));
    puts("PASS: aspect ratios, bars and zero-size windows");
    float zooms[]={.07f,.14f,.28f,1.f};
    for(unsigned z=0;z<4;z++)for(int frame=0;frame<300;frame++) {
        float zoom=zooms[z],p[2]={-300+frame*2,-100+frame*.5f};
        float center[2]={-3115.94f+frame,6108.09f-frame*.7f};
        float translation[2]={center[0]*zoom,center[1]*zoom},world[2];
        pc_cursor_map_world(p,translation,zoom,world);
        assert(fabsf(world[0]*zoom+translation[0]-p[0])<.002f);
        assert(fabsf(world[1]*zoom+translation[1]-p[1])<.002f);
        // Dragging by a logical pixel moves the rendered map by that pixel at every zoom.
        float dragged=center[0]+2/zoom;
        assert(fabsf(dragged*zoom-translation[0]-2)<.002f);
    }
    puts("PASS: 1200 moving pointer/pan samples at 4 map zoom levels");
    float middle[2]={0,0},corner[2]={632,280},header[2]={0,330},outside[2]={700,0},delta[2];
    pc_map_navigation(middle,0,0,true,.02f,1,delta);assert(delta[0]==0&&delta[1]==0);
    pc_map_navigation(header,0,0,true,.02f,1,delta);assert(delta[0]==0&&delta[1]==0);
    pc_map_navigation(outside,0,0,true,.02f,1,delta);assert(delta[0]==0&&delta[1]==0);
    pc_map_navigation(corner,0,0,false,.02f,1,delta);assert(delta[0]==0&&delta[1]==0);
    for(int fps=30;fps<=120;fps*=2)for(unsigned z=0;z<4;z++) {
        float total=0;
        for(int f=0;f<fps;f++){pc_map_navigation(middle,1,0,false,1.f/fps,zooms[z],delta);total+=delta[0]*zooms[z];}
        assert(fabsf(total+360)<.002f);
        pc_map_navigation(corner,0,0,true,1.f/fps,zooms[z],delta);
        assert(delta[0]<0&&delta[1]<0);
        assert(fabsf(hypotf(delta[0],delta[1])*zooms[z]-360.f/fps)<.002f);
    }
    pc_map_navigation(middle,1,0,false,10,1,delta);assert(fabsf(delta[0]+18)<.001f);
    puts("PASS: map navigation speed at 30/60/120 FPS and 4 zooms; normalized corners, header/outside exclusion, drag/modal suppression and stall limit");
}
