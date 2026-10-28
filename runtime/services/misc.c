// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u
// The add-on contents set up (services/fs.c: an image per index) are the ones installed; their data is mounted
// through fsp-srv OpenDataStorageByDataId.
// BOTW's DLC was sold only as the Expansion Pass: index 1 holds the data, index 3 is the pass itself (no data: what
// owning it unlocks). The game mounts the DLC only when the list holds two (main 0x70010cc2f8 in 1.9.0: exactly 2,
// one of them 1; a 3 turns on the second pack), so with only the data set up (aoc1, no other: a dump of the first DLC
// alone) the pass is listed too. Both DLC set up (aoc1 and aoc2) are listed as they are.
#define AOC_PASS 3
static int aoc_list(uint32_t *out, int max) {
    bool only_data = fs_aoc_present(1);
    for (int i = 2; i <= 8 && only_data; i++) only_data = !fs_aoc_present(i);
    int n = 0;
    for (int i = 1; i <= 8; i++)
        if (fs_aoc_present(i) || (i == AOC_PASS && only_data)) { if (out && n < max) out[n] = (uint32_t)i; n++; }
    return n;
}

SERVICE("aoc:u", aoc_u) {
    (void)s;
    switch (req->cmd) {
    case 0: case 2: OUT(uint32_t, 0) = (uint32_t)aoc_list(NULL, 0); resp_size(resp, 4); break;  // CountAddOnContent
    case 1: case 3: {  // ListAddOnContent(offset, count) -> B buffer of indices
        uint32_t idx[8], offset = IN(uint32_t, 0), count = IN(uint32_t, 4);
        int n = aoc_list(idx, 8), w = 0;
        IpcBuf *b = out_buf(req, 0);
        uint32_t room = b ? (uint32_t)(b->size / 4) : 0;
        if (count > room) count = room;
        for (uint32_t i = offset; i < (uint32_t)n && (uint32_t)w < count; i++)
            ((uint32_t *)(uintptr_t)b->addr)[w++] = idx[i];
        LOG("[aoc] ListAddOnContent(%u, %u): %d of %d\n", offset, count, w, n);
        OUT(uint32_t, 0) = (uint32_t)w;
        resp_size(resp, 4);
        break;
    }
    case 4: case 5: OUT(uint64_t, 0) = fs_aoc_base_id(); resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 6: case 7: LOG("[aoc] PrepareAddOnContent %u\n", IN(uint32_t, 0)); break;
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // ...ListChangedEvent
    default: ok_logger(s, req, resp);
    }
}

// ------------------------------------------------------------------ nfp:user (amiibo: no NFC reader)
// IUserManager hands out an IUser; the IUser reports no devices, so games see amiibo as unavailable instead of
// crashing on a missing object (BOTW 1.8.2 calls nn::nfp::Initialize at start-up).
static void nfp_user(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: break;                                                // Initialize / Finalize
    case 2: OUT(int32_t, 0) = 0; resp_size(resp, 4); break;               // ListDevices: none
    case 17: case 18: case 23:                                            // Attach{Activate,Deactivate,Availability}Event
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    case 19: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;            // GetState: initialized
    case 20: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;            // GetDeviceState: none
    default: LOG("[nfp] IUser cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("nfp:user", nfp_user_manager) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IUser(nfp)", nfp_user, NULL));  // CreateUserInterface
    else LOG("[nfp] IUserManager cmd %u (ok)\n", req->cmd);
}
