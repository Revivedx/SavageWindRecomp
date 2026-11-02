// Opt-in virtual amiibo experiment. Public IPC layouts: switchbrew/libnx services/nfc.h and nfc.c.
// No tag dumps or keys: answer OS-level queries using the selected synthetic identity.
#include <stdlib.h>
#include <string.h>

#include "../amiibo.h"
#include "../settings.h"
#include "../hle.h"
#include "../ipc.h"

enum { NFP_READY, NFP_SEARCHING, NFP_FOUND, NFP_REMOVED, NFP_MOUNTED };
static pthread_mutex_t nfp_lock = PTHREAD_MUTEX_INITIALIZER;
static bool initialized, enabled, repeat_scans;
static uint8_t active_uid[7];
// Process-lifetime sequence: keep it across in-place reboots so repeat scans do not reuse serials.
static uint64_t repeat_serial;
static bool repeat_seeded;
static uint32_t device_state;
static int selected_figure, active_figure;
static KEvent *activate, *deactivate, *availability;
static uint32_t activate_w, activate_r, deactivate_w, deactivate_r, availability_w, availability_r;
static const uint64_t device = 1;

RT_ON_RESET(nfp_reset) {
    initialized = enabled = repeat_scans = false;
    memset(active_uid, 0, sizeof active_uid);
    device_state = NFP_READY;
    selected_figure = active_figure = AMIIBO_ARCHER;
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

// The figure is latched for the whole scan: model and UID must always agree.
static void present_figure(void) {
    active_figure = selected_figure;
    const uint8_t stable_uid[7] = {0x04, 0x53, 0x57, 0x01, 0x00, 0x00, (uint8_t)(active_figure + 1)};
    memcpy(active_uid, stable_uid, sizeof active_uid);
    if (repeat_scans) {
        if (!repeat_seeded) {
            repeat_serial = ((uint64_t)arc4random() << 32) | arc4random();
            repeat_seeded = true;
        }
        // A 47-bit serial space, separate from the normal stable identities (byte 1 has its high bit set).
        uint64_t serial = ++repeat_serial;
        for (int i = 6; i >= 1; i--) { active_uid[i] = serial & 0xff; serial >>= 8; }
        active_uid[1] |= 0x80;
    }
    device_state = NFP_FOUND;
    event_set(activate, true);
    LOG("[nfp] virtual %s detected%s\n", virtual_amiibos[active_figure].name,
        repeat_scans ? " (repeat scan identity)" : "");
}

void nfp_configure(bool on, int figure, bool repeat) {
    pthread_mutex_lock(&nfp_lock);
    selected_figure = amiibo_valid_figure(figure);
    repeat_scans = repeat;
    if (enabled != on) {
        enabled = on;
        if (!on) {
            event_set(activate, false);
            if (device_state == NFP_FOUND || device_state == NFP_MOUNTED) {
                device_state = NFP_REMOVED;
                event_set(deactivate, true);
            }
        } else if (initialized && device_state == NFP_SEARCHING) present_figure();
    }
    pthread_mutex_unlock(&nfp_lock);
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
    if (req->cmd != 19 && req->cmd != 20 && (enabled || req->cmd == 0)) LOG("[nfp] cmd %u state %u\n", req->cmd, device_state);
    // All device commands start with the eight-byte handle, except the global state/event queries.
    bool device_cmd = (req->cmd >= 3 && req->cmd <= 18) || (req->cmd >= 20 && req->cmd <= 22);
    if (device_cmd && (!initialized || req->data_size < 8 || IN(uint64_t, 0) != device)) {
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
        enabled = g_settings.virtual_amiibo;
        repeat_scans = g_settings.amiibo_repeat;
        selected_figure = amiibo_valid_figure(g_settings.amiibo_figure);
        initialized = true;
        device_state = NFP_READY;
        if (enabled) LOG("[nfp] virtual %s enabled; scans are automatic\n", virtual_amiibos[selected_figure].name);
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
        int count = initialized && b && b->addr && b->size >= sizeof device;
        if (count) memcpy((void *)(uintptr_t)b->addr, &device, sizeof device);
        OUT(int32_t, 0) = count;
        resp_size(resp, 4);
        break;
    }
    case 3:
        // Present one figure at the start of each detection window. No timing thread or UI required.
        event_set(activate, false);
        event_set(deactivate, false);
        device_state = NFP_SEARCHING;
        if (enabled) present_figure();
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
        memcpy(info, active_uid, sizeof active_uid);
        info[0xa] = sizeof active_uid;
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
        const VirtualAmiibo *figure = &virtual_amiibos[active_figure];
        // Character bytes stay in tag order; only the numbering ID is byte-swapped.
        memcpy(info, figure->character, sizeof figure->character);
        info[3] = figure->series;
        info[4] = figure->model & 0xff;
        info[5] = figure->model >> 8;
        info[6] = figure->type;
        if (write_info(req, resp, info, sizeof info)) LOG("[nfp] supplied %s model info\n", figure->name);
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
