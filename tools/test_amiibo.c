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
    nfp_configure(true, AMIIBO_ARCHER, false);
    assert(activate->signaled && device_state == NFP_FOUND);
    assert(!request(13, tag, sizeof tag).result && tag[6] == 1 && tag[0xa] == 7);
    assert(!request(5, NULL, 0).result);
    nfp_configure(true, AMIIBO_ZELDA, false);  // settings changes cannot change an already mounted figure
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
    // Catalog IDs are fixed fixtures, in tag order; IPC swaps only the numbering field.
    const char *catalog_ids[] = {
        "0100000003530902",
        "0101000003560902",
        "0140000003550902",
        "01410000035c0902",
        "0100000003540902",
        "0105000003580902",
        "01070000035a0902",
        "01080000035b0902",
        "0106000003590902",
        "01000000034f0902",
        "01000000034b0902",
        "01000000034c0902",
        "01000000034d0902",
        "01000000034e0902",
        "0100010003500902",
        "0101000003520902",
        "0100000000040002",
        "01010000000e0002",
        "0100010000160002",
        "0101010000170002",
        "01020100001b0002",
        "0000000000000002",
    };
    assert(sizeof catalog_ids / sizeof *catalog_ids == AMIIBO_LEGACY_COUNT);
    for (int i = 0; i < AMIIBO_COUNT; i++) {
        nfp_configure(true, i, false);
        assert(!request(4, NULL, 0).result && !request(3, NULL, 0).result);
        assert(!request(13, tag, sizeof tag).result && tag[6] == (uint8_t)(i + 1) && tag[5] == (uint8_t)((i + 1) >> 8));
        assert(!request(5, NULL, 0).result && !request(16, model, 0x40).result);
        uint8_t expected[8];
        for (int j = 0; j < 8; j++) {
            unsigned v; assert(sscanf((i < AMIIBO_LEGACY_COUNT ? catalog_ids[i] : virtual_amiibos[i].id) + j * 2, "%2x", &v) == 1); expected[j] = v;
        }
        uint8_t tmp = expected[4]; expected[4] = expected[5]; expected[5] = tmp;
        assert(!memcmp(model, expected, 7));
        for (int j = 0; j < i; j++) {
            assert(strcmp(virtual_amiibos[i].key, virtual_amiibos[j].key));
            assert(strcmp(virtual_amiibos[i].id, virtual_amiibos[j].id));
        }
    }
    nfp_configure(false, AMIIBO_ZELDA, false);
    assert(deactivate->signaled && !activate->signaled && request(16, model, 0x40).result);
    assert(!request(4, NULL, 0).result && !request(3, NULL, 0).result);
    assert(device_state == NFP_SEARCHING && !activate->signaled);
    // Repeat mode bypasses only daily UID counts. Identity and lifetime history remain unchanged.
    nfp_configure(true, AMIIBO_ARCHER, true);
    assert(!request(13, tag, sizeof tag).result);
    uint8_t first_tag[0x58]; memcpy(first_tag, tag, sizeof tag);
    const uint8_t stable_archer[] = {4, 0x53, 0x57, 1, 0, 0, 1};
    assert(!memcmp(tag, stable_archer, 7) && tag[0xa] == 7);
    assert(rt_amiibo_ignore_daily("AmiiboTouchHistory", 0));
    for (uint32_t kind = 0; kind < 4; kind++)
        assert(!rt_amiibo_ignore_daily("AmiiboTouchHistoryTotal", kind));
    assert(!rt_amiibo_ignore_daily("AmiiboTouchHistory", 1));
    assert(!rt_amiibo_ignore_daily("AmiiboLastTouchDate", 0));
    assert(!rt_amiibo_ignore_daily(NULL, 0));
    assert(!request(5, NULL, 0).result && !request(16, model, 0x40).result);
    assert(!memcmp(model, archer, sizeof archer));
    nfp_configure(true, AMIIBO_ARCHER, false);  // turning repeat off mid-scan preserves the current serial
    assert(!rt_amiibo_ignore_daily("AmiiboTouchHistory", 0));
    assert(!request(13, tag, sizeof tag).result && !memcmp(first_tag, tag, sizeof tag));
    nfp_configure(true, AMIIBO_ARCHER, true);
    assert(!request(4, NULL, 0).result && !request(3, NULL, 0).result);
    assert(!request(13, tag, sizeof tag).result && !memcmp(first_tag, tag, 7));
    memcpy(first_tag, tag, sizeof tag);
    assert(!request(1, NULL, 0).result && !request(0, NULL, 0).result);
    nfp_configure(true, AMIIBO_ARCHER, true);
    assert(!request(3, NULL, 0).result && !request(13, tag, sizeof tag).result);
    assert(!memcmp(first_tag, tag, 7));  // identity survives service reinitialization
    nfp_configure(true, AMIIBO_ARCHER, false);
    assert(!request(4, NULL, 0).result && !request(3, NULL, 0).result);
    assert(!request(13, tag, sizeof tag).result);
    assert(!memcmp(tag, stable_archer, 7));
    nfp_configure(false, AMIIBO_ARCHER, true);
    assert(!rt_amiibo_ignore_daily("AmiiboTouchHistory", 0));
    nfp_reset();
    assert(!initialized && !activate && !deactivate);
    puts("PASS: virtual amiibo discovery, live controls, stable scans, catalog and reset");
}
