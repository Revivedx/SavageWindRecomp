// Focused Linux tests; include motion.c to exercise the real fusion and source switching without game assets.
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>
#include "../runtime/hle/motion.c"

Settings g_settings;
static uint64_t clock_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return clock_ns; }
bool pc_gyro_keyboard_active(void) { return false; }  // (runtime/pc_gyro_scene.c: no shrine apparatus here)

static void checksum(uint8_t *p, size_t n) {
    memset(p + 8, 0, 4);
    uint32_t crc = crc32(0, p, n);  // independent zlib oracle
    for (int i = 0; i < 4; i++) p[8+i] = crc >> (i*8);
}
static void u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (i*8); }
static void f32(uint8_t *p, float f) { uint32_t v; memcpy(&v, &f, 4); u32(p, v); }
static void packet(uint8_t p[100], uint32_t type, uint32_t seq, uint64_t ts) {
    memset(p, 0, 100);
    memcpy(p, "DSUS", 4);
    p[4] = 0xe9, p[5] = 3;
    p[6] = type == 0x100001 ? 16 : 84;
    u32(p + 12, 42); u32(p + 16, type);
    p[21] = p[22] = 2; p[31] = 1;
    u32(p + 32, seq); u32(p + 68, ts); u32(p + 72, ts >> 32);
    f32(p + 80, -1); // flat DSU gravity
    checksum(p, p[6] + 16);
}
static void near(double a, double b) { assert(fabs(a-b) < 1e-6); }

static void parsing(void) {
    uint8_t p[101], req[28]; DsuPacket d;
    packet(p, 0x100002, 7, 123456);
    assert(dsu_decode(p, 100, &d));
    assert(d.sequence == 7 && d.timestamp == 123456 && d.server == 42);
    near(d.acc[0], 0); near(d.acc[1], 0); near(d.acc[2], -1);
    // Independent expected mapping, including each gyro axis and a tilted acceleration.
    f32(p+76, .25); f32(p+80, -.5); f32(p+84, .75);
    f32(p+88, 180); f32(p+92, 90); f32(p+96, -45); checksum(p, 100);
    assert(dsu_decode(p, 100, &d));
    near(d.acc[0], .25); near(d.acc[1], -.75); near(d.acc[2], -.5);
    near(d.gyro[0], PI); near(d.gyro[1], -PI/4); near(d.gyro[2], -PI/2);
    for (int n = 0; n < 100; n++) assert(!dsu_decode(p, n, &d));
    p[100] = 99; assert(dsu_decode(p, 101, &d)); // protocol permits trailing bytes
    for (int i = 0; i < 100; i++) { p[i] ^= 0x80; assert(!dsu_decode(p, 100, &d)); p[i] ^= 0x80; }
    f32(p+88, NAN); checksum(p,100); assert(!dsu_decode(p,100,&d));
    f32(p+88, INFINITY); checksum(p,100); assert(!dsu_decode(p,100,&d));
    packet(p,0x100002,1,100); p[20]=4; checksum(p,100); assert(!dsu_decode(p,100,&d));
    p[20]=0; p[6]=83; checksum(p,99); assert(!dsu_decode(p,100,&d));
    // Captured SteamDeckGyroDSU 2.1 version-reply layout (padding included in CRC, type zero).
    memset(p,0,24); memcpy(p,"DSUS",4); p[4]=0xe9; p[5]=3; p[6]=6; p[20]=0xe9; p[21]=3;
    checksum(p,24); assert(dsu_decode(p,24,&d) && d.type==0x100000);
    p[23]=1; assert(!dsu_decode(p,24,&d)); p[23]=0;
    u32(p+16,0x100000); checksum(p,22); assert(dsu_decode(p,22,&d));
    for (uint32_t type=0x100000; type<=0x100002; type++) {
        size_t n=dsu_request(req,123,type,2);
        assert(n==(type==0x100000 ? 20 : 28));
        uint32_t crc=(uint32_t)req[8] | (uint32_t)req[9]<<8 | (uint32_t)req[10]<<16 | (uint32_t)req[11]<<24;
        memset(req+8,0,4); assert(crc==crc32(0,req,n));
        assert(req[12]==123 && req[6]==n-16);
        if (type==0x100001) assert(req[20]==4 && req[27]==3);
        if (type==0x100002) assert(req[20]==1 && req[21]==2);
    }
}

static void freshness(void) {
    DsuClient c={.fd=-1,.slot=0,.server=42}; DsuPacket d; uint8_t p[100];
    packet(p,0x100002,UINT32_MAX,100); assert(dsu_decode(p,100,&d));
    assert(dsu_accept(&c,&d,clock_ns));
    assert(!dsu_accept(&c,&d,clock_ns+1)); // duplicate must not keep motion alive
    d.sequence=0; d.timestamp++; assert(dsu_accept(&c,&d,clock_ns+2)); // wrap
    d.sequence=UINT32_MAX; d.timestamp++; assert(!dsu_accept(&c,&d,clock_ns+3));
    d.sequence=1; d.timestamp=c.timestamp; assert(!dsu_accept(&c,&d,clock_ns+4));
    dsu_expire(&c,clock_ns+2+DSU_TIMEOUT_NS-1); assert(c.fresh);
    dsu_expire(&c,clock_ns+2+DSU_TIMEOUT_NS); assert(!c.fresh && c.slot==-1);
    c.slot=0; d.sequence=1; d.timestamp=1; assert(dsu_accept(&c,&d,clock_ns+DSU_RETRY_NS));
    d.slot=1; assert(!dsu_accept(&c,&d,clock_ns));
    d.slot=0; d.mac[0]=1; assert(!dsu_accept(&c,&d,clock_ns));
}

static int sample_count;
static void count_sample(const DsuPacket *p, uint32_t gen, void *u) { (void)p; (void)gen; (void)u; sample_count++; }
static void network(void) {
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0); assert(fd>=0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(fd,(struct sockaddr *)&addr,sizeof addr));
    socklen_t len=sizeof addr; assert(!getsockname(fd,(struct sockaddr *)&addr,&len));
    char endpoint[64]; snprintf(endpoint,sizeof endpoint,"127.0.0.1:%u",ntohs(addr.sin_port));
    DsuClient c; assert(dsu_init(&c,endpoint,123));
    dsu_poll(&c,clock_ns,count_sample,NULL);
    uint8_t p[100], r[128]; struct sockaddr_in client; len=sizeof client;
    assert(recvfrom(fd,r,sizeof r,0,(struct sockaddr *)&client,&len)==20); assert(r[16]==0);
    assert(recv(fd,r,sizeof r,0)==28); assert(r[16]==1);
    packet(p,0x100001,0,0); assert(sendto(fd,p,32,0,(struct sockaddr *)&client,len)==32);
    dsu_poll(&c,clock_ns+1,count_sample,NULL); assert(c.slot==0);
    assert(recv(fd,r,sizeof r,0)==28); assert(r[16]==2 && r[20]==1 && r[21]==0);
    packet(p,0x100002,10,1000); assert(sendto(fd,p,100,0,(struct sockaddr *)&client,len)==100);
    dsu_poll(&c,clock_ns+2,count_sample,NULL); assert(c.fresh && sample_count==1);
    // Restart with new server ID; discovery resets the counter/timestamp epoch.
    packet(p,0x100001,0,0); u32(p+12,99); checksum(p,32);
    sendto(fd,p,32,0,(struct sockaddr *)&client,len); dsu_poll(&c,clock_ns+3,count_sample,NULL);
    assert(c.server==99 && !c.fresh);
    packet(p,0x100002,1,1); u32(p+12,99); checksum(p,100);
    sendto(fd,p,100,0,(struct sockaddr *)&client,len); dsu_poll(&c,clock_ns+4,count_sample,NULL);
    assert(c.fresh && sample_count==2);
    dsu_poll(&c,clock_ns+DSU_TIMEOUT_NS+5,count_sample,NULL); assert(!c.fresh && c.slot==-1);
    // Same-ID restart / late server availability recovers via periodic discovery.
    packet(p,0x100001,0,0); u32(p+12,99); checksum(p,32);
    sendto(fd,p,32,0,(struct sockaddr *)&client,len); dsu_poll(&c,clock_ns+DSU_RETRY_NS,count_sample,NULL);
    packet(p,0x100002,0,0); u32(p+12,99); checksum(p,100);
    sendto(fd,p,100,0,(struct sockaddr *)&client,len); dsu_poll(&c,clock_ns+DSU_RETRY_NS+1,count_sample,NULL);
    assert(c.fresh && sample_count==3);
    dsu_close(&c); close(fd);
    assert(!dsu_init(&c,"localhost:26760",1)); assert(!dsu_init(&c,"127.0.0.1:0",1));
}

static void selection(void) {
    setenv("SAVAGEWIND_DSU","off",1);
    backend.enabled=true;
    SDL_Event e={0}; e.gsensor.sensor=SDL_SENSOR_ACCEL; e.gsensor.sensor_timestamp=100;
    e.gsensor.data[1]=SDL_STANDARD_GRAVITY;
    motion_sensor_event(&e); assert(!g.sensors);
    e.gsensor.sensor=SDL_SENSOR_GYRO; e.gsensor.data[1]=0;
    motion_sensor_event(&e); assert(backend.source==1 && g.started);
    near(g.acc[2],-1);
    // Keep upstream's native recovery for controllers with a frozen device clock.
    hs.period = .004;
    clock_ns += 4000000;
    e.gsensor.timestamp = clock_ns;
    e.gsensor.data[0] = .2f;
    motion_sensor_event(&e);
    assert(backend.source == 1 && hs.stalled && hs.fused);
    e.gsensor.data[0] = 0;
    // Invalid SDL data must not refresh the native source.
    uint64_t received = backend.gyro_time;
    clock_ns++;
    e.gsensor.timestamp++;
    e.gsensor.data[0] = NAN;
    motion_sensor_event(&e);
    assert(backend.gyro_time == received);
    e.gsensor.data[0] = 0;
    e.gsensor.timestamp = 0;
    DsuPacket d={.acc={0,0,-1},.gyro={1,2,3},.timestamp=100};
    dsu_sample(&d,1,NULL); assert(backend.source==1); // SDL priority
    clock_ns+=DSU_TIMEOUT_NS; motion_poll(); assert(!g.sensors && backend.source==0);
    dsu_sample(&d,1,NULL); assert(backend.source==2 && g.started);
    g.bias[0]=1; dsu_sample(&d,2,NULL); near(g.bias[0],0); // device epoch resets calibration
    clock_ns++;
    e.gsensor.sensor=SDL_SENSOR_ACCEL; e.gsensor.sensor_timestamp=200; e.gsensor.data[1]=SDL_STANDARD_GRAVITY;
    motion_sensor_event(&e); assert(backend.source==2); // cannot mix DSU gyro with SDL accel
    e.gsensor.sensor=SDL_SENSOR_GYRO; e.gsensor.data[1]=0;
    motion_sensor_event(&e); assert(backend.source==1);
    // Source changes must not synthesize angular velocity in the game-facing states.
    g_settings.motion_sens=1;
    update(clock_ns); select_motion(2); g.started=true; g.q=euler_quat(1,.4,.3);
    update(clock_ns+20000000);
    for(int i=0;i<3;i++) near(s.ring[s.head].gyro[i],0);
    motion_pad_closed();
    assert(!backend.enabled);
}
int main(void) { parsing(); freshness(); network(); selection(); puts("DSU tests passed"); }
