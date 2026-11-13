// (test) A player without a game for online play (runtime/online.c): joins a session and walks in a circle around the
// first other player it hears from, so a single copy of the game shows someone in the world: 6 s walking with its
// gear put away, then 4 s standing, sword drawn, swinging it (the player's own AS: Move, WaitAttention, CutS1, ...).
// Says hello in the chat. BOT_SWAP=1: its upper piece off and on, its sword another, every 5 s (2: once);
// BOT_BOOM=1: a Remote Bomb bomb of its going off where it stands every 10 s.
//
//   cc -O2 -o build/online_bot tools/online_bot.c [-lws2_32]
//   build/online_bot [host] [port] [name] [radius] [head upper lower] [weapon shield bow sheath quiver]
//                    (127.0.0.1 52520 Bot 4, Hylian set, Traveler's sword, shield, bow, its sheath, a quiver)
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    float pos[3], yaw;
    uint8_t flags;
    uint16_t ping_ms;
    uint8_t anim;
    char as[4][32];  // the AS its Link plays: slot 0, slot 1 banks 0 and 1, slot 2
    uint8_t gear_bone[6];  // runtime/online_actors.c k_bones + 1: 1 Weapon_R, 2 Weapon_L, 3 Pod_A, 4 Pod_B, 5 Pod_C
    float gear_rel[6][12];
    uint8_t shots[1 + 8 * 51 + 4 * 15];  // (OnlineShots: none)
    uint8_t effects;
    struct { char name[40]; float from[3], m[12]; } grab;  // (OnlineGrab: what it holds with the Magnesis rune)
    struct { char name[40]; float from[3], m[12]; } stop;  // (and what it stopped with Stasis: none)
    uint8_t ice_n;  // (its Cryonis pillars: none)
    float ice[3][12];
    char map[32];  // (the map it is on: "" unknown, as on everyone's)
    struct { uint32_t hash; char name[28]; int32_t life; } boss[2];  // (the bosses near it: none)
} WireState;
typedef struct { uint32_t magic; uint16_t version; char name[24], pass[24]; } WireHello;
#pragma pack(pop)
enum { M_HELLO = 1, M_WELCOME, M_REJECT, M_STATE, M_JOIN, M_LEAVE, M_CHAT, M_PING, M_PONG, M_EQUIP };

static void msg(sock_t s, uint8_t type, const void *p, int n) {
    uint8_t b[1100];
    b[0] = (uint8_t)(n + 1), b[1] = (uint8_t)((n + 1) >> 8), b[2] = type;
    memcpy(b + 3, p, n);
    send(s, (const char *)b, n + 3, 0);
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1", *port = argc > 2 ? argv[2] : "52520";
    const char *name = argc > 3 ? argv[3] : "Bot";
    float radius = argc > 4 ? (float)atof(argv[4]) : 4;
    fprintf(stderr, "bot: far %d boom %d swap %d\n", !!getenv("BOT_FAR"), !!getenv("BOT_BOOM"), !!getenv("BOT_SWAP"));
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
    if (getaddrinfo(host, port, &hints, &res)) { fprintf(stderr, "bot: can't resolve %s\n", host); return 1; }
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(s, res->ai_addr, (int)res->ai_addrlen)) { fprintf(stderr, "bot: can't connect to %s:%s\n", host, port); return 1; }
    WireHello h = {.magic = 0x4c4f5753u, .version = 23};
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
        // BOT_FREEZE=<s>: after that many seconds it says nothing more, its connection left open (a game that hung, a
        // network gone without closing)
        if (getenv("BOT_FREEZE") && t > atof(getenv("BOT_FREEZE"))) {
            static int said_freeze;
            if (!said_freeze++) fprintf(stderr, "bot: frozen\n");
            sleep_ms(50);
            continue;
        }
        if (id >= 0) {
            static WireState ws;
            static float a;
            double ph = fmod(t, 10);
            bool fight = ph >= 6;
            if (!fight) a = (float)(a + 0.05 * 0.6);  // (walking: 4 m around at 0.6 rad/s is 2.4 m/s)
            // BOT_FAR=1: walks away in a straight line to 150 m and back, again and again (40 s each way)
            if (getenv("BOT_FAR")) {
                double ft = fmod(t, 80);
                radius = 3 + (float)(ft < 40 ? ft : 80 - ft) * 3.7f;
                a = 0.6f;
            }
            memset(&ws, 0, sizeof ws);
            ws.pos[0] = centre[0] + sinf(a) * radius, ws.pos[1] = centre[1], ws.pos[2] = centre[2] + cosf(a) * radius;
            ws.yaw = a + 1.5707963f;
            ws.flags = have ? 0 : 2;
            ws.anim = fight ? 0 : 1;
            static const char *const swing[] = {"WaitAttention", "CutS1", "CutS2", "CutS3", "CutSF", "WaitAttention"};
            const char *as = fight ? swing[(int)((ph - 6) / 0.7) % 6] : "Move";
            snprintf(ws.as[0], 32, "%s", as);
            snprintf(ws.as[1], 32, "%s", as);
            // the gear on its bones (as it sits there: the bone's own matrix): the sword in the right hand while
            // fighting, else on the back with the shield and the bow; the sheath and quiver on the back
            static const uint8_t away[5] = {3, 4, 5, 3, 5}, drawn[5] = {1, 2, 5, 3, 5};
            for (int k = 0; k < 5; k++) {
                ws.gear_bone[k] = fight ? drawn[k] : away[k];
                ws.gear_rel[k][0] = ws.gear_rel[k][5] = ws.gear_rel[k][10] = 1;
            }
            // BOT_BOOM=1: a Remote Bomb bomb of its going off where it stands, every 10 s (sent for 2 s)
            static uint16_t boom_seq;
            static float boom_at[3];
            static double boom_t = -100;
            if (getenv("BOT_BOOM") && have && fmod(t, 10) < 0.05 && t > 15)
                boom_seq++, boom_t = t;
            if (getenv("BOT_BOOM") && have && t > 12 && fmod(t, 10) > 6) {  // (its bomb on the ground first, 4 s)
                // (around the player it follows, 4 m away, another way each time: one in view)
                float ang = 1.5707963f * (float)((int)(t / 10) % 4);
                float br = getenv("BOT_BOOM_R") ? (float)atof(getenv("BOT_BOOM_R")) : 4;
                float m[12] = {1, 0, 0, centre[0] + br * sinf(ang), 0, 1, 0, centre[1] + 0.3f, 0, 0, 1, centre[2] + br * cosf(ang)};
                uint16_t id = (uint16_t)(1 + (int)(t / 10));
                ws.shots[0] = 1, memcpy(ws.shots + 1, &id, 2), ws.shots[3] = 0, memcpy(ws.shots + 4, m, 48);
                memcpy(boom_at, m + 3, 4), boom_at[1] = m[7], boom_at[2] = m[11];
            }
            if (boom_seq && t - boom_t < 2) {
                uint8_t *b0 = ws.shots + 1 + 8 * 51;
                memcpy(b0, &boom_seq, 2), b0[2] = 0, memcpy(b0 + 3, boom_at, 12);
            }
            // BOT_ICE=x,y,z: a Cryonis pillar of its there 10 s of every 20
            ws.ice_n = 0;
            if (getenv("BOT_ICE") && t > 12 && fmod(t, 20) < 10) {
                float f[3];
                if (sscanf(getenv("BOT_ICE"), "%f,%f,%f", &f[0], &f[1], &f[2]) == 3) {
                    float m[12] = {1, 0, 0, f[0], 0, 1, 0, f[1], 0, 0, 1, f[2]};
                    ws.ice_n = 1, memcpy(ws.ice[0], m, sizeof m);
                }
            }
            // BOT_DEMO=1: in a cutscene (the others' A held back near it) 20 s of every 40
            if (getenv("BOT_DEMO") && fmod(t, 40) >= 20) ws.effects = 0x80;
            // BOT_GRAB=<actor>,x,y,z: holds that actor (the one of that name nearest x y z) with the Magnesis rune 10 s
            // of every 20: lifted 3 m over where it was, going round a 2 m circle
            memset(&ws.grab, 0, sizeof ws.grab);
            if (getenv("BOT_GRAB") && t > 12 && fmod(t, 20) < 10) {
                char gn[40] = {0};
                float f[3] = {0};
                if (sscanf(getenv("BOT_GRAB"), "%39[^,],%f,%f,%f", gn, &f[0], &f[1], &f[2]) == 4) {
                    float a = (float)fmod(t, 20) * 1.2f, up = fminf((float)fmod(t, 20), 1.5f) * 2;
                    float m[12] = {cosf(a), 0, sinf(a), f[0] + 2 * sinf(a), 0, 1, 0, f[1] + up, -sinf(a), 0, cosf(a), f[2] + 2 * cosf(a) - 2};
                    memcpy(ws.grab.name, gn, sizeof gn), memcpy(ws.grab.from, f, 12), memcpy(ws.grab.m, m, 48);
                }
            }
            msg(s, M_STATE, &ws, sizeof ws);
            if (have && !said) {
                said = 1;
                const char *hi = "hola! soy un bot de prueba";
                msg(s, M_CHAT, hi, (int)strlen(hi) + 1);
                char equip[9][48] = {{0}};  // what it wears and carries (the game's actor names)
                const char *def[8] = {"Armor_001_Head", "Armor_001_Upper", "Armor_001_Lower", "Weapon_Sword_001",
                                      "Weapon_Shield_001", "Weapon_Bow_001", "Weapon_Sheath_001", "Weapon_Quiver_001"};
                for (int k = 0; k < 8; k++) snprintf(equip[k], 48, "%s", argc > 5 + k ? argv[5 + k] : def[k]);
                msg(s, M_EQUIP, equip, sizeof equip);
            }
            // BOT_SWAP=1: every 5 s its upper piece comes off and back on, and its sword is another one meanwhile
            static double next_swap = 10;
            static int swaps;
            if (have && said && getenv("BOT_SWAP") && t >= next_swap && (atoi(getenv("BOT_SWAP")) != 2 || swaps < 2)) {
                static int phase;
                next_swap = t + (atoi(getenv("BOT_SWAP")) == 3 ? 1.5 : 5), phase ^= 1, swaps++;  // (BOT_SWAP=2: off and on once)
                char equip[9][48] = {{0}};
                const char *def[8] = {"Armor_001_Head", "Armor_001_Upper", "Armor_001_Lower", "Weapon_Sword_001",
                                      "Weapon_Shield_001", "Weapon_Bow_001", "Weapon_Sheath_001", "Weapon_Quiver_001"};
                for (int k = 0; k < 8; k++) snprintf(equip[k], 48, "%s", argc > 5 + k ? argv[5 + k] : def[k]);
                if (phase) equip[1][0] = 0, snprintf(equip[3], 48, "Weapon_Sword_002");
                if (atoi(getenv("BOT_SWAP")) == 3) {  // (3: another set every 1.5 s, mixed ones too)
                    static const char *const sets[][3] = {
                        {"Armor_001_Head", "Armor_001_Upper", "Armor_001_Lower"}, {"Armor_123_Head", "Armor_116_Upper", "Armor_102_Lower"},
                        {"", "Armor_005_Upper", ""}, {"Armor_009_Head", "Armor_104_Upper", "Armor_006_Lower"},
                        {"Armor_130_Head", "Armor_112_Upper", "Armor_113_Lower"}, {"", "", ""}, {"Armor_001_Head_B", "Armor_002_Upper", "Armor_003_Lower"}};
                    static int si;
                    si = (si + 1) % 7;
                    for (int k = 0; k < 3; k++) snprintf(equip[k], 48, "%s", sets[si][k]);
                }
                fprintf(stderr, "bot: %s, %s\n", equip[1][0] ? equip[1] : "(no upper)", equip[3]);
                msg(s, M_EQUIP, equip, sizeof equip);
            }
        }
        sleep_ms(50);
    }
}
