// nfp:user, the game's amiibo reader, with virtual figures (runtime/amiibo.h). One reader (device handle 0, the first
// controller). The game starts detection when it wants a figure (the amiibo rune): the figure chosen in Savage Wind's
// options is on the reader then, with no window over the game (none chosen: the game finds no figure, as with an
// empty reader). Mounting reads its data: the tag information (serial number), the model information (the amiibo
// ID, all BOTW goes by), and its game data (application area, 0xd8 bytes), kept as a file per figure; with the
// amiibo keys a figure's own settings and game data come from its dump (runtime/amiibo.h).
//
// IUser commands (nn::nfp::IUser): 0 Initialize, 1 Finalize, 2 ListDevices, 3 StartDetection, 4 StopDetection,
// 5 Mount, 6 Unmount, 7 OpenApplicationArea, 8 GetApplicationArea, 9 SetApplicationArea, 10 Flush, 11 Restore,
// 12 CreateApplicationArea, 13 GetTagInfo, 14 GetRegisterInfo, 15 GetCommonInfo, 16 GetModelInfo,
// 17 AttachActivateEvent, 18 AttachDeactivateEvent, 19 GetState, 20 GetDeviceState, 21 GetNpadId,
// 22 GetApplicationAreaSize, 23 AttachAvailabilityChangeEvent, 24 RecreateApplicationArea.
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "../amiibo.h"
#include "../hle.h"
#include "../ipc.h"

enum { DEV_INITIALIZED, DEV_SEARCHING, DEV_FOUND, DEV_REMOVED, DEV_MOUNTED, DEV_UNAVAILABLE, DEV_FINALIZED };
enum { E_DEVICE_NOT_FOUND = 64, E_WRONG_STATE = 73, E_TAG_REMOVED = 97, E_NOT_SET_UP = 120, E_AREA_NOT_INITIALIZED = 128,
       E_WRONG_AREA_ID = 152, E_AREA_EXISTS = 168 };
#define NFP_ERROR(d) MAKE_RESULT(115, d)
#define AREA_SIZE 0xd8

static _Atomic int g_state = DEV_INITIALIZED;  // the reader's
static _Atomic int g_fig = -1;                 // the figure on it (amiibo_at), -1 none
static bool g_initialized;
static KEvent *g_activate, *g_deactivate;
static uint32_t g_activate_h, g_deactivate_h, g_avail_h;
// the mounted figure's game data
static uint8_t g_area[AREA_SIZE];
static int g_area_size = -1;  // -1: none on the figure
static uint32_t g_area_id;
static bool g_area_open, g_area_dirty;
static AmiiboData g_data;  // what the mounted figure holds (amiibo_read_data)
static bool g_data_ok;     // ... read with the amiibo keys

RT_ON_RESET(nfp_reset) {
    g_state = DEV_INITIALIZED, g_fig = -1, g_initialized = false;
    g_activate = g_deactivate = NULL, g_activate_h = g_deactivate_h = g_avail_h = 0;
    g_area_size = -1, g_area_open = g_area_dirty = false, g_data_ok = false;
}

static void fire(KEvent *ev) {
    if (!ev) return;
    klock();
    kernel_signal(ev);
    kunlock();
}

static const Amiibo *mounted(IpcResp *resp) {
    if (g_state != DEV_MOUNTED) { resp->result = NFP_ERROR(g_state == DEV_REMOVED ? E_TAG_REMOVED : E_WRONG_STATE); return NULL; }
    return amiibo_at(g_fig);
}

static void write_out(IpcReq *req, const void *src, size_t n) {
    IpcBuf *b = out_buf(req, 0);
    if (!b || !b->addr) return;
    size_t k = b->size < n ? b->size : n;
    memset((void *)(uintptr_t)b->addr, 0, b->size);
    memcpy((void *)(uintptr_t)b->addr, src, k);
}

static void today(uint8_t *p) {  // {u16 year, u8 month, u8 day}
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    int y = tm.tm_year + 1900;
    p[0] = (uint8_t)y, p[1] = (uint8_t)(y >> 8), p[2] = (uint8_t)(tm.tm_mon + 1), p[3] = (uint8_t)tm.tm_mday;
}

static void nfp_user(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    const Amiibo *a;
    switch (req->cmd) {
    case 0: g_initialized = true, g_state = DEV_INITIALIZED; break;  // Initialize
    case 1: g_initialized = false, g_state = DEV_FINALIZED; break;  // Finalize
    case 2: {  // ListDevices: one, handle 0
        uint64_t h = 0;
        write_out(req, &h, sizeof h);
        OUT(int32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 3: {  // StartDetection: the chosen figure is on the reader (none chosen: nothing is found)
        int i = amiibo_on_reader();
        g_fig = i, g_state = i >= 0 ? DEV_FOUND : DEV_SEARCHING;
        if (i >= 0) {
            LOG("[nfp] %s on the reader\n", amiibo_at(i)->name);
            fire(g_activate);
        } else {
            LOG("[nfp] looking for a figure, none on the reader\n");
        }
        break;
    }
    case 4:  // StopDetection (the game also sends it now and then with nothing going on)
        if (g_state != DEV_INITIALIZED) LOG("[nfp] detection stopped\n");
        if (g_state == DEV_MOUNTED && g_area_dirty) amiibo_area_save(amiibo_at(g_fig), g_area_id, g_area, g_area_size);
        g_fig = -1, g_state = DEV_INITIALIZED, g_area_open = g_area_dirty = false;
        break;
    case 5:  // Mount: read the figure's game data
        if (g_state != DEV_FOUND) { resp->result = NFP_ERROR(g_state == DEV_REMOVED ? E_TAG_REMOVED : E_WRONG_STATE); break; }
        g_area_size = amiibo_area_load(amiibo_at(g_fig), &g_area_id, g_area, AREA_SIZE);
        g_data_ok = amiibo_read_data(amiibo_at(g_fig), &g_data);
        g_area_open = g_area_dirty = false;
        g_state = DEV_MOUNTED;
        break;
    case 6:  // Unmount
        if (g_state == DEV_MOUNTED) {
            if (g_area_dirty) amiibo_area_save(amiibo_at(g_fig), g_area_id, g_area, g_area_size);
            g_state = DEV_FOUND, g_area_open = g_area_dirty = false;
        }
        break;
    case 7:  // OpenApplicationArea (access ID)
        if (!mounted(resp)) break;
        if (g_area_size < 0) resp->result = NFP_ERROR(E_AREA_NOT_INITIALIZED);
        else if (g_area_id != IN(uint32_t, 8)) resp->result = NFP_ERROR(E_WRONG_AREA_ID);
        else g_area_open = true;
        break;
    case 8:  // GetApplicationArea
        if (!mounted(resp)) break;
        if (!g_area_open) { resp->result = NFP_ERROR(E_AREA_NOT_INITIALIZED); break; }
        write_out(req, g_area, (size_t)g_area_size);
        OUT(uint32_t, 0) = (uint32_t)g_area_size;
        resp_size(resp, 4);
        break;
    case 9: {  // SetApplicationArea
        if (!mounted(resp)) break;
        IpcBuf *b = in_buf(req, 0);
        if (!g_area_open || !b) { resp->result = NFP_ERROR(E_AREA_NOT_INITIALIZED); break; }
        size_t n = b->size < AREA_SIZE ? b->size : AREA_SIZE;
        memset(g_area, 0, AREA_SIZE);
        memcpy(g_area, (const void *)(uintptr_t)b->addr, n);
        g_area_size = AREA_SIZE, g_area_dirty = true;
        break;
    }
    case 10:  // Flush
        if ((a = mounted(resp)) && g_area_dirty) amiibo_area_save(a, g_area_id, g_area, g_area_size), g_area_dirty = false;
        break;
    case 12: case 24: {  // CreateApplicationArea / RecreateApplicationArea (access ID, its first contents)
        if (!(a = mounted(resp))) break;
        if (req->cmd == 12 && g_area_size >= 0) { resp->result = NFP_ERROR(E_AREA_EXISTS); break; }
        IpcBuf *b = in_buf(req, 0);
        memset(g_area, 0, AREA_SIZE);
        if (b) memcpy(g_area, (const void *)(uintptr_t)b->addr, b->size < AREA_SIZE ? b->size : AREA_SIZE);
        g_area_id = IN(uint32_t, 8), g_area_size = AREA_SIZE, g_area_dirty = false;
        amiibo_area_save(a, g_area_id, g_area, g_area_size);
        break;
    }
    case 13: {  // GetTagInfo: the serial number, an NFC-A type 2 tag
        int st = g_state;
        if (st != DEV_FOUND && st != DEV_MOUNTED) { resp->result = NFP_ERROR(st == DEV_REMOVED ? E_TAG_REMOVED : E_WRONG_STATE); break; }
        uint8_t t[0x58] = {0};
        memcpy(t, amiibo_at(g_fig)->uid, 7);
        t[0x0a] = 7, t[0x20] = 1, t[0x24] = 2;
        write_out(req, t, sizeof t);
        break;
    }
    case 14: {  // GetRegisterInfo: its nickname (no owner Mii); read with the keys, a figure never set up has none
        if (!(a = mounted(resp))) break;
        if (g_data_ok && !(g_data.flags & AMIIBO_SET_UP)) { resp->result = NFP_ERROR(E_NOT_SET_UP); break; }
        uint8_t r[0x100] = {0};
        today(r + 0x58);
        char *nick = (char *)r + 0x5c;  // (UTF-8)
        if (g_data_ok) {
            int k = 0;
            for (int i = 0; i < 10 && g_data.nickname[i] && k < 0x25; i++) {
                unsigned c = g_data.nickname[i];
                if (c < 0x80) nick[k++] = (char)c;
                else if (c < 0x800) nick[k++] = (char)(0xc0 | c >> 6), nick[k++] = (char)(0x80 | (c & 0x3f));
                else nick[k++] = (char)(0xe0 | c >> 12), nick[k++] = (char)(0x80 | (c >> 6 & 0x3f)), nick[k++] = (char)(0x80 | (c & 0x3f));
            }
        } else {
            snprintf(nick, 0x29, "%.10s", a->name);
        }
        write_out(req, r, sizeof r);
        break;
    }
    case 15: {  // GetCommonInfo
        if (!mounted(resp)) break;
        uint8_t c[0x40] = {0};
        today(c);
        c[0x08] = AREA_SIZE;
        write_out(req, c, sizeof c);
        break;
    }
    case 16: {  // GetModelInfo: the amiibo ID (character as on the tag, the model number little-endian)
        if (!(a = mounted(resp))) break;
        uint8_t m[0x40] = {0};
        m[0] = a->id[0], m[1] = a->id[1], m[2] = a->id[2], m[3] = a->id[3];
        m[4] = a->id[5], m[5] = a->id[4], m[6] = a->id[6];
        write_out(req, m, sizeof m);
        break;
    }
    // (a new event each time: the game closes the last one's handle when it finalizes the reader)
    case 17:  // AttachActivateEvent
        g_activate = kernel_create_event_pair(&(uint32_t){0}, &g_activate_h);
        resp->copy_handles[resp->ncopy++] = g_activate_h;
        if (g_state == DEV_FOUND) fire(g_activate);  // (the figure was put on the reader before the game asked)
        break;
    case 18:  // AttachDeactivateEvent
        g_deactivate = kernel_create_event_pair(&(uint32_t){0}, &g_deactivate_h);
        resp->copy_handles[resp->ncopy++] = g_deactivate_h;
        break;
    case 23:  // AttachAvailabilityChangeEvent
        kernel_create_event_pair(&(uint32_t){0}, &g_avail_h);
        resp->copy_handles[resp->ncopy++] = g_avail_h;
        break;
    case 19: OUT(uint32_t, 0) = g_initialized ? 1 : 0; resp_size(resp, 4); break;  // GetState
    case 20: OUT(uint32_t, 0) = (uint32_t)g_state; resp_size(resp, 4); break;    // GetDeviceState
    case 21: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;                   // GetNpadId: the first controller
    case 22: OUT(uint32_t, 0) = AREA_SIZE; resp_size(resp, 4); break;           // GetApplicationAreaSize
    case 11: break;                                                            // Restore
    default: LOG("[nfp] IUser cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
    if (req->cmd != 4 && req->cmd != 19 && req->cmd != 20) LOG("[nfp] cmd %u -> %#x, reader %d\n", req->cmd, resp->result, (int)g_state);
}

SERVICE("nfp:user", nfp_user_manager) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IUser(nfp)", nfp_user, NULL));  // CreateUserInterface
    else LOG("[nfp] IUserManager cmd %u (ok)\n", req->cmd);
}
