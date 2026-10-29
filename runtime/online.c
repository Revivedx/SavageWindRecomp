// Online play: the server, the client and the shared state (runtime/online.h).
//
// The wire: TCP, each message a u16 length (what follows it), a u8 type and its payload, little-endian. A client says
// HELLO (magic, protocol version, name, password); the server answers WELCOME (its slot) or REJECT (why), sends it
// JOIN for everyone already there (and their last STATE), and tells the others JOIN. From then on a client sends STATE
// 20 times a second and the server passes it on to everyone else with the sender's slot in front; CHAT the same way.
// A client that goes away is a LEAVE for the others. PING / PONG measure the round trip, which each STATE carries.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "online.h"
#include "game_sites.h"
#include "platform.h"
#include "rt.h"

#define MAGIC 0x4c4f5753u  // "SWOL"
#define PROTOCOL 6
#define SEND_HZ 20
#define INTERP_NS 100000000ull  // the others are drawn this far behind what was last heard (smooth between updates)
#define TIMEOUT_NS 10000000000ull  // nothing heard for this long: the connection is gone

enum { M_HELLO = 1, M_WELCOME, M_REJECT, M_STATE, M_JOIN, M_LEAVE, M_CHAT, M_PING, M_PONG, M_EQUIP, M_SKEL };
// SKEL: u8 Link's bone count, u8 his bow's, then each bone's rotation (smallest three of its quaternion: 2 bits which
// is dropped, 3 x 15 bits; 6 bytes) and translation (3 x s16, 1/8192 m); 20 times a second
#define SKEL_BONE_BYTES 12
#define SKEL_BYTES (2 + ONLINE_BONES * SKEL_BONE_BYTES)
enum { R_FULL = 1, R_VERSION, R_PASSWORD, R_NAME };

#pragma pack(push, 1)
typedef struct {
    float pos[3], yaw;
    uint8_t flags;
    uint16_t ping_ms;
    uint8_t anim;
    char as[ONLINE_AS_CHANNELS][ONLINE_AS_MAX];  // OnlinePose ("" all: none; the others go by anim)
    uint8_t gear_bone[ONLINE_GEAR];
    float gear_rel[ONLINE_GEAR][12];
} WireState;
typedef struct { uint32_t magic; uint16_t version; char name[ONLINE_NAME_MAX], pass[ONLINE_NAME_MAX]; } WireHello;
typedef struct { char armor[3][48], gear[ONLINE_GEAR][48]; } WireEquip;  // EQUIP: what a player wears (head,
                                                                          // upper, lower) and carries (actor names)
#pragma pack(pop)

OnlineSettings g_online = {.port = ONLINE_PORT, .show_names = true, .show_markers = true};

// ------------------------------------------------------------------ sockets
static void net_init(void) {
#ifdef _WIN32
    static bool done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
#endif
}

static void sock_nodelay(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static void sock_nonblock(sock_t s, bool on) {
#ifdef _WIN32
    u_long m = on;
    ioctlsocket(s, FIONBIO, &m);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
#endif
}

// the whole buffer, blocking (false: the connection is gone)
static bool send_all(sock_t s, const void *p, int n) {
    const char *c = p;
    while (n > 0) {
        int k = send(s, c, n, 0);
        if (k <= 0) return false;
        c += k, n -= k;
    }
    return true;
}

static bool send_msg(sock_t s, uint8_t type, const void *a, int na, const void *b, int nb) {
    uint8_t buf[3 + 2048];
    int len = 1 + na + nb;
    if (len + 2 > (int)sizeof buf) return false;
    buf[0] = (uint8_t)len, buf[1] = (uint8_t)(len >> 8), buf[2] = type;
    if (na) memcpy(buf + 3, a, na);
    if (nb) memcpy(buf + 3 + na, b, nb);
    return send_all(s, buf, len + 2);
}

// A connection's incoming bytes, cut into messages
typedef struct { uint8_t data[32768]; int n; } RecvBuf;

// read what is there into rb (false: closed or failed)
static bool recv_some(sock_t s, RecvBuf *rb) {
    int k = recv(s, (char *)rb->data + rb->n, (int)sizeof rb->data - rb->n, 0);
    if (k <= 0) return false;
    rb->n += k;
    return true;
}

// the next whole message in rb: its type, payload and length (returns false when none is complete yet)
static bool next_msg(RecvBuf *rb, int *off, uint8_t *type, const uint8_t **p, int *len) {
    int o = *off;
    if (rb->n - o < 3) return false;
    int l = rb->data[o] | rb->data[o + 1] << 8;
    if (l < 1 || rb->n - o - 2 < l) return false;
    *type = rb->data[o + 2], *p = rb->data + o + 3, *len = l - 1;
    *off = o + 2 + l;
    return true;
}

static void consumed(RecvBuf *rb, int off) {
    memmove(rb->data, rb->data + off, rb->n - off);
    rb->n -= off;
}

static void copy_name(char *dst, const char *src) {
    snprintf(dst, ONLINE_NAME_MAX, "%s", src);
    for (char *c = dst; *c; c++)
        if ((unsigned char)*c < 32) *c = '?';
}

// ------------------------------------------------------------------ the server
typedef struct {
    sock_t s;
    bool used, hello;
    RecvBuf rb;
    char name[ONLINE_NAME_MAX];
    WireState last;
    bool have_last;
    WireEquip equip;
    bool have_equip;
} SClient;

typedef struct {
    sock_t listener;
    SClient c[ONLINE_MAX];
    char password[ONLINE_NAME_MAX];
    volatile bool stop;
} Server;

static void srv_broadcast(Server *sv, int except, uint8_t type, const void *a, int na, const void *b, int nb) {
    for (int i = 0; i < ONLINE_MAX; i++)
        if (sv->c[i].used && sv->c[i].hello && i != except) send_msg(sv->c[i].s, type, a, na, b, nb);
}

static void srv_drop(Server *sv, int i) {
    SClient *c = &sv->c[i];
    sock_close(c->s);
    if (c->hello) {
        uint8_t id = (uint8_t)i;
        LOG("[online] server: %s left (slot %d)\n", c->name, i);
        c->used = false;
        srv_broadcast(sv, -1, M_LEAVE, &id, 1, NULL, 0);
    }
    c->used = c->hello = c->have_last = c->have_equip = false;
    c->rb.n = 0;
}

// one message of client i (false: drop it)
static bool srv_message(Server *sv, int i, uint8_t type, const uint8_t *p, int len) {
    SClient *c = &sv->c[i];
    uint8_t id = (uint8_t)i;
    if (!c->hello) {
        if (type != M_HELLO || len < (int)sizeof(WireHello)) return false;
        WireHello h;
        memcpy(&h, p, sizeof h);
        h.name[ONLINE_NAME_MAX - 1] = h.pass[ONLINE_NAME_MAX - 1] = 0;
        uint8_t why = h.magic != MAGIC || h.version != PROTOCOL ? R_VERSION
                    : sv->password[0] && strcmp(sv->password, h.pass) ? R_PASSWORD
                    : !h.name[0] ? R_NAME : 0;
        if (why) {
            send_msg(c->s, M_REJECT, &why, 1, NULL, 0);
            return false;
        }
        copy_name(c->name, h.name);
        c->hello = true;
        send_msg(c->s, M_WELCOME, &id, 1, NULL, 0);
        for (int k = 0; k < ONLINE_MAX; k++) {  // everyone already there, and where they are
            SClient *o = &sv->c[k];
            if (k == i || !o->used || !o->hello) continue;
            uint8_t ok = (uint8_t)k;
            send_msg(c->s, M_JOIN, &ok, 1, o->name, ONLINE_NAME_MAX);
            if (o->have_last) send_msg(c->s, M_STATE, &ok, 1, &o->last, sizeof o->last);
            if (o->have_equip) send_msg(c->s, M_EQUIP, &ok, 1, &o->equip, sizeof o->equip);
        }
        srv_broadcast(sv, -1, M_JOIN, &id, 1, c->name, ONLINE_NAME_MAX);  // (the new player too: its own name back)
        LOG("[online] server: %s joined (slot %d)\n", c->name, i);
        return true;
    }
    switch (type) {
    case M_STATE:
        if (len < (int)sizeof(WireState)) return false;
        memcpy(&c->last, p, sizeof c->last);
        c->have_last = true;
        srv_broadcast(sv, i, M_STATE, &id, 1, &c->last, sizeof c->last);
        break;
    case M_EQUIP:
        if (len < (int)sizeof(WireEquip)) return false;
        memcpy(&c->equip, p, sizeof c->equip);
        c->have_equip = true;
        srv_broadcast(sv, i, M_EQUIP, &id, 1, &c->equip, sizeof c->equip);
        break;
    case M_SKEL:
        if (len >= 2 && len <= SKEL_BYTES) srv_broadcast(sv, i, M_SKEL, &id, 1, p, len);
        break;
    case M_CHAT: {
        char text[ONLINE_CHAT_MAX];
        int n = len < ONLINE_CHAT_MAX - 1 ? len : ONLINE_CHAT_MAX - 1;
        memcpy(text, p, n);
        text[n] = 0;
        srv_broadcast(sv, -1, M_CHAT, &id, 1, text, n + 1);
        break;
    }
    case M_PING:
        send_msg(c->s, M_PONG, p, len < 8 ? len : 8, NULL, 0);
        break;
    }
    return true;
}

static bool srv_open(Server *sv, int port) {
    net_init();
    memset(sv, 0, sizeof *sv);
    for (int i = 0; i < ONLINE_MAX; i++) sv->c[i].s = SOCK_BAD;
    sv->listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);  // (both IPv6 and IPv4 where the OS allows)
    bool v6 = sv->listener != SOCK_BAD;
    if (!v6) sv->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sv->listener == SOCK_BAD) return false;
    int one = 1, zero = 0;
    setsockopt(sv->listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    int r;
    if (v6) {
        setsockopt(sv->listener, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port), .sin6_addr = in6addr_any};
        r = bind(sv->listener, (struct sockaddr *)&a, sizeof a);
    } else {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        r = bind(sv->listener, (struct sockaddr *)&a, sizeof a);
    }
    if (r != 0 || listen(sv->listener, 8) != 0) {
        sock_close(sv->listener);
        sv->listener = SOCK_BAD;
        return false;
    }
    return true;
}

// accept, read and pass on until sv->stop
static void srv_loop(Server *sv) {
    while (!sv->stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sv->listener, &rd);
        sock_t mx = sv->listener;
        for (int i = 0; i < ONLINE_MAX; i++)
            if (sv->c[i].used) {
                FD_SET(sv->c[i].s, &rd);
                if (sv->c[i].s > mx) mx = sv->c[i].s;
            }
        struct timeval tv = {0, 100000};
        int n = select((int)mx + 1, &rd, NULL, NULL, &tv);
        if (n <= 0) continue;
        if (FD_ISSET(sv->listener, &rd)) {
            sock_t s = accept(sv->listener, NULL, NULL);
            if (s != SOCK_BAD) {
                int i = 0;
                while (i < ONLINE_MAX && sv->c[i].used) i++;
                sock_nodelay(s);
                if (i == ONLINE_MAX) {
                    uint8_t why = R_FULL;
                    send_msg(s, M_REJECT, &why, 1, NULL, 0);
                    sock_close(s);
                } else {
                    sv->c[i] = (SClient){.s = s, .used = true};
                }
            }
        }
        for (int i = 0; i < ONLINE_MAX; i++) {
            SClient *c = &sv->c[i];
            if (!c->used || !FD_ISSET(c->s, &rd)) continue;
            if (!recv_some(c->s, &c->rb)) { srv_drop(sv, i); continue; }
            int off = 0, len;
            uint8_t type;
            const uint8_t *p;
            bool ok = true;
            while (ok && next_msg(&c->rb, &off, &type, &p, &len)) ok = srv_message(sv, i, type, p, len);
            if (!ok || (c->rb.n == (int)sizeof c->rb.data && off == 0)) { srv_drop(sv, i); continue; }
            consumed(&c->rb, off);
        }
    }
    for (int i = 0; i < ONLINE_MAX; i++)
        if (sv->c[i].used) sock_close(sv->c[i].s);
    sock_close(sv->listener);
}

static Server g_srv;
static pthread_t g_srv_thread;
static bool g_srv_running;

static void *srv_thread(void *arg) {
    srv_loop(arg);
    return NULL;
}

int online_server_main(int port, const char *password) {
    Server *sv = &g_srv;
    if (!srv_open(sv, port)) {
        fprintf(stderr, "[online] can't open a server on port %d (in use?)\n", port);
        return 1;
    }
    if (password) snprintf(sv->password, sizeof sv->password, "%s", password);
    fprintf(stderr, "[online] dedicated server on port %d, %d players%s\n", port, ONLINE_MAX,
            sv->password[0] ? ", with a password" : "");
    srv_loop(sv);
    return 0;
}

// ------------------------------------------------------------------ the client
typedef struct { uint64_t t; float pos[3], yaw; uint8_t flags, anim; OnlinePose pose; } Snap;
#define SNAPS 8
typedef struct {
    bool used;
    char name[ONLINE_NAME_MAX];
    Snap s[SNAPS];
    int ns;  // snapshots held (newest at s[(head - 1) % SNAPS])
    int head;
    uint16_t ping;
    char armor[3][48], gear[ONLINE_GEAR][48];
    struct { uint64_t t; int n, nbow; float q[ONLINE_BONES][4], tr[ONLINE_BONES][3]; } sk[4];  // (SKEL, newest at skh - 1)
    int skh, skn;
} Remote;

#define CHAT_LINES 8
#define NOTICES 6
static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_on;
    volatile bool stop;
    int state;
    bool hosting;
    int id;
    const char *status_fmt;
    char status_arg[96];
    Remote r[ONLINE_MAX];
    char chat[CHAT_LINES][ONLINE_CHAT_MAX + ONLINE_NAME_MAX + 4];
    int nchat;
    uint64_t chat_t;
    char notice[NOTICES][ONLINE_NAME_MAX];
    const char *notice_fmt[NOTICES];
    uint64_t notice_t[NOTICES];
    int nnotice;
    char outbox[4][ONLINE_CHAT_MAX];  // chat lines to send (the network thread sends them)
    int nout;
    uint16_t ping;
    // the camera (vulkan/exec.c)
    double vp[16];
    float ysign;
    uint64_t cam_t;
} g = {.lock = PTHREAD_MUTEX_INITIALIZER, .id = -1};

// The status: an English text (tr() it) with at most one %s, and what goes there
static void set_status(int state, const char *fmt, const char *arg) {
    pthread_mutex_lock(&g.lock);
    g.state = state;
    g.status_fmt = fmt;
    snprintf(g.status_arg, sizeof g.status_arg, "%s", arg ? arg : "");
    pthread_mutex_unlock(&g.lock);
    char line[200];
    snprintf(line, sizeof line, fmt, arg ? arg : "");
    LOG("[online] %s\n", line);
}

static void add_notice(const char *fmt, const char *name) {  // (lock held)
    if (g.nnotice == NOTICES) {
        memmove(g.notice, g.notice + 1, sizeof g.notice[0] * (NOTICES - 1));
        memmove(g.notice_fmt, g.notice_fmt + 1, sizeof g.notice_fmt[0] * (NOTICES - 1));
        memmove(g.notice_t, g.notice_t + 1, sizeof g.notice_t[0] * (NOTICES - 1));
        g.nnotice--;
    }
    snprintf(g.notice[g.nnotice], sizeof g.notice[0], "%s", name);
    g.notice_fmt[g.nnotice] = fmt;
    g.notice_t[g.nnotice++] = plat_now_ns();
}

// What Link is doing, from how he moves (20 samples a second): on the ground by his speed, in the air by how fast he
// rises or falls, gliding when he falls slowly while going fast. A change needs two samples in a row.
static uint8_t motion_anim(float dh, float dy) {
    float vh = dh * SEND_HZ, vy = dy * SEND_HZ;
    uint8_t a;
    if (vy < -7) a = ONLINE_A_FALL;
    else if (vy < -0.8f && vh > 3) a = ONLINE_A_GLIDE;
    else if (vy > 3) a = ONLINE_A_JUMP;
    else if (vy > 0.4f && vh < 1.2f) a = ONLINE_A_CLIMB;
    else if (vh < 0.3f) a = ONLINE_A_WAIT;
    else if (vh < 2.8f) a = ONLINE_A_WALK;
    else if (vh < 6.5f) a = ONLINE_A_RUN;
    else a = ONLINE_A_DASH;
    static uint8_t cur, cand;
    if (a == cand) cur = a;
    cand = a;
    return cur;
}

// SKEL's bones
static void skel_encode(uint8_t *o, const float q_in[4], const float t[3]) {
    float q[4] = {q_in[0], q_in[1], q_in[2], q_in[3]};
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    int big = 0;
    for (int k = 0; k < 4; k++) {
        q[k] = l > 0 ? q[k] / l : k == 3;
        if (fabsf(q[k]) > fabsf(q[big])) big = k;
    }
    float sign = q[big] < 0 ? -1.0f : 1.0f;
    uint64_t bits = (uint64_t)big;
    for (int k = 0, sh = 2; k < 4; k++) {
        if (k == big) continue;
        float v = q[k] * sign * 1.41421356f;  // (-1 .. 1)
        int iv = (int)lroundf((v < -1 ? -1 : v > 1 ? 1 : v) * 16383.0f) + 16383;
        bits |= (uint64_t)(iv & 0x7fff) << sh;
        sh += 15;
    }
    for (int k = 0; k < 6; k++) o[k] = (uint8_t)(bits >> (8 * k));
    for (int k = 0; k < 3; k++) {
        float v = t[k] * 8192.0f;
        int16_t iv = (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : lroundf(v));
        o[6 + 2 * k] = (uint8_t)iv, o[7 + 2 * k] = (uint8_t)((uint16_t)iv >> 8);
    }
}

static void skel_decode(const uint8_t *o, float q[4], float t[3]) {
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++) bits |= (uint64_t)o[k] << (8 * k);
    int big = (int)(bits & 3);
    float sum = 0;
    for (int k = 0, sh = 2; k < 4; k++) {
        if (k == big) continue;
        q[k] = ((float)((bits >> sh) & 0x7fff) - 16383.0f) / 16383.0f / 1.41421356f;
        sum += q[k] * q[k];
        sh += 15;
    }
    q[big] = sqrtf(sum < 1 ? 1 - sum : 0);
    for (int k = 0; k < 3; k++) t[k] = (float)(int16_t)(o[6 + 2 * k] | o[7 + 2 * k] << 8) / 8192.0f;
}

// the local Link's skeleton, encoded (online_set_local_skeleton, the frame loop's thread)
static uint8_t g_skel[SKEL_BYTES];
static int g_skel_len;
static uint64_t g_skel_t;

void online_set_local_skeleton(int n, int nbow, const float (*q)[4], const float (*t)[3]) {
    uint8_t buf[SKEL_BYTES];
    if (nbow > ONLINE_BOW_BONES) nbow = ONLINE_BOW_BONES;
    if (n > ONLINE_BONES - nbow) n = ONLINE_BONES - nbow;
    buf[0] = (uint8_t)n, buf[1] = (uint8_t)nbow;
    for (int b = 0; b < n + nbow; b++) skel_encode(buf + 2 + b * SKEL_BONE_BYTES, q[b], t[b]);
    pthread_mutex_lock(&g.lock);
    memcpy(g_skel, buf, 2 + (n + nbow) * SKEL_BONE_BYTES);
    g_skel_len = 2 + (n + nbow) * SKEL_BONE_BYTES;
    g_skel_t = plat_now_ns();
    pthread_mutex_unlock(&g.lock);
}

bool online_skeleton(int slot, int *n, int *nbow, float (*q)[4], float (*t)[3]) {
    if (slot < 0 || slot >= ONLINE_MAX) return false;
    uint64_t now = plat_now_ns(), at = now - INTERP_NS;
    bool ok = false;
    pthread_mutex_lock(&g.lock);
    Remote *r = &g.r[slot];
    if (r->used && r->skn) {
        int newest = (r->skh + 3) % 4;
        if (now - r->sk[newest].t < 500000000ull) {
            int a = newest, b = newest;  // (the two around at, or the newest)
            for (int k = 1; k < r->skn; k++) {
                int nb = (r->skh + 4 - k) % 4, na = (r->skh + 4 - k - 1) % 4;
                if (r->sk[na].t <= at && r->sk[nb].t >= at) { a = na, b = nb; break; }
            }
            float f = r->sk[b].t > r->sk[a].t ? (float)(at - r->sk[a].t) / (float)(r->sk[b].t - r->sk[a].t) : 1;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            if (r->sk[a].n != r->sk[b].n || r->sk[a].nbow != r->sk[b].nbow) a = b, f = 1;  // (another bow)
            int nn = r->sk[b].n + r->sk[b].nbow;
            for (int bo = 0; bo < nn; bo++) {
                const float *qa = r->sk[a].q[bo], *qb = r->sk[b].q[bo];
                float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3], s = d < 0 ? -1.0f : 1.0f, l = 0;
                for (int k = 0; k < 4; k++) q[bo][k] = qa[k] + (qb[k] * s - qa[k]) * f, l += q[bo][k] * q[bo][k];
                l = l > 0 ? 1 / sqrtf(l) : 1;
                for (int k = 0; k < 4; k++) q[bo][k] *= l;
                for (int k = 0; k < 3; k++) t[bo][k] = r->sk[a].tr[bo][k] + (r->sk[b].tr[bo][k] - r->sk[a].tr[bo][k]) * f;
            }
            *n = r->sk[b].n, *nbow = r->sk[b].nbow;
            ok = nn > 0;
        }
    }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

// the local Link's pose (online_set_local_pose, the frame loop's thread)
static pthread_mutex_t g_pose_lock = PTHREAD_MUTEX_INITIALIZER;
static OnlinePose g_pose;
static uint64_t g_pose_t;

void online_set_local_pose(const OnlinePose *p) {
    pthread_mutex_lock(&g_pose_lock);
    g_pose = *p;
    g_pose_t = plat_now_ns();
    pthread_mutex_unlock(&g_pose_lock);
}

// Link right now: false while there is no Link (loading, the title screen)
static bool sample_local(WireState *w) {
    static float last[3], yaw;
    static bool have;
    w->flags = 0;
    w->anim = ONLINE_A_WAIT;
    if (g_site.pause_state && *(uint32_t *)(uintptr_t)g_site.pause_state) w->flags |= ONLINE_F_PAUSED;
    uint64_t pinfo = g_site.player_info ? *(uint64_t *)(uintptr_t)g_site.player_info : 0;
    if (!pinfo) { have = false; return false; }
    memcpy(w->pos, (const void *)(uintptr_t)(pinfo + 0x88), sizeof w->pos);
    for (int k = 0; k < 3; k++)
        if (!isfinite(w->pos[k]) || fabsf(w->pos[k]) > 100000) { have = false; return false; }
    if (w->pos[0] == 0 && w->pos[1] == 0 && w->pos[2] == 0) { have = false; return false; }  // (no Link placed yet)
    static uint8_t anim;
    if (have) {  // heading: where he has been going (kept while he stands)
        float dx = w->pos[0] - last[0], dy = w->pos[1] - last[1], dz = w->pos[2] - last[2];
        if (dx * dx + dz * dz > 0.02f * 0.02f) yaw = atan2f(dx, dz);
        if (dx * dx + dy * dy + dz * dz < 50 * 50) anim = motion_anim(sqrtf(dx * dx + dz * dz), dy);
    }
    memcpy(last, w->pos, sizeof last);
    have = true;
    w->yaw = yaw;
    w->anim = anim;
    pthread_mutex_lock(&g_pose_lock);  // (a pose of the last half second: the frame loop runs)
    if (g_pose.valid && plat_now_ns() - g_pose_t < 500000000ull) {
        memcpy(w->as, g_pose.as, sizeof w->as);
        memcpy(w->gear_bone, g_pose.gear_bone, sizeof w->gear_bone);
        memcpy(w->gear_rel, g_pose.gear_rel, sizeof w->gear_rel);
        w->yaw = g_pose.yaw;
    }
    pthread_mutex_unlock(&g_pose_lock);
    return true;
}

// What Link wears and carries (runtime/online_actors.c reads it from his actor links on the frame loop)
static WireEquip g_equip;
static uint64_t g_equip_t;

void online_set_local_equip(const char armor[3][48], const char gear[ONLINE_GEAR][48]) {
    pthread_mutex_lock(&g_pose_lock);
    memcpy(g_equip.armor, armor, sizeof g_equip.armor);
    memcpy(g_equip.gear, gear, sizeof g_equip.gear);
    g_equip_t = plat_now_ns();
    pthread_mutex_unlock(&g_pose_lock);
}

static bool sample_equip(WireEquip *e) {
    pthread_mutex_lock(&g_pose_lock);
    bool ok = g_equip_t && plat_now_ns() - g_equip_t < 500000000ull;
    if (ok) *e = g_equip;
    pthread_mutex_unlock(&g_pose_lock);
    return ok;
}

static void remote_snap(int id, const WireState *w) {  // (lock held)
    Remote *r = &g.r[id];
    Snap *s = &r->s[r->head];
    s->t = plat_now_ns();
    memcpy(s->pos, w->pos, sizeof s->pos);
    s->yaw = w->yaw, s->flags = w->flags, s->anim = w->anim < ONLINE_A_COUNT ? w->anim : ONLINE_A_WAIT;
    memcpy(s->pose.as, w->as, sizeof s->pose.as);
    for (int k = 0; k < ONLINE_AS_CHANNELS; k++) s->pose.as[k][ONLINE_AS_MAX - 1] = 0;
    memcpy(s->pose.gear_bone, w->gear_bone, sizeof s->pose.gear_bone);
    memcpy(s->pose.gear_rel, w->gear_rel, sizeof s->pose.gear_rel);
    s->pose.yaw = w->yaw;
    s->pose.valid = w->as[0][0] != 0;
    r->head = (r->head + 1) % SNAPS;
    if (r->ns < SNAPS) r->ns++;
    r->ping = w->ping_ms;
}

// a message from the server (false: the session ends)
static bool cl_message(uint8_t type, const uint8_t *p, int len) {
    pthread_mutex_lock(&g.lock);
    bool ok = true;
    switch (type) {
    case M_WELCOME:
        if (len >= 1 && p[0] < ONLINE_MAX) g.id = p[0], g.state = ONLINE_CONNECTED;
        g.status_fmt = g.hosting ? "Hosting" : "Connected";
        break;
    case M_REJECT: {
        int why = len ? p[0] : 0;
        g.status_fmt = (why == R_FULL ? "The session is full" : why == R_VERSION ? "Another version of Savage Wind"
                 : why == R_PASSWORD ? "Wrong password" : why == R_NAME ? "Choose a name first" : "Refused by the server");
        g.status_arg[0] = 0;
        g.state = ONLINE_FAILED;
        ok = false;
        break;
    }
    case M_JOIN:
        if (len >= 1 + ONLINE_NAME_MAX && p[0] < ONLINE_MAX) {
            Remote *r = &g.r[p[0]];
            bool was = r->used;
            memset(r, 0, sizeof *r);
            r->used = true;
            memcpy(r->name, p + 1, ONLINE_NAME_MAX);
            r->name[ONLINE_NAME_MAX - 1] = 0;
            if (!was && p[0] != g.id) add_notice("%s joined", r->name);
        }
        break;
    case M_LEAVE:
        if (len >= 1 && p[0] < ONLINE_MAX && g.r[p[0]].used) {
            add_notice("%s left", g.r[p[0]].name);
            g.r[p[0]].used = false;
        }
        break;
    case M_STATE:
        if (len >= 1 + (int)sizeof(WireState) && p[0] < ONLINE_MAX && g.r[p[0]].used) {
            WireState w;
            memcpy(&w, p + 1, sizeof w);
            remote_snap(p[0], &w);
        }
        break;
    case M_SKEL:
        if (len >= 2 && p[0] < ONLINE_MAX && g.r[p[0]].used) {
            Remote *r = &g.r[p[0]];
            int n = p[1], nbow = len >= 3 ? p[2] : 0;
            if (n + nbow > ONLINE_BONES || len < 3 + (n + nbow) * SKEL_BONE_BYTES) break;
            r->sk[r->skh].t = plat_now_ns();
            r->sk[r->skh].n = n, r->sk[r->skh].nbow = nbow;
            for (int b = 0; b < n + nbow; b++)
                skel_decode(p + 3 + b * SKEL_BONE_BYTES, r->sk[r->skh].q[b], r->sk[r->skh].tr[b]);
            r->skh = (r->skh + 1) % 4;
            if (r->skn < 4) r->skn++;
        }
        break;
    case M_EQUIP:
        if (len >= 1 + (int)sizeof(WireEquip) && p[0] < ONLINE_MAX && g.r[p[0]].used) {
            WireEquip e;
            memcpy(&e, p + 1, sizeof e);
            memcpy(g.r[p[0]].armor, e.armor, sizeof e.armor);
            memcpy(g.r[p[0]].gear, e.gear, sizeof e.gear);
            for (int k = 0; k < 3; k++) g.r[p[0]].armor[k][47] = 0;
            for (int k = 0; k < ONLINE_GEAR; k++) g.r[p[0]].gear[k][47] = 0;
        }
        break;
    case M_CHAT:
        if (len >= 2 && p[0] < ONLINE_MAX) {
            if (g.nchat == CHAT_LINES) memmove(g.chat, g.chat + 1, sizeof g.chat[0] * (CHAT_LINES - 1)), g.nchat--;
            char text[ONLINE_CHAT_MAX];
            int n = len - 1 < ONLINE_CHAT_MAX - 1 ? len - 1 : ONLINE_CHAT_MAX - 1;
            memcpy(text, p + 1, n);
            text[n] = 0;
            snprintf(g.chat[g.nchat++], sizeof g.chat[0], "%s: %s", g.r[p[0]].used ? g.r[p[0]].name : "?", text);
            g.chat_t = plat_now_ns();
        }
        break;
    case M_PONG:
        if (len >= 8) {
            uint64_t t;
            memcpy(&t, p, 8);
            uint64_t ms = (plat_now_ns() - t) / 1000000;
            g.ping = (uint16_t)(ms > 65535 ? 65535 : ms);
        }
        break;
    }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

// connect with a time limit (host: a name or an address; IPv4 or IPv6)
static sock_t cl_connect(const char *host, int port) {
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP}, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return SOCK_BAD;
    sock_t s = SOCK_BAD;
    for (struct addrinfo *a = res; a && s == SOCK_BAD && !g.stop; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SOCK_BAD) continue;
        sock_nonblock(s, true);
        connect(s, a->ai_addr, (int)a->ai_addrlen);
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        struct timeval tv = {6, 0};
        int err = 1;
        socklen_t el = sizeof err;
        if (select((int)s + 1, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(s, &ex) ||
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0 || err != 0) {
            sock_close(s);
            s = SOCK_BAD;
            continue;
        }
        sock_nonblock(s, false);
        sock_nodelay(s);
    }
    freeaddrinfo(res);
    return s;
}

static void *cl_thread(void *arg) {
    (void)arg;
    char host[64];
    int port;
    pthread_mutex_lock(&g.lock);
    snprintf(host, sizeof host, "%s", g.hosting ? "127.0.0.1" : g_online.host);
    port = g_online.port;
    pthread_mutex_unlock(&g.lock);
    char where[96];
    snprintf(where, sizeof where, "%s:%d", host, port);
    set_status(ONLINE_CONNECTING, "Connecting to %s...", where);
    sock_t s = cl_connect(host, port);
    if (s == SOCK_BAD) {
        if (!g.stop) set_status(ONLINE_FAILED, "Can't reach %s", where);
        return NULL;
    }
    WireHello h = {.magic = MAGIC, .version = PROTOCOL};
    copy_name(h.name, g_online.name[0] ? g_online.name : "Link");
    snprintf(h.pass, sizeof h.pass, "%s", g_online.password);
    send_msg(s, M_HELLO, &h, sizeof h, NULL, 0);
    RecvBuf rb = {0};
    uint64_t next_send = 0, next_ping = 0, heard = plat_now_ns();
    bool alive = true;
    while (!g.stop && alive) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, 10000};
        if (select((int)s + 1, &rd, NULL, NULL, &tv) > 0) {
            if (!recv_some(s, &rb)) { alive = false; break; }
            heard = plat_now_ns();
            int off = 0, len;
            uint8_t type;
            const uint8_t *p;
            while (alive && next_msg(&rb, &off, &type, &p, &len)) alive = cl_message(type, p, len);
            consumed(&rb, off);
        }
        uint64_t now = plat_now_ns();
        if (now - heard > TIMEOUT_NS) {
            set_status(ONLINE_FAILED, "Lost the connection", NULL);
            break;
        }
        if (g.state != ONLINE_CONNECTED) continue;
        if (now >= next_send) {
            next_send = now + 1000000000ull / SEND_HZ;
            WireState w = {0};
            if (!sample_local(&w)) w.flags |= ONLINE_F_LOADING;
            w.ping_ms = g.ping;
            if (!send_msg(s, M_STATE, &w, sizeof w, NULL, 0)) alive = false;
            uint8_t sk[SKEL_BYTES];
            int skl = 0;
            pthread_mutex_lock(&g.lock);
            if (g_skel_len && now - g_skel_t < 500000000ull) memcpy(sk, g_skel, skl = g_skel_len);
            pthread_mutex_unlock(&g.lock);
            if (skl && !(w.flags & ONLINE_F_LOADING) && !send_msg(s, M_SKEL, sk, skl, NULL, 0)) alive = false;
        }
        if (now >= next_ping) {
            next_ping = now + 1000000000ull;
            send_msg(s, M_PING, &now, 8, NULL, 0);
        }
        {   // what he wears: when it changes, and every 5 s (for whoever came in meanwhile)
            static WireEquip last;
            static uint64_t next_equip;
            WireEquip e;
            if (sample_equip(&e) && (memcmp(&e, &last, sizeof e) || now >= next_equip)) {
                next_equip = now + 5000000000ull;
                if (memcmp(&e, &last, sizeof e))
                    LOG("[online] wearing %s|%s|%s carrying %s|%s|%s|%s|%s\n", e.armor[0], e.armor[1], e.armor[2], e.gear[0],
                        e.gear[1], e.gear[2], e.gear[3], e.gear[4]);
                last = e;
                send_msg(s, M_EQUIP, &e, sizeof e, NULL, 0);
            }
        }
        pthread_mutex_lock(&g.lock);
        char out[4][ONLINE_CHAT_MAX];
        int nout = g.nout;
        memcpy(out, g.outbox, sizeof out);
        g.nout = 0;
        pthread_mutex_unlock(&g.lock);
        for (int i = 0; i < nout; i++) send_msg(s, M_CHAT, out[i], (int)strlen(out[i]) + 1, NULL, 0);
    }
    sock_close(s);
    if (!g.stop && g.state == ONLINE_CONNECTED) set_status(ONLINE_FAILED, "Disconnected by the server", NULL);
    return NULL;
}

static void stop_all(void) {
    g.stop = true;
    if (g.thread_on) pthread_join(g.thread, NULL), g.thread_on = false;
    if (g_srv_running) {
        g_srv.stop = true;
        pthread_join(g_srv_thread, NULL);
        g_srv_running = false;
    }
    pthread_mutex_lock(&g.lock);
    memset(g.r, 0, sizeof g.r);
    g.id = -1, g.nchat = g.nout = 0, g.hosting = false, g.ping = 0;
    pthread_mutex_unlock(&g.lock);
    g.stop = false;
}

static bool start_client(bool hosting) {
    net_init();
    pthread_mutex_lock(&g.lock);
    g.hosting = hosting;
    g.state = ONLINE_CONNECTING;
    pthread_mutex_unlock(&g.lock);
    g.thread_on = pthread_create(&g.thread, NULL, cl_thread, NULL) == 0;
    return g.thread_on;
}

bool online_host(void) {
    stop_all();
    online_settings_save();
    if (!srv_open(&g_srv, g_online.port)) {
        char port[16];
        snprintf(port, sizeof port, "%d", g_online.port);
        set_status(ONLINE_FAILED, "Can't host on port %s (in use?)", port);
        return false;
    }
    snprintf(g_srv.password, sizeof g_srv.password, "%s", g_online.password);
    g_srv_running = pthread_create(&g_srv_thread, NULL, srv_thread, &g_srv) == 0;
    if (!g_srv_running) { sock_close(g_srv.listener); return false; }
    LOG("[online] hosting on port %d\n", g_online.port);
    return start_client(true);
}

bool online_join(void) {
    stop_all();
    online_settings_save();
    if (!g_online.host[0]) { set_status(ONLINE_FAILED, "Type the host's address first", NULL); return false; }
    return start_client(false);
}

void online_leave(void) {
    stop_all();
    set_status(ONLINE_OFF, "Not connected", NULL);
}

int online_state(void) { return g.state; }
bool online_is_host(void) { return g.hosting && g_srv_running; }
int online_local_id(void) { return g.state == ONLINE_CONNECTED ? g.id : -1; }

const char *online_status(char *arg, size_t cap) {
    pthread_mutex_lock(&g.lock);
    const char *f = g.status_fmt ? g.status_fmt : "Not connected";
    snprintf(arg, cap, "%s", g.status_arg);
    pthread_mutex_unlock(&g.lock);
    return f;
}

bool online_players(OnlinePlayer out[ONLINE_MAX]) {
    memset(out, 0, sizeof(OnlinePlayer) * ONLINE_MAX);
    if (g.state != ONLINE_CONNECTED) return false;
    uint64_t t = plat_now_ns() - INTERP_NS;
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < ONLINE_MAX; i++) {
        Remote *r = &g.r[i];
        if (!r->used) continue;
        OnlinePlayer *o = &out[i];
        o->used = true;
        memcpy(o->name, r->name, sizeof o->name);
        memcpy(o->armor, r->armor, sizeof o->armor);
        memcpy(o->gear, r->gear, sizeof o->gear);
        o->ping_ms = i == g.id ? g.ping : r->ping;
        if (!r->ns) { o->flags = ONLINE_F_LOADING; continue; }
        // the two snapshots around t (or the newest one)
        const Snap *a = NULL, *b = &r->s[(r->head + SNAPS - 1) % SNAPS];
        for (int k = 1; k < r->ns; k++) {
            const Snap *n = &r->s[(r->head + SNAPS - k) % SNAPS], *p = &r->s[(r->head + SNAPS - k - 1) % SNAPS];
            if (p->t <= t && n->t >= t) { a = p, b = n; break; }
        }
        float f = a && b->t > a->t ? (float)(t - a->t) / (float)(b->t - a->t) : 1;
        if (!a || (b->flags & ONLINE_F_LOADING) || (a->flags & ONLINE_F_LOADING)) a = b, f = 1;
        float dx = b->pos[0] - a->pos[0], dy = b->pos[1] - a->pos[1], dz = b->pos[2] - a->pos[2];
        if (dx * dx + dy * dy + dz * dz > 30 * 30) a = b, f = 1;  // (a warp: no sliding across the map)
        for (int k = 0; k < 3; k++) o->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * f;
        float dyaw = remainderf(b->yaw - a->yaw, 6.2831853f);
        o->yaw = a->yaw + dyaw * f;
        o->flags = b->flags;
        o->anim = b->anim;
        o->pose = b->pose;  // (the newer one: what he does next comes a little early, never late)
        o->pose.yaw = o->yaw;
    }
    pthread_mutex_unlock(&g.lock);
    return true;
}

void online_chat_send(const char *text) {
    if (!text || !*text || g.state != ONLINE_CONNECTED) return;
    pthread_mutex_lock(&g.lock);
    if (g.nout < 4) snprintf(g.outbox[g.nout++], ONLINE_CHAT_MAX, "%s", text);
    pthread_mutex_unlock(&g.lock);
}

int online_chat_lines(char out[][ONLINE_CHAT_MAX + ONLINE_NAME_MAX + 4], int max, uint64_t *newest_ms) {
    pthread_mutex_lock(&g.lock);
    int n = g.nchat < max ? g.nchat : max;
    memcpy(out, g.chat + (g.nchat - n), sizeof g.chat[0] * n);
    if (newest_ms) *newest_ms = g.chat_t ? (plat_now_ns() - g.chat_t) / 1000000 : UINT64_MAX;
    pthread_mutex_unlock(&g.lock);
    return n;
}

int online_notices(char names[][ONLINE_NAME_MAX], const char *fmts[], uint64_t ages_ms[], int max) {
    pthread_mutex_lock(&g.lock);
    int n = g.nnotice < max ? g.nnotice : max;
    uint64_t now = plat_now_ns();
    for (int i = 0; i < n; i++) {
        int k = g.nnotice - n + i;
        memcpy(names[i], g.notice[k], sizeof g.notice[0]);
        fmts[i] = g.notice_fmt[k];
        ages_ms[i] = (now - g.notice_t[k]) / 1000000;
    }
    pthread_mutex_unlock(&g.lock);
    return n;
}

void online_local_addresses(char *out, size_t cap) {
    net_init();
    out[0] = 0;
    char hn[256];
    if (gethostname(hn, sizeof hn) != 0) return;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(hn, NULL, &hints, &res) != 0) return;
    size_t n = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        char ip[64];
        if (!inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof ip) || !strncmp(ip, "127.", 4)) continue;
        int k = snprintf(out + n, cap - n, "%s%s", n ? ", " : "", ip);
        if (k < 0 || (size_t)k >= cap - n) break;
        n += (size_t)k;
    }
    freeaddrinfo(res);
}

// ------------------------------------------------------------------ the camera
bool online_wants_camera(void) { return g.state == ONLINE_CONNECTED && g_online.show_markers; }

void online_camera_set(const double vp[16], float ysign) {
    pthread_mutex_lock(&g.lock);
    memcpy(g.vp, vp, sizeof g.vp);
    g.ysign = ysign;
    g.cam_t = plat_now_ns();
    pthread_mutex_unlock(&g.lock);
}

bool online_camera_get(double vp[16], float *ysign, uint64_t *age_ns) {
    pthread_mutex_lock(&g.lock);
    bool ok = g.cam_t != 0;
    memcpy(vp, g.vp, sizeof g.vp);
    *ysign = g.ysign;
    *age_ns = ok ? plat_now_ns() - g.cam_t : UINT64_MAX;
    pthread_mutex_unlock(&g.lock);
    return ok;
}

// ------------------------------------------------------------------ settings (online.ini)
static void name_from_env(void);

void online_settings_load(void) {
    const char *u = getenv("USERNAME");
    if (!u || !*u) u = getenv("USER");
    copy_name(g_online.name, u && *u ? u : "Link");
    FILE *f = fopen("online.ini", "r");
    if (!f) { name_from_env(); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[128] = "";
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %127[^\r\n]", key, val) < 1) continue;
        if (!strcmp(key, "name")) copy_name(g_online.name, val);
        else if (!strcmp(key, "host")) snprintf(g_online.host, sizeof g_online.host, "%s", val);
        else if (!strcmp(key, "port")) { int p = atoi(val); if (p > 0 && p < 65536) g_online.port = p; }
        else if (!strcmp(key, "password")) snprintf(g_online.password, sizeof g_online.password, "%s", val);
        else if (!strcmp(key, "show_names")) g_online.show_names = atoi(val) != 0;
        else if (!strcmp(key, "show_markers")) g_online.show_markers = atoi(val) != 0;
    }
    fclose(f);
    name_from_env();
}

// SAVAGEWIND_ONLINE_NAME: this run's name (two copies of the game on one machine)
static void name_from_env(void) {
    const char *n = getenv("SAVAGEWIND_ONLINE_NAME");
    if (n && *n) copy_name(g_online.name, n);
}

void online_settings_save(void) {
    const char *n = getenv("SAVAGEWIND_ONLINE_NAME");
    if (n && *n && !strcmp(g_online.name, n)) return;  // (a test run's name: online.ini stays as it was)
    FILE *f = fopen("online.ini", "w");
    if (!f) return;
    fprintf(f, "# Savage Wind online settings (Settings > Online in game)\n");
    fprintf(f, "name = %s\nhost = %s\nport = %d\npassword = %s\nshow_names = %d\nshow_markers = %d\n", g_online.name,
            g_online.host, g_online.port, g_online.password, g_online.show_names, g_online.show_markers);
    fclose(f);
}
