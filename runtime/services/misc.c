// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../dlc.h"

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

// ------------------------------------------------------------------ aoc:u (locally extracted DLC)
SERVICE("aoc:u", aoc_u) {
    (void)s;
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = dlc_count(); resp_size(resp, 4); break;
    case 3: {  // ListAddOnContent(offset, count, process_id) -> indices
        if (req->data_size < 8) { resp->result = MAKE_RESULT(2, 6001); break; }
        IpcBuf *b = out_buf(req, 0);
        uint32_t capacity = IN(uint32_t, 4);
        if (!b || !b->addr) capacity = 0;
        else if (capacity > b->size / sizeof(int32_t)) capacity = (uint32_t)(b->size / sizeof(int32_t));
        if (capacity) kernel_watch_write("DLC list", b->addr, (uint64_t)capacity * sizeof(int32_t));
        OUT(uint32_t, 0) = dlc_list(IN(uint32_t, 0), capacity, b ? (int32_t *)(uintptr_t)b->addr : NULL);
        resp_size(resp, 4);
        break;
    }
    case 5: {
        uint64_t id = dlc_base_id();
        memcpy(resp->data, &id, sizeof id);
        resp_size(resp, sizeof id);
        break;
    }
    case 7:  // PrepareAddOnContent(index, process_id)
        if (req->data_size < 4 || !dlc_present(IN(int32_t, 0))) resp->result = MAKE_RESULT(2, 1001);
        break;
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
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
