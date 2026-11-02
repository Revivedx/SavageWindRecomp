// Opt-in virtual amiibo experiment. Public IPC layouts: switchbrew/libnx services/nfc.h and nfc.c.
// No tag dumps or keys: answer the OS-level queries with a synthetic Link (Archer) identity.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

enum { NFP_READY, NFP_SEARCHING, NFP_FOUND, NFP_REMOVED, NFP_MOUNTED };
static pthread_mutex_t nfp_lock = PTHREAD_MUTEX_INITIALIZER;
static bool initialized, enabled;
static uint32_t device_state;
static KEvent *activate, *deactivate, *availability;
static uint32_t activate_w, activate_r, deactivate_w, deactivate_r, availability_w, availability_r;
static const uint64_t device = 1;

RT_ON_RESET(nfp_reset) {
    initialized = enabled = false;
    device_state = NFP_READY;
    activate = deactivate = availability = NULL;
    activate_w = activate_r = deactivate_w = deactivate_r = availability_w = availability_r = 0;
}

static void event_set(KEvent *event, bool signal) {
    if (!event) return;
    klock();
    if (signal) kernel_signal(event);
    else event->signaled = false;
    pthread_mutex_unlock(&g_klock);
}

static void return_event(IpcResp *resp, KEvent **event, uint32_t *w, uint32_t *r) {
    if (!*event) *event = kernel_create_event_pair(w, r);
    // The IPC layer writes handles directly, so give the caller its own reference to close.
    resp->copy_handles[resp->ncopy++] = kernel_new_handle(*event);
}

static bool write_info(IpcReq *req, IpcResp *resp, const void *data, size_t size) {
    IpcBuf *b = out_buf(req, 0);
    if (!b || !b->addr || b->size < size) {
        resp->result = MAKE_RESULT(115, 65);
        return false;
    }
    memcpy((void *)(uintptr_t)b->addr, data, size);
    return true;
}

static void nfp_user(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    pthread_mutex_lock(&nfp_lock);
    if (enabled || req->cmd == 0) LOG("[nfp] cmd %u state %u\n", req->cmd, device_state);
    // All device commands start with the eight-byte handle, except the global state/event queries.
    bool device_cmd = (req->cmd >= 3 && req->cmd <= 18) || (req->cmd >= 20 && req->cmd <= 22);
    if (device_cmd && (!enabled || !initialized || req->data_size < 8 || IN(uint64_t, 0) != device)) {
        resp->result = MAKE_RESULT(115, 64);
        goto done;
    }
    if (req->cmd >= 5 && req->cmd <= 16 && req->cmd != 13 &&
        device_state != NFP_MOUNTED && !(req->cmd == 5 && device_state == NFP_FOUND)) {
        resp->result = MAKE_RESULT(115, 73);
        goto done;
    }
    switch (req->cmd) {
    case 0: {
        const char *v = getenv("SAVAGEWIND_AMIIBO");
        enabled = v && !strcmp(v, "archer");
        initialized = true;
        device_state = NFP_READY;
        if (enabled) LOG("[nfp] virtual Link (Archer) enabled; scans are automatic\n");
        else if (v && *v) LOG("[nfp] unknown SAVAGEWIND_AMIIBO value; use archer\n");
        break;
    }
    case 1:
        event_set(activate, false);
        event_set(deactivate, false);
        initialized = false;
        device_state = NFP_READY;
        break;
    case 2: {
        IpcBuf *b = out_buf(req, 0);
        int count = enabled && initialized && b && b->addr && b->size >= sizeof device;
        if (count) memcpy((void *)(uintptr_t)b->addr, &device, sizeof device);
        OUT(int32_t, 0) = count;
        resp_size(resp, 4);
        break;
    }
    case 3:
        // Present one figure at the start of each detection window. No timing thread or UI required.
        event_set(activate, false);
        event_set(deactivate, false);
        device_state = NFP_FOUND;
        event_set(activate, true);
        LOG("[nfp] virtual figure detected\n");
        break;
    case 4:
        event_set(activate, false);
        if (device_state == NFP_FOUND || device_state == NFP_MOUNTED) event_set(deactivate, true);
        device_state = NFP_READY;
        break;
    case 5: device_state = NFP_MOUNTED; break;
    case 6: device_state = NFP_FOUND; break;
    case 7: case 8: case 9: case 12: case 24:
        // This prototype has no application area (in particular, no Wolf Link save data).
        resp->result = MAKE_RESULT(115, 128);
        break;
    case 10: case 11: break;  // Flush / Restore: the synthetic tag has no writable storage.
    case 13: {
        if (device_state != NFP_FOUND && device_state != NFP_MOUNTED) {
            resp->result = MAKE_RESULT(115, 73);
            break;
        }
        uint8_t info[0x58] = {0};
        const uint8_t uid[7] = {0x04, 0x53, 0x57, 0x01, 0x00, 0x00, 0x01};
        memcpy(info, uid, sizeof uid);
        info[0xa] = sizeof uid;
        info[0x20] = 1;  // Type A protocol
        info[0x24] = 2;  // Type 2 tag
        write_info(req, resp, info, sizeof info);
        break;
    }
    case 14:
        // Unregistered: avoid supplying a fabricated Mii or opening the console registration applet.
        resp->result = MAKE_RESULT(115, 120);
        break;
    case 15: {
        uint8_t info[0x40] = {0};
        info[0] = 0xe8; info[1] = 0x07; info[2] = 1; info[3] = 1;  // 2024-01-01
        info[8] = 0xd8;  // maximum application area size, not an existing application area
        write_info(req, resp, info, sizeof info);
        break;
    }
    case 16: {
        uint8_t info[0x40] = {0};
        // Catalog ID 01000000-03530902: character bytes stay in tag order;
        // only the numbering ID is byte-swapped by GetModelInfo.
        info[0] = 0x01;                // Link
        info[3] = 0x00;                // Series ID
        info[4] = 0x53; info[5] = 0x03; // model 0x0353 (Archer)
        info[6] = 0x09;                // NFP type (Breath of the Wild)
        write_info(req, resp, info, sizeof info);
        LOG("[nfp] supplied Link (Archer) model info\n");
        break;
    }
    case 17:
        return_event(resp, &activate, &activate_w, &activate_r);
        if (device_state == NFP_FOUND || device_state == NFP_MOUNTED) event_set(activate, true);
        break;
    case 18: return_event(resp, &deactivate, &deactivate_w, &deactivate_r); break;
    case 19: OUT(uint32_t, 0) = initialized; resp_size(resp, 4); break;
    case 20: OUT(uint32_t, 0) = device_state; resp_size(resp, 4); break;
    case 21: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // player 1
    case 22: OUT(uint32_t, 0) = 0xd8; resp_size(resp, 4); break;
    case 23: return_event(resp, &availability, &availability_w, &availability_r); break;
    default:
        LOG("[nfp] unimplemented cmd %u\n", req->cmd);
        resp->result = MAKE_RESULT(115, 65);
        break;
    }
done:
    pthread_mutex_unlock(&nfp_lock);
}

SERVICE("nfp:user", nfp_user_manager) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IUser(nfp)", nfp_user, NULL));
    else resp->result = MAKE_RESULT(115, 65);
}
