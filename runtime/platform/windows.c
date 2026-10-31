// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>

#include "../gamelib.h"
#include "../platform.h"

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;
// (debug) the data watchpoint (runtime/dispatch.c) in single-step mode: after the faulting instruction runs on an
// open page, the trap flag brings us back here to close the page again, so no access slips through.
static __thread uint64_t t_reclose_page, t_written;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_reclose_page) {
        uint64_t close = t_reclose_page;
        if (t_written) {  // the value the write left (logged by runtime/dispatch.c's watchpoint)
            uint64_t v = *(volatile uint64_t *)(uintptr_t)t_written;
            fprintf(stderr, "[watch]   -> %016llx\n", (unsigned long long)v);
            extern uint64_t rt_watch_after_write(uint64_t addr, uint64_t value);
            uint64_t moved = rt_watch_after_write(t_written, v);
            DWORD o;
            if (moved && moved != close) VirtualProtect((void *)(uintptr_t)moved, PLAT_PAGE, PAGE_NOACCESS, &o);
            t_written = 0;
        }
        DWORD old;
        VirtualProtect((void *)(uintptr_t)close, PLAT_PAGE, PAGE_NOACCESS, &old);
        t_reclose_page = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    if (lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    g_fault_is_write = er->ExceptionInformation[0] == 1;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) {
        extern uint64_t rt_watch_step_page(uint64_t fa);  // runtime/dispatch.c: the page to close again, or 0
        uint64_t page = rt_watch_step_page(fa);
        if (page) {
            extern bool rt_watch_in_range(uint64_t fa);
            t_reclose_page = page;
            t_written = g_fault_is_write && rt_watch_in_range(fa) ? (fa & ~7ull) : 0;
            ep->ContextRecord->EFlags |= 0x100;  // TF: single-step the faulting instruction
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// MEM_WRITE_WATCH: Windows keeps which pages were written (plat_written_pages; the Vulkan backend refreshes its copy of
// the heap's geometry from it). SAVAGEWIND_NO_WRITE_WATCH=1, or a system that refuses it: a plain reservation.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = NULL;
    if (!getenv("SAVAGEWIND_NO_WRITE_WATCH")) {
        p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (p && p != (void *)(uintptr_t)lo) VirtualFree(p, 0, MEM_RELEASE), p = NULL;
        g_write_watch = p != NULL;
    }
    if (!p) p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) return false;
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

long plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, size_t max) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || !max) return -1;
    ULONG_PTR n = max;
    DWORD gran;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, size, (void **)pages, &n, &gran) != 0) return -1;
    return n >= max ? -1 : (long)n;  // (a full list may have left pages out)
}

int plat_any_written(uint64_t addr, uint64_t size) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi) return -1;
    void *page;
    ULONG_PTR n = 1;
    DWORD gran;
    if (GetWriteWatch(0, (void *)(uintptr_t)addr, size, &page, &n, &gran) != 0) return -1;
    return n != 0;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

void plat_decommit(uint64_t addr, uint64_t size) {
    uint64_t a = (addr + 0xFFF) & ~0xFFFull, e = (addr + size) & ~0xFFFull;  // (whole pages inside the range only)
    if (e > a && a >= g_lo && e <= g_hi) VirtualFree((void *)(uintptr_t)a, e - a, MEM_DECOMMIT);
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

// (one timer per thread, closed when the thread ends: guest threads come and go, a death reload creates new ones)
static pthread_key_t timer_key;
static pthread_once_t timer_once = PTHREAD_ONCE_INIT;
static void timer_close(void *h) { CloseHandle((HANDLE)h); }
static void timer_key_init(void) { pthread_key_create(&timer_key, timer_close); }

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        if (timer) {
            pthread_once(&timer_once, timer_key_init);
            pthread_setspecific(timer_key, timer);
        }
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

// ---- process
// Release builds are GUI-subsystem executables (no console window): stdout / stderr go to savagewind.log next to the
// exe, and a fatal error shows a message box (plat_fatal_notice) instead of vanishing. Processes started by this one
// (the start screen re-executing itself for the game) append to the same log.
static char g_log_path[MAX_PATH];

static void log_to_file_without_console(void) {
    if (GetStdHandle(STD_ERROR_HANDLE) && GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE) return;  // console / pipe
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(g_log_path, sizeof g_log_path, "%ssavagewind.log", exe);
    bool append = getenv("SAVAGEWIND_LOG_APPEND") != NULL;
    if (!append) {  // the last run's log stays as savagewind.prev.log: opening again after a failed setup kept nothing of it
        char prev[MAX_PATH + 16];
        snprintf(prev, sizeof prev, "%ssavagewind.prev.log", exe);
        MoveFileExA(g_log_path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    if (!freopen(g_log_path, append ? "ab" : "wb", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    _putenv("SAVAGEWIND_LOG_APPEND=1");
}

// The recompiled game code is built for a target this CPU runs (runtime/gamelib.h): say so instead of crashing with an
// illegal instruction on a CPU too old for any.
static void require_cpu(void) {
    const char *need = gamelib_cpu_problem();
    if (!need) return;
    char text[256];
    snprintf(text, sizeof text, "Savage Wind needs %s.", need);
    MessageBoxA(NULL, text, "Savage Wind", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): the last FATAL / exception line of the log in a message box, when there is no console.
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1024] = "Savage Wind stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (!line) for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, "Savage Wind stopped because of an error:\n\n%.600s", line);
            if (strstr(line, "device lost")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nThe graphics driver reset the GPU. Usually a frame took it too long "
                         "(Windows resets a GPU after 2 seconds) or it ran out of video memory. Lower Video > Upscaler "
                         "quality and Shadow resolution, close other programs and update the driver; on a laptop, run "
                         "Savage Wind on the dedicated GPU.");
            } else if (strstr(line, "GPU ran out of memory")) {  // (vk.h VK_CHECK: out of device or host memory, #117)
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nThe graphics card ran out of memory. Integrated graphics share the "
                         "PC's RAM, and Savage Wind keeps the game's 3 GB of memory available to the GPU: with integrated "
                         "graphics it needs 16 GB of RAM. Close other programs and lower Video > Shadow resolution and the "
                         "upscaler quality; with a dedicated GPU, update its driver.");
            } else if (strstr(line, "Vulkan 1.3") || strstr(line, "vkCreateInstance")) {  // (no driver, or too old)
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nSavage Wind needs a graphics driver with Vulkan 1.3: install the "
                         "latest driver for your GPU (NVIDIA, AMD or Intel).");
            } else if (strstr(line, "Vulkan") || strstr(line, "VK_") || strstr(line, "vk")) {  // (any other driver error)
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nThe graphics driver reported an error. Updating the driver for "
                         "your GPU may help; if it keeps happening, please report it with the log below.");
            }
        }
    }
    size_t l = strlen(text);
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    MessageBoxA(NULL, text, "Savage Wind", MB_ICONERROR | MB_OK);
}

void plat_init(void) {
    log_to_file_without_console();
    require_cpu();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

// One logical processor per physical core for the whole process: the game's main thread and the render thread then
// never share a core with another busy thread of the game, or with a helper spinning for work. With fewer cores than
// the game's busy threads (main, two job workers, render, audio, file loading) they would queue for one instead, so by
// default only with SMT and 8 or more physical cores. SAVAGEWIND_ONE_PER_CORE=1: always, 0: never. Called when the game
// starts, so what the start screen runs (the game code build) keeps every logical processor.
void plat_cpu_placement(void) {
    DWORD_PTR all, sys, one = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &all, &sys)) return;
    int logical = __builtin_popcountll((unsigned long long)all), cores = 0;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    char *buf = len ? malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (void *)buf, &len))
        for (DWORD o = 0; o < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)(buf + o);
            DWORD_PTR m = e->Processor.GroupMask[0].Group == 0 ? e->Processor.GroupMask[0].Mask & all : 0;
            if (m) cores++, one |= m & -m;  // (the core's first logical processor)
            o += e->Size;
        }
    free(buf);
    const char *x = getenv("SAVAGEWIND_ONE_PER_CORE");
    bool on = one && (x ? atoi(x) != 0 : cores >= 8 && logical > cores);
    fprintf(stderr, "[cpu] %d cores, %d logical processors: %s\n", cores, logical,
            on ? "one logical processor per core" : "all logical processors");
    if (on) SetProcessAffinityMask(GetCurrentProcess(), one);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, then exit.
void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c) - mingw builds carry DWARF, not PDB symbols.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx savagewind+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- POSIX shims (win_compat.h)
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) fprintf(stderr, "[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // every child in one job that ends with this process (a compiler's own children too: no leftovers on quit)
    static HANDLE job;
    if (!job && (job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)pi.hProcess;
}

void plat_proc_kill(PlatProc p) {  // (what it started ends with this process: the job above)
    TerminateProcess((HANDLE)p, 255);
    WaitForSingleObject((HANDLE)p, INFINITE);
    CloseHandle((HANDLE)p);
}

int plat_proc_wait(PlatProc p, bool block) {
    HANDLE h = (HANDLE)p;
    DWORD w = WaitForSingleObject(h, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    if (w != WAIT_OBJECT_0) return 255;  // (not a process we can wait for: ended as far as the caller is concerned)
    DWORD code = 255;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code > 255 ? 255 : (int)code;  // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1))
}

bool plat_remove_tree(const char *dir) {
    // SHFileOperation wants a double-NUL-terminated path with backslashes
    static __thread wchar_t w[MAX_PATH + 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH);
    if (n <= 1) return false;
    w[n] = 0;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    SHFILEOPSTRUCTW op = {.wFunc = FO_DELETE, .pFrom = w, .fFlags = FOF_NO_UI};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) { TerminateProcess(GetCurrentProcess(), 0); }
