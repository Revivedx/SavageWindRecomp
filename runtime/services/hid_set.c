// hid (shared memory, input state filled in later) and settings (set, set:sys).
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../language.h"
#include "../vibration.h"

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ hid
KSharedMem *g_hid_shmem;
static uint32_t hid_shmem_handle;
RT_ON_RESET(hid_reset) { g_hid_shmem = NULL; hid_shmem_handle = 0; }

static void applet_resource(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) {  // GetSharedMemoryHandle
        if (!g_hid_shmem) g_hid_shmem = kernel_create_shared_memory(0x40000, NULL, &hid_shmem_handle);
        resp->copy_handles[resp->ncopy++] = hid_shmem_handle;
    }
}

static void active_vibration_devices(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)req; (void)resp;  // ActivateVibrationDevice: our one controller is already open through SDL.
}

SERVICE("hid", hid) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IAppletResource", applet_resource, NULL)); break;
    case 40: case 106: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    case 101: OUT(uint32_t, 0) = 0x1F; resp_size(resp, 4); break;  // GetSupportedNpadStyleSet
    case 121: OUT(int64_t, 0) = 0; resp_size(resp, 8); break;      // GetNpadJoyHoldType
    case 129: OUT(int64_t, 0) = 0; resp_size(resp, 8); break;      // GetNpadHandheldActivationMode
    case 201: {  // SendVibrationValue: handle, four floats, padding, applet resource user id
        if (req->data_size < 32) break;
        VibrationValue v;
        memcpy(&v, req->data + 4, sizeof v);
        vibration_send(IN(uint32_t, 0), &v);
        break;
    }
    case 202: {  // GetActualVibrationValue
        if (req->data_size < 16) break;
        VibrationValue v = vibration_get(IN(uint32_t, 0));
        memcpy(resp->data, &v, sizeof v);
        resp_size(resp, sizeof v);
        break;
    }
    case 203: ipc_return_object(resp, service_new("IActiveVibrationDeviceList", active_vibration_devices, NULL)); break;
    case 204: if (req->data_size) vibration_permit(IN(uint8_t, 0) != 0); break;
    case 205: OUT(uint8_t, 0) = vibration_permitted(); resp_size(resp, 1); break;
    case 206: {  // SendVibrationValues: parallel input buffers (handles, values)
        IpcBuf *handles = in_buf(req, 0), *values = in_buf(req, 1);
        if (!handles || !values) break;
        uint64_t n = handles->size / sizeof(uint32_t), nv = values->size / sizeof(VibrationValue);
        if (n > nv) n = nv;
        for (uint64_t i = 0; i < n; i++) {
            uint32_t h;
            VibrationValue v;
            memcpy(&h, (const void *)(uintptr_t)(handles->addr + i * sizeof h), sizeof h);
            memcpy(&v, (const void *)(uintptr_t)(values->addr + i * sizeof v), sizeof v);
            vibration_send(h, &v);
        }
        break;
    }
    case 207: case 208: case 209: break;
    case 210: vibration_clear(); break;  // EndPermitVibrationSession
    case 200:  // GetVibrationDeviceInfo
        if (req->data_size < 4) break;
        OUT(uint32_t, 0) = 1;  // linear resonant actuator
        OUT(uint32_t, 4) = 1 + ((IN(uint32_t, 0) >> 16) & 1);
        resp_size(resp, 8);
        break;
    default: break;  // Activate*/Set*: accept
    }
}

SERVICE("hid:sys", hid_sys) { (void)s; (void)req; resp_size(resp, 0x10); }

// ------------------------------------------------------------------ settings
static const char *const languages[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl",
                                         "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans",
                                         "zh-Hant", "pt-BR"};
#define NLANG 18

static uint64_t lang_code(const char *s) {
    uint64_t v = 0;
    memcpy(&v, s, strlen(s));
    return v;
}

static void language_list(IpcReq *req, IpcResp *resp, int max) {
    IpcBuf *b = out_buf(req, 0);
    int n = 0;
    if (b) {
        uint64_t *out = (uint64_t *)(uintptr_t)b->addr;
        for (; n < max && n < NLANG && (uint64_t)(n + 1) * 8 <= b->size; n++) out[n] = lang_code(languages[n]);
    }
    OUT(int32_t, 0) = n;
    resp_size(resp, 4);
}

SERVICE("set", set) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint64_t, 0) = lang_code(language_text_code()); resp_size(resp, 8); break;  // GetLanguageCode
    case 1: language_list(req, resp, 15); break;                                   // GetAvailableLanguageCodes
    case 2: {                                                                      // MakeLanguageCode
        uint32_t i = IN(uint32_t, 0);
        OUT(uint64_t, 0) = i < NLANG ? lang_code(languages[i]) : 0;
        resp_size(resp, 8);
        break;
    }
    case 3: OUT(int32_t, 0) = 15; resp_size(resp, 4); break;
    case 4: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;                       // GetRegionCode: USA
    case 5: language_list(req, resp, NLANG); break;
    case 6: OUT(int32_t, 0) = NLANG; resp_size(resp, 4); break;
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;                        // GetQuestFlag
    default: LOG("[set] cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("set:sys", set_sys) {
    (void)s;
    switch (req->cmd) {
    case 3: case 4: {  // GetFirmwareVersion(2) -> B buffer 0x100
        IpcBuf *b = out_buf(req, 0);
        if (b) {
            uint8_t *v = (uint8_t *)(uintptr_t)b->addr;
            memset(v, 0, 0x100);
            v[0] = 8; v[1] = 1; v[2] = 0; v[4] = 8; v[5] = 1; v[6] = 0;  // 8.1.0
            strcpy((char *)v + 0x68, "8.1.0");
            strcpy((char *)v + 0x80, "8.1.0-1.0");
        }
        break;
    }
    case 23: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetColorSetId: dark
    default: LOG("[set:sys] cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
