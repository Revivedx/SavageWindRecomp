// Discord Rich Presence (discord_rpc.h). The pipe speaks frames: op and length (little endian u32 each), then JSON.
// op 0 is the handshake, op 1 a command, op 2 Discord closing the connection.
#include "discord_rpc.h"

#include <stdio.h>
#include <string.h>

bool discord_app_id_valid(const char *id) {
    if (!*id) return false;
    for (; *id; id++)
        if (*id < '0' || *id > '9') return false;
    return true;
}

// s as the inside of a JSON string; the next free byte of out, NULL when it doesn't fit
static char *json_text(char *out, const char *end, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (end - out < 3) return NULL;
        if (c == '"' || c == '\\') *out++ = '\\';
        if (c >= 0x20) *out++ = (char)c;
    }
    *out = 0;
    return out;
}

int discord_activity_json(char *out, size_t size, long pid, uint64_t start, const char *details, const char *state, unsigned nonce) {
    char d[260], s[260], *e = d + sizeof d;
    if (!json_text(d, e, details) || !json_text(s, s + sizeof s, state)) return -1;
    int n;
    if (*s)
        n = snprintf(out, size, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":{\"details\":\"%s\",\"state\":\"%s\","
                     "\"timestamps\":{\"start\":%llu}}},\"nonce\":\"%u\"}", pid, d, s, (unsigned long long)start, nonce);
    else
        n = snprintf(out, size, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":{\"details\":\"%s\","
                     "\"timestamps\":{\"start\":%llu}}},\"nonce\":\"%u\"}", pid, d, (unsigned long long)start, nonce);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

int discord_frame(uint8_t *out, size_t size, uint32_t op, const char *json) {
    size_t len = strlen(json);
    if (size < 8 + len) return -1;
    uint32_t head[2] = {op, (uint32_t)len};
    memcpy(out, head, 8);
    memcpy(out + 8, json, len);
    return (int)(8 + len);
}

#ifndef DISCORD_RPC_TEST

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "rt.h"
#include "settings.h"

#ifdef _WIN32
#include <windows.h>

#define RETRY_MS 15000
#define REPLY_MS 5000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char want_details[128], want_state[128];
static unsigned want_serial;
static bool want_on;
static char app_id[32];  // the environment's SAVAGEWIND_DISCORD_APP_ID ("" none: the feature stays off; a build
                         // ships no application of its own until the project has one)

static HANDLE pipe_open(void) {
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return NULL;
}

static bool pipe_send(HANDLE h, uint32_t op, const char *json) {
    uint8_t f[2048];
    int n = discord_frame(f, sizeof f, op, json);
    DWORD done;
    return n > 0 && WriteFile(h, f, (DWORD)n, &done, NULL) && done == (DWORD)n;
}

static bool pipe_read(HANDLE h, void *out, DWORD n) {
    DWORD done;
    return ReadFile(h, out, n, &done, NULL) && done == n;
}

// One frame, waiting REPLY_MS at most (the text cut to fit); false: nothing came, or the pipe broke
static bool pipe_receive(HANDLE h, uint32_t *op, char *text, size_t size) {
    for (int waited = 0; ; waited += 50) {
        DWORD ready = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &ready, NULL)) return false;
        if (ready >= 8) break;
        if (waited >= REPLY_MS) return false;
        Sleep(50);
    }
    uint32_t head[2];
    if (!pipe_read(h, head, 8)) return false;
    *op = head[0];
    size_t kept = 0;
    for (uint32_t left = head[1]; left;) {
        char part[512];
        DWORD n = left < sizeof part ? left : sizeof part;
        if (!pipe_read(h, part, n)) return false;
        left -= n;
        size_t take = kept + n < size ? n : size - 1 - kept;
        memcpy(text + kept, part, take);
        kept += take;
    }
    text[kept] = 0;
    return true;
}

static bool handshake(HANDLE h) {
    char json[96], reply[512] = "";
    uint32_t op = 0;
    snprintf(json, sizeof json, "{\"v\":1,\"client_id\":\"%s\"}", app_id);
    if (!pipe_send(h, 0, json) || !pipe_receive(h, &op, reply, sizeof reply)) return false;
    if (op == 2 || !strstr(reply, "\"READY\"")) {
        LOG("[discord] refused: %s\n", reply);
        return false;
    }
    return true;
}

static bool send_activity(HANDLE h, uint64_t start, const char *details, const char *state) {
    static unsigned nonce;
    char json[1024], reply[512] = "";
    uint32_t op = 0;
    if (discord_activity_json(json, sizeof json, (long)GetCurrentProcessId(), start, details, state, ++nonce) < 0) return true;
    if (!pipe_send(h, 1, json) || !pipe_receive(h, &op, reply, sizeof reply)) return false;
    if (op == 2) return false;
    if (strstr(reply, "\"ERROR\"")) LOG("[discord] %s\n", reply);
    return true;
}

static void *discord_thread(void *arg) {
    (void)arg;
    uint64_t start = (uint64_t)time(NULL);
    bool told_waiting = false;
    HANDLE h = NULL;
    unsigned sent = 0;
    for (;;) {
        pthread_mutex_lock(&lock);
        bool on = want_on;
        pthread_mutex_unlock(&lock);
        if (!on) {  // the switch went off: Discord clears the status when the pipe closes
            if (h) CloseHandle(h), h = NULL, LOG("[discord] off\n");
            Sleep(500);
            continue;
        }
        if (!h) {
            h = pipe_open();
            if (!h || !handshake(h)) {
                if (h) CloseHandle(h), h = NULL;
                if (!told_waiting) LOG("[discord] not connected, trying every %d s\n", RETRY_MS / 1000);
                told_waiting = true;
                Sleep(RETRY_MS);
                continue;
            }
            LOG("[discord] connected\n");
            told_waiting = false;
            sent = 0;  // (a new connection tells Discord the text as it is)
        }
        char details[128], state[128];
        pthread_mutex_lock(&lock);
        unsigned serial = want_serial;
        memcpy(details, want_details, sizeof details);
        memcpy(state, want_state, sizeof state);
        on = want_on;
        pthread_mutex_unlock(&lock);
        if (!on) continue;  // (the head of the loop closes the pipe)
        DWORD ready;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &ready, NULL)) {
            CloseHandle(h), h = NULL;
            LOG("[discord] connection lost\n");
            Sleep(1000);
            continue;
        }
        if (serial != sent) {
            if (!send_activity(h, start, details, state)) {
                CloseHandle(h), h = NULL;
                LOG("[discord] connection lost\n");
                Sleep(1000);
                continue;
            }
            LOG("[discord] %s | %s\n", details, state);
            sent = serial;
        }
        Sleep(1000);
    }
    return NULL;
}

void discord_rpc_set(const char *details, const char *state) {
    static bool up;  // the thread exists and an application id is set (else nothing runs, whatever the switch says)
    if (!up) {
        const char *id = getenv("SAVAGEWIND_DISCORD_APP_ID");
        if (id && *id) snprintf(app_id, sizeof app_id, "%s", id);
        if (!app_id[0]) LOG("[discord] off: no Discord application (SAVAGEWIND_DISCORD_APP_ID)\n");
        else if (!discord_app_id_valid(app_id)) {
            LOG("[discord] off: the application id is not valid\n");
            app_id[0] = 0;
        } else {
            pthread_t t;
            if (!pthread_create(&t, NULL, discord_thread, NULL)) pthread_detach(t);
            else app_id[0] = 0;
        }
        up = app_id[0] != 0;
    }
    // On by the switch (Options > Gameplay), unless the environment forces it either way (SAVAGEWIND_DISCORD=0|1)
    const char *env = getenv("SAVAGEWIND_DISCORD");
    bool on = env && *env ? atoi(env) != 0 : g_settings.discord_rpc;
    pthread_mutex_lock(&lock);
    want_on = on;
    if (on && (strcmp(want_details, details) || strcmp(want_state, state))) {
        snprintf(want_details, sizeof want_details, "%s", details);
        snprintf(want_state, sizeof want_state, "%s", state);
        want_serial++;
    }
    pthread_mutex_unlock(&lock);
}

#else

void discord_rpc_set(const char *details, const char *state) { (void)details; (void)state; }

#endif
#endif
