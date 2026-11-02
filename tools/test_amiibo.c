// Exercise the IPC boundary: discovery while off, live enable, scan consistency and output bounds.
#include <assert.h>
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#include "../runtime/services/nfp.c"

Settings g_settings;
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KEvent events[16];
static unsigned nevents, next_handle = 0x100;
void klock(void) { pthread_mutex_lock(&g_klock); }
void kernel_signal(KObject *o) { o->signaled = true; }
uint32_t kernel_new_handle(KObject *o) { (void)o; return ++next_handle; }
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    assert(nevents < 16);
    KEvent *e = &events[nevents++];
    *w = kernel_new_handle(e); *r = kernel_new_handle(e);
    return e;
}

static IpcResp request(uint32_t cmd, void *buf, size_t size) {
    uint64_t handle = 1;
    IpcReq req = {.cmd = cmd, .data = (void *)&handle, .data_size = 8};
    if (buf) { req.nc = 1; req.c[0] = (IpcBuf){(uintptr_t)buf, size}; }
    IpcResp resp = {0};
    nfp_user(NULL, &req, &resp);
    return resp;
}

int main(void) {
    uint8_t tag[0x58], model[0x41];
    uint64_t handle = 0;
    nfp_reset();
    assert(!request(0, NULL, 0).result);
    IpcResp r = request(2, &handle, sizeof handle);
    int32_t count; memcpy(&count, r.data, sizeof count);
    assert(count == 1 && handle == 1);  // reader remains visible when virtual figures are off
    assert(!request(17, NULL, 0).result);
    assert(!request(18, NULL, 0).result);
    assert(!request(3, NULL, 0).result && device_state == NFP_SEARCHING);
    assert(!activate->signaled && request(13, tag, sizeof tag).result);
    nfp_configure(true, AMIIBO_ARCHER);
    assert(activate->signaled && device_state == NFP_FOUND);
    assert(!request(13, tag, sizeof tag).result && tag[6] == 1 && tag[0xa] == 7);
    assert(!request(5, NULL, 0).result);
    nfp_configure(true, AMIIBO_ZELDA);  // settings changes cannot change an already mounted figure
    memset(model, 0xcc, sizeof model);
    assert(!request(16, model, 0x40).result);
    const uint8_t archer[] = {1, 0, 0, 0, 0x53, 3, 9};
    assert(!memcmp(model, archer, sizeof archer) && model[0x40] == 0xcc);
    assert(request(16, model, 0x3f).result);  // reject undersized output instead of truncating
    assert(!request(4, NULL, 0).result && deactivate->signaled && !activate->signaled);
    assert(!request(3, NULL, 0).result && !deactivate->signaled);
    assert(!request(13, tag, sizeof tag).result && tag[6] == 2);
    assert(!request(5, NULL, 0).result);
    assert(!request(16, model, 0x40).result && model[1] == 1 && model[4] == 0x56);
    for (int i = AMIIBO_GUARDIAN; i < AMIIBO_COUNT; i++) {
        nfp_configure(true, i);
        assert(!request(4, NULL, 0).result && !request(3, NULL, 0).result);
        assert(!request(13, tag, sizeof tag).result && tag[6] == i + 1);
        assert(!request(5, NULL, 0).result && !request(16, model, 0x40).result);
        assert(model[1] == (i == AMIIBO_GUARDIAN ? 0x40 : 0x41));
        assert(model[4] == (i == AMIIBO_GUARDIAN ? 0x55 : 0x5c));
    }
    nfp_configure(false, AMIIBO_ZELDA);
    assert(deactivate->signaled && !activate->signaled && request(16, model, 0x40).result);
    assert(!request(4, NULL, 0).result && !request(3, NULL, 0).result);
    assert(device_state == NFP_SEARCHING && !activate->signaled);
    nfp_reset();
    assert(!initialized && !activate && !deactivate);
    puts("PASS: virtual amiibo discovery, live controls, stable scans, catalog and reset");
}
