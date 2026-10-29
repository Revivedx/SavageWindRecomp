// (test) A player without a game for online play (runtime/online.c): joins a session and walks in a circle around the
// first other player it hears from, so a single copy of the game shows someone in the world. Says hello in the chat.
//
//   cc -O2 -o build/online_bot tools/online_bot.c [-lws2_32]
//   build/online_bot [host] [port] [name] [radius]       (127.0.0.1 52520 Bot 4)
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define sleep_ms(ms) Sleep(ms)
#else
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define closesocket close
#define sleep_ms(ms) usleep((ms) * 1000)
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct { float pos[3], yaw; uint8_t flags; uint16_t ping_ms; uint8_t anim; } WireState;
typedef struct { uint32_t magic; uint16_t version; char name[24], pass[24]; } WireHello;
#pragma pack(pop)
enum { M_HELLO = 1, M_WELCOME, M_REJECT, M_STATE, M_JOIN, M_LEAVE, M_CHAT, M_PING, M_PONG };

static void msg(sock_t s, uint8_t type, const void *p, int n) {
    uint8_t b[600];
    b[0] = (uint8_t)(n + 1), b[1] = (uint8_t)((n + 1) >> 8), b[2] = type;
    memcpy(b + 3, p, n);
    send(s, (const char *)b, n + 3, 0);
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1", *port = argc > 2 ? argv[2] : "52520";
    const char *name = argc > 3 ? argv[3] : "Bot";
    float radius = argc > 4 ? (float)atof(argv[4]) : 4;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
    if (getaddrinfo(host, port, &hints, &res)) { fprintf(stderr, "bot: can't resolve %s\n", host); return 1; }
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(s, res->ai_addr, (int)res->ai_addrlen)) { fprintf(stderr, "bot: can't connect to %s:%s\n", host, port); return 1; }
    WireHello h = {.magic = 0x4c4f5753u, .version = 2};
    snprintf(h.name, sizeof h.name, "%s", name);
    msg(s, M_HELLO, &h, sizeof h);
    float centre[3] = {0, 0, 0};
    int have = 0, id = -1, said = 0;
    uint8_t buf[8192];
    int n = 0;
    for (double t = 0;; t += 0.05) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, 0};
        while (select((int)s + 1, &rd, NULL, NULL, &tv) > 0) {
            int k = recv(s, (char *)buf + n, (int)sizeof buf - n, 0);
            if (k <= 0) { fprintf(stderr, "bot: disconnected\n"); return 0; }
            n += k;
            int o = 0;
            while (n - o >= 3) {
                int l = buf[o] | buf[o + 1] << 8;
                if (n - o - 2 < l) break;
                uint8_t ty = buf[o + 2], *p = buf + o + 3;
                if (ty == M_WELCOME) id = p[0], fprintf(stderr, "bot: in, slot %d\n", id);
                if (ty == M_REJECT) { fprintf(stderr, "bot: refused (%d)\n", p[0]); return 1; }
                if (ty == M_JOIN) fprintf(stderr, "bot: %.24s is slot %d\n", (const char *)p + 1, p[0]);
                if (ty == M_CHAT) fprintf(stderr, "bot: chat from %d: %s\n", p[0], (const char *)p + 1);
                if (ty == M_STATE && p[0] != id) {
                    WireState ws;
                    memcpy(&ws, p + 1, sizeof ws);
                    if (!(ws.flags & 2)) {
                        if (!have) fprintf(stderr, "bot: following slot %d at %.1f %.1f %.1f\n", p[0], ws.pos[0], ws.pos[1], ws.pos[2]);
                        for (int i = 0; i < 3; i++) centre[i] += (ws.pos[i] - centre[i]) * (have ? 0.05f : 1);
                        have = 1;
                    }
                }
                o += 2 + l;
            }
            memmove(buf, buf + o, n - o);
            n -= o;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
        }
        if (id >= 0) {
            WireState ws = {0};
            float a = (float)(t * 0.6);
            ws.pos[0] = centre[0] + sinf(a) * radius, ws.pos[1] = centre[1], ws.pos[2] = centre[2] + cosf(a) * radius;
            ws.yaw = a + 1.5707963f;
            ws.flags = have ? 0 : 2;
            ws.anim = 1;  // (walking: 4 m around at 0.6 rad/s is 2.4 m/s)
            msg(s, M_STATE, &ws, sizeof ws);
            if (have && !said) {
                said = 1;
                const char *hi = "hola! soy un bot de prueba";
                msg(s, M_CHAT, hi, (int)strlen(hi) + 1);
            }
        }
        sleep_ms(50);
    }
}
