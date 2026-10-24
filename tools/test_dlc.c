// Synthetic files only. Run with `make test-dlc`; no game dump is needed.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../runtime/romfs_build.h"

#include "../runtime/services/fs.c"
#include "../runtime/services/misc.c"

#ifdef _WIN32
#include <windows.h>
// This harness is single-threaded; the application uses platform/windows.c.
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)n);
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _write(fd, buf, (unsigned)n);
}
#endif

const char *g_romfs_dir;
uint64_t g_program_id;
void rt_fatal_trace(void) { abort(); }
void kernel_watch_write(const char *what, uint64_t addr, uint64_t len) { (void)what; (void)addr; (void)len; }
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { *w = 1; *r = 2; return NULL; }
Service *service_new(const char *name, ServiceHandler h, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = name; s->handler = h; s->state = state;
    return s;
}
void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }
const char *language_voice_folder(void) { return NULL; }
const char *rt_version(void) { return "test"; }

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
}

int main(void) {
#ifdef _WIN32
    _fmode = _O_BINARY;
    char temp_path[MAX_PATH], tmp[MAX_PATH];
    assert(GetTempPathA(sizeof temp_path, temp_path));
    assert(GetTempFileNameA(temp_path, "swd", 0, tmp));
    assert(DeleteFileA(tmp));
    assert(mkdir(tmp, 0755) == 0);
    char *root = tmp;
#else
    char tmp[] = "/tmp/savagewind-dlc-XXXXXX";
    char *root = mkdtemp(tmp);
#endif
    assert(root);
    char cwd[4096];
    assert(getcwd(cwd, sizeof cwd));
    assert(chdir(root) == 0);
    g_program_id = 0x01007ef00011e000ull;
    dlc_init(".", g_program_id);
    assert(dlc_count() == 0);
    mkdir("dlc", 0755);
    mkdir("dlc/2", 0755); mkdir("dlc/2/romfs", 0755);
    put("dlc/2/romfs/payload", "second DLC\r\n\x1a");
    mkdir("dlc/01007ef00011f001", 0755); mkdir("dlc/01007ef00011f001/romfs", 0755);
    put("dlc/01007ef00011f001/romfs/payload", "first DLC");
    mkdir("dlc/3", 0755); put("dlc/3/romfs.bin", "truncated");
    mkdir("dlc/4096", 0755); mkdir("dlc/0100000000001001", 0755);
    char why[256];
    assert(romfs_build("dlc/2/romfs", "dlc/4096/romfs.bin", NULL, why, sizeof why));
    assert(romfs_build("dlc/2/romfs", "dlc/0100000000001001/romfs.bin", NULL, why, sizeof why));
    dlc_init(".", g_program_id);
    assert(dlc_count() == 2 && dlc_base_id() == 0x01007ef00011f000ull);
    int32_t indices[3] = {-1, -1, -1};
    assert(dlc_list(0, 3, indices) == 2 && indices[0] == 1 && indices[1] == 2 && indices[2] == -1);
    assert(dlc_list(1, 1, indices) == 1 && indices[0] == 2);
    assert(dlc_list(UINT32_MAX, 3, indices) == 0);
    assert(!dlc_present(-1) && !dlc_present(3));

    uint64_t args[2] = {0, 0};
    IpcReq req = {.cmd = 2, .data = (const uint8_t *)args, .data_size = sizeof args};
    IpcResp resp = {0};
    aoc_u(NULL, &req, &resp);
    assert(*(uint32_t *)resp.data == 2);
    req.cmd = 5; resp = (IpcResp){0}; aoc_u(NULL, &req, &resp);
    uint64_t reported_base;
    memcpy(&reported_base, resp.data, sizeof reported_base);
    assert(reported_base == dlc_base_id());
    uint32_t list_args[4] = {0, 100, 0, 0};
    req = (IpcReq){.cmd = 3, .data = (const uint8_t *)list_args, .data_size = sizeof list_args,
                   .b = {{(uintptr_t)indices, 4}}, .nb = 1};
    indices[1] = -1;
    resp = (IpcResp){0}; aoc_u(NULL, &req, &resp);
    assert(*(uint32_t *)resp.data == 1 && indices[0] == 1 && indices[1] == -1);
    req.nb = 0; resp = (IpcResp){0}; aoc_u(NULL, &req, &resp);
    assert(*(uint32_t *)resp.data == 0);
    req.cmd = 7; list_args[0] = 3;
    resp = (IpcResp){0}; aoc_u(NULL, &req, &resp); assert(resp.result);
    list_args[0] = 1;
    resp = (IpcResp){0}; aoc_u(NULL, &req, &resp); assert(!resp.result);

    args[1] = dlc_base_id() + 2;
    req = (IpcReq){.cmd = 202, .data = (const uint8_t *)args, .data_size = sizeof args};
    resp = (IpcResp){0}; fsp_srv(NULL, &req, &resp);
    assert(!resp.result && resp.nobjects == 1);
    Service *image = resp.objects[0];
    Storage *st = image->state;
    // A base overlay must never alter DLC reads or its reported size.
    ov_n = 1; ov_end = st->size + 1234;
    req.cmd = 4; resp = (IpcResp){0}; image->handler(image, &req, &resp);
    uint64_t reported_size;
    memcpy(&reported_size, resp.data, sizeof reported_size);
    assert(reported_size == st->size);
    char payload[32] = {0};
    args[0] = 0x200; args[1] = strlen("second DLC\r\n\x1a");
    req = (IpcReq){.cmd = 0, .data = (const uint8_t *)args, .data_size = sizeof args,
                   .b = {{(uintptr_t)payload, sizeof payload}}, .nb = 1};
    resp = (IpcResp){0}; image->handler(image, &req, &resp);
    assert(!resp.result && !memcmp(payload, "second DLC\r\n\x1a", args[1]));
    args[0] = st->size - 1; args[1] = 2;
    resp = (IpcResp){0}; image->handler(image, &req, &resp); assert(resp.result);
    args[0] = UINT64_MAX;
    resp = (IpcResp){0}; image->handler(image, &req, &resp); assert(resp.result);
    free(image->state); free(image); ov_n = 0; ov_end = 0;
    args[1] = dlc_base_id() + 3; req.cmd = 202;
    resp = (IpcResp){0}; fsp_srv(NULL, &req, &resp); assert(resp.result && !resp.nobjects);
    // A new boot closes the previous catalog and discovers the files again.
    dlc_init(".", g_program_id); assert(dlc_count() == 2);
    dlc_init("missing", g_program_id); assert(dlc_count() == 0);
    const char *files[] = {"dlc/2/romfs/payload", "dlc/2/romfs.bin", "dlc/01007ef00011f001/romfs/payload",
                           "dlc/01007ef00011f001/romfs.bin", "dlc/3/romfs.bin",
                           "dlc/4096/romfs.bin", "dlc/0100000000001001/romfs.bin"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) assert(unlink(files[i]) == 0);
    const char *dirs[] = {"dlc/2/romfs", "dlc/2", "dlc/01007ef00011f001/romfs", "dlc/01007ef00011f001",
                          "dlc/3", "dlc/4096", "dlc/0100000000001001", "dlc"};
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) assert(rmdir(dirs[i]) == 0);
    assert(chdir(cwd) == 0); assert(rmdir(root) == 0);
    puts("DLC discovery, IPC, isolated storage and reboot tests passed");
}
