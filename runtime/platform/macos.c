// macOS host layer (runtime/platform.h).
#include <crt_externs.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "../platform.h"

static mach_timebase_info_data_t tb;

void plat_init(void) {}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    // Everything readable/writable: macOS pages are 16K but the guest maps at 4K granularity.
    // Untouched pages cost nothing.
    mach_vm_address_t a = lo;
    return mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED) == KERN_SUCCESS;
}

// Fresh zero pages over the whole window (the old ones are released at once).
long plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, size_t max) { return -1; }  // (not tracked)
int plat_any_written(uint64_t addr, uint64_t size) { return -1; }

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    mach_vm_address_t a = lo;
    return mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

void plat_decommit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

uint64_t plat_now_ns(void) {
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

void plat_sleep_until_ns(uint64_t t) {
    if (!tb.denom) mach_timebase_info(&tb);
    mach_wait_until(t * tb.denom / tb.numer);
}

void plat_cpu_placement(void) {}  // (one logical processor per core: measured on Windows only)

// Real-time (time-constraint) thread: exempt from the timer coalescing macOS applies to
// ordinary threads when the app isn't frontmost, which otherwise stretches 16ms waits to ~100ms.
void plat_thread_realtime(uint64_t period_ns) {
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t period = period_ns * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)period,
        .computation = (uint32_t)(period / 20),
        .constraint = (uint32_t)(period / 4),
        .preemptible = 1,
    };
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

bool plat_exe_path(char *buf, size_t size) {
    uint32_t len = (uint32_t)size;
    return _NSGetExecutablePath(buf, &len) == 0;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    if (plat_exe_path(path, sizeof path)) execv(path, *_NSGetArgv());
}

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

void plat_fatal_notice(void) {}  // (the log / terminal shows it)

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %s\n", syms[i]);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die - the report
                                                                               // itself aborting looped, 5.7 GB log)
    uint64_t fa = (uint64_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + (WTERMSIG(st) & 127) : 255;  // (as a shell: 137 = killed)
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }
