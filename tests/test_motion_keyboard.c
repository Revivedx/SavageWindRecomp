#include <assert.h>
#include <stdio.h>
#include "../runtime/hle/motion.c"

Settings g_settings = {.motion_sens = 1};
static uint64_t clock_ns;
uint64_t plat_now_ns(void) { return clock_ns; }


static void fresh(void) {
    motion_reboot_reset(); memset(&g,0,sizeof g); g.q.w=1;g.acc[2]=-1;
    g_settings.motion_sens=1;g_settings.motion_invert=0;
    clock_ns=1000000000;update(clock_ns);
}
// Independent Rx/Ry/Rz composition, not the runtime's quaternion/Euler helpers.
static void orientation(double pitch,double roll,double yaw) {
    double p=pitch*PI/180,r=roll*PI/180,y=yaw*PI/180;
    double a[3][3]={{cos(r),0,sin(r)}, {sin(p)*sin(r),cos(p),-sin(p)*cos(r)},
                    {-cos(p)*sin(r),sin(p),cos(p)*cos(r)}};
    double z[3][3]={{cos(y),-sin(y),0},{sin(y),cos(y),0},{0,0,1}};
    const SixAxisState *st=&s.ring[s.head];
    for(int i=0;i<3;i++)for(int j=0;j<3;j++) {
        double want=0;for(int k=0;k<3;k++)want+=z[i][k]*a[k][j];
        if(fabs(st->dir[j][i]-want)>2e-5){fprintf(stderr,"matrix mismatch p/r/y %.2f %.2f %.2f row/col %d %d got %.5f expected %.5f\n",pitch,roll,yaw,i,j,st->dir[j][i],want);abort();}
    }
    double gravity=0;
    for(int i=0;i<3;i++) {
        assert(isfinite(st->gyro[i])&&fabs(st->gyro[i])<4.01);
        gravity+=st->acc[i]*st->acc[i];
    }
    assert(fabs(gravity-1)<2e-5);
}
static void full_turns(int rate) {
    const int axes[][3]={{1,0,0},{0,1,0},{0,0,1},{1,1,1},{-1,-1,-1}};
    for(unsigned a=0;a<5;a++) {
        fresh(); motion_keyboard(axes[a][0],axes[a][1],axes[a][2],false);
        for(int i=1;i<=rate*20;i++) {
            update(clock_ns=1000000000+(uint64_t)i*1000000000/rate);
            double t=(double)(s.last_ns-1000000000)/1e9;
            orientation(axes[a][0]*90*t,axes[a][1]*90*t,axes[a][2]*90*t);
        }
        motion_keyboard(0,0,0,false);update(clock_ns+=100000000);
        orientation(axes[a][0]*1800,axes[a][1]*1800,axes[a][2]*1800);
        for(int i=0;i<3;i++)assert(fabs(s.ring[s.head].gyro[i])<1e-5);
    }
    // Non-integer sensitivity and inversion must remain smooth across full turns.
    fresh();g_settings.motion_sens=.5;g_settings.motion_invert=MOTION_INVERT_UD|MOTION_INVERT_LR;
    motion_keyboard(1,1,1,false);
    for(int i=1;i<=rate*20;i++) {
        update(clock_ns=1000000000+(uint64_t)i*1000000000/rate);
        double t=(double)(s.last_ns-1000000000)/1e9;
        orientation(-45*t,90*t,-45*t);
    }
    printf("%d Hz: multiple turns, all three axes, diagonals, 90/180/360 crossings, half sensitivity and inversion PASS\n",rate);
}
static void mouse_turns(void) {
    fresh();motion_tilt_held(true);
    for(int i=1;i<=1200;i++) {
        motion_mouse(3,3);motion_mouse_yaw(.1f);
        update(clock_ns+=10000000);
        orientation(i*.6,i*.6,i*.6);
    }
    motion_tilt_held(false);
    double oldp=s.pitch,oldr=s.roll,oldy=s.yaw;
    for(int i=0;i<400;i++) {
        motion_mouse(100,100);motion_mouse_yaw(10);
        update(clock_ns+=5000000);
    }
    assert(fabs(s.pitch-oldp)<1e-8 && fabs(s.roll-oldr)<1e-8 && fabs(wrap(s.yaw-oldy))<1e-8);
    orientation(720,720,720);
    motion_tilt_held(true);motion_mouse(10,10);motion_mouse_yaw(1);
    update(clock_ns+=10000000);orientation(722,722,726);
    motion_tilt_held(false);
    motion_keyboard(0,0,0,true);update(clock_ns+=10000000);update(clock_ns+=10000000);
    orientation(0,0,0);
    assert(!g.tilt_pitch&&!g.tilt_roll&&!g.tilt_yaw);
    motion_tilt_held(true);motion_mouse(10,10);motion_mouse_yaw(1);
    motion_reboot_reset();assert(!g.tilt_held&&!g.tilt_pitch&&!g.tilt_roll&&!g.tilt_yaw);
    // Physical sensor path still uses the existing conservative camera pitch limit.
    fresh();g.sensors=g.started=true;g.q=euler_quat(0,89*PI/180,0);
    update(clock_ns+=10000000);assert(fabs(s.pitch-PITCH_MAX)<1e-7);
    puts("T+mouse/wheel: two full turns, clutch hold/reposition/resume, explicit O reset, reboot and physical pitch limit PASS");
}

static void fast_mouse(void) {
    const double bursts[]={180,200,360,720,-180,-200,-360,-720};
    for(int axis=0;axis<3;axis++)for(unsigned b=0;b<8;b++) {
        fresh();motion_tilt_held(true);
        double target=bursts[b];
        if(axis==0)motion_mouse(0,target/.2);
        if(axis==1)motion_mouse(target/.2,0);
        if(axis==2)motion_mouse_yaw(target/6);
        double previous=0;
        for(int i=1;i<=110;i++) {
            update(clock_ns+=5000000);
            double expected=copysign(fmin(fabs(target),7.2*i),target);
            orientation(axis==0?expected:0,axis==1?expected:0,axis==2?expected:0);
            double actual=(axis==0?s.pitch:axis==1?s.roll:s.yaw)*180/PI;
            assert((actual-previous)*target>=-1e-6); // no reversal at 180 / 360
            previous=actual;
        }
        assert(fabs(previous-target)<1e-4);
    }
    // Cancelling a very large backlog must not replay every old revolution.
    fresh();motion_tilt_held(true);motion_mouse(18000,18000);motion_mouse_yaw(600);
    update(clock_ns+=10000000);
    motion_keyboard(0,0,0,true);
    for(int i=0;i<10;i++)update(clock_ns+=5000000);
    orientation(0,0,0);
    puts("Fast packets: +/-180/200/360/720 on all axes, bounded samples, correct direction, complete turns, backlog cancellation PASS");
}

static void arcball_composition(void) {
    fresh();
    // Independent Rodrigues rotation, repeatedly composed in world space.
    double axis[3]={.23,.47,.85},len=sqrt(.23*.23+.47*.47+.85*.85);
    for(int i=0;i<3;i++)axis[i]/=len;
    double step[3][3],expected[3][3]={{1,0,0},{0,1,0},{0,0,1}},angle=PI/180;
    for(int r=0;r<3;r++)for(int c=0;c<3;c++){
        double cross=(r==0&&c==1)?-axis[2]:(r==0&&c==2)?axis[1]:(r==1&&c==0)?axis[2]:(r==1&&c==2)?-axis[0]:(r==2&&c==0)?-axis[1]:(r==2&&c==1)?axis[0]:0;
        step[r][c]=cos(angle)*(r==c)+(1-cos(angle))*axis[r]*axis[c]+sin(angle)*cross;
    }
    for(int i=0;i<720;i++) {
        motion_gizmo_arcball(cos(angle/2),axis[0]*sin(angle/2),axis[1]*sin(angle/2),axis[2]*sin(angle/2));
        double next[3][3]={};
        for(int r=0;r<3;r++)for(int c=0;c<3;c++)for(int k=0;k<3;k++)next[r][c]+=step[r][k]*expected[k][c];
        memcpy(expected,next,sizeof next);
        Mat target;euler_mat(game_turn(virtual_yaw()),virtual_pitch(),virtual_roll(),target);
        for(int r=0;r<3;r++)for(int c=0;c<3;c++)assert(fabs(target[r][c]-expected[r][c])<2e-5);
        update(clock_ns+=10000000);
    }
    for(int i=0;i<40;i++)update(clock_ns+=10000000);
    for(int r=0;r<3;r++)for(int c=0;c<3;c++)assert(fabs(s.m[r][c]-expected[r][c])<2e-5);
    motion_virtual_end();update(clock_ns+=10000000);orientation(0,0,0);
    puts("Arcball: arbitrary-axis quaternion composition over two turns, sensor delivery and apparatus exit reset PASS");
}
int main(void) {
    const int rates[] = {30,60,120,200,1000};
    for(unsigned k=0;k<sizeof rates/sizeof rates[0];k++) {
        fresh();
        motion_keyboard(1,0,0,false);
        for(int i=1;i<=rates[k];i++)update(clock_ns=1000000000+(uint64_t)i*1000000000/rates[k]);
        assert(fabs(g.keyboard.pitch-90)<.3 && fabs(s.pitch*180/PI-90)<.3);
        motion_keyboard(0,0,0,false);
        double saved=g.keyboard.pitch;
        update(clock_ns+=100000000);
        assert(fabs(g.keyboard.pitch-saved)<1e-8);
        SixAxisState state[16]; uint32_t handle=3; Cpu cpu={0};
        cpu.x[0]=(uintptr_t)state;cpu.x[1]=16;cpu.x[2]=(uintptr_t)&handle;
        hid_sixaxis_states(&cpu);
        assert(cpu.x[0]==16 && state[0].attributes==1);
        double acc=0;
        for(int r=0;r<3;r++) {
            assert(fabs(state[0].gyro[r])<1e-5);
            acc+=state[0].acc[r]*state[0].acc[r];
            for(int c=0;c<3;c++) {
                double dot=0;for(int j=0;j<3;j++)dot+=state[0].dir[r][j]*state[0].dir[c][j];
                assert(fabs(dot-(r==c))<1e-5);
            }
        }
        assert(fabs(acc-1)<1e-5);
        motion_keyboard(0,-1,0,false);update(clock_ns+=100000000);
        assert(fabs(g.keyboard.roll+9)<1e-8);
        motion_keyboard(0,0,0,true);update(clock_ns+=100000000);
        assert(!g.keyboard.active && fabs(s.pitch)<1e-8 && fabs(s.roll)<1e-8);
        motion_keyboard(-1,1,0,false);
        for(int i=0;i<20;i++)update(clock_ns+=100000000);
        assert(fabs(g.keyboard.pitch+180)<1e-7 && fabs(g.keyboard.roll-180)<1e-7);
        motion_reboot_reset();assert(!g.keyboard.active && !g.key_pitch && !g.key_roll);
        full_turns(rates[k]);
        printf("%d Hz: HID orientation, gravity, gyro, release hold, reset, continuous rotation and reboot PASS\n",rates[k]);
    }
    mouse_turns();
    fast_mouse();
    arcball_composition();
}
