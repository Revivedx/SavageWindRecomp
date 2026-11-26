// macOS host layer (runtime/platform.h).
#include <crt_externs.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "../platform.h"

static mach_timebase_info_data_t tb;

// stdout / stderr go to savagewind.log in the data folder, as on Linux (runtime/platform/linux.c, the same rules): the
// app opened from the Finder had its log nowhere, so a player had no savagewind.log to attach, and a problem report
// (Options) had none to copy. ~/Library/Application Support/local.savagewind (runtime/paths.c; rt_paths_init runs
// later). The last run's log stays as savagewind.prev.log. Started from a terminal, the log is shown there too; in a
// development tree (an extracted/ folder here) stderr stays where it goes.
static char g_log_path[1024];
static off_t g_echo_from;  // (where this process's part of the log starts)

static void *log_echo_thread(void *arg) {
    int out = (int)(intptr_t)arg, in = open(g_log_path, O_RDONLY);
    if (in < 0) return NULL;
    lseek(in, g_echo_from, SEEK_SET);
    char buf[8192];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n > 0) { if (write(out, buf, (size_t)n) < 0) break; continue; }
        usleep(100000);
    }
    close(in);
    return NULL;
}

// a launcher that starts this with --prepare (runtime/main.c) reads its progress lines from stdout
static bool prepare_asked(void) {
    char **argv = *_NSGetArgv();
    for (int i = 1; i < *_NSGetArgc(); i++)
        if (!strcmp(argv[i], "--prepare")) return true;
    return false;
}

static void log_to_file(void) {
    char dir[900];
    const char *env = getenv("SAVAGEWIND_DATA_ROOT"), *home = getenv("HOME");
    struct stat st;
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode)) return;  // dev mode: wherever stderr goes already
    else snprintf(dir, sizeof dir, "%s/Library/Application Support/local.savagewind", home && *home ? home : "/tmp");
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/savagewind.log", dir);
    bool append = getenv("SAVAGEWIND_LOG_APPEND") != NULL;
    if (!append) {
        char prev[1100];
        snprintf(prev, sizeof prev, "%s/savagewind.prev.log", dir);
        rename(g_log_path, prev);
    }
    const char *e = getenv("SAVAGEWIND_LOG_ECHO_FD");
    bool quiet = getenv("SAVAGEWIND_LOG_QUIET") != NULL;  // (savagewind <command>: its own output only, runtime/cli.h)
    int echo = !quiet && e && isatty(atoi(e)) ? atoi(e) : -1;
    if (echo < 0 && !quiet && isatty(STDERR_FILENO) && (echo = dup(STDERR_FILENO)) >= 0) {  // (kept across the re-exec)
        char fd[16];
        snprintf(fd, sizeof fd, "%d", echo);
        setenv("SAVAGEWIND_LOG_ECHO_FD", fd, 1);
        dprintf(echo, "[log] written to %s and shown here\n", g_log_path);
    }
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);  // (reopened, stderr was buffered: lines lost at the re-exec, or in a crash)
    if (!prepare_asked()) dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("SAVAGEWIND_LOG_APPEND", "1", 1);
    g_echo_from = lseek(STDERR_FILENO, 0, SEEK_END);
    pthread_t t;
    if (echo >= 0 && pthread_create(&t, NULL, log_echo_thread, (void *)(intptr_t)echo) == 0) pthread_detach(t);
}

void plat_init(void) { log_to_file(); }

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

bool plat_committed_range(uint64_t addr, uint64_t end, uint64_t *a, uint64_t *e) {  // (the whole window is backed)
    if (addr >= end) return false;
    *a = addr, *e = end;
    return true;
}

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

void plat_wait_until_ns(uint64_t t) { plat_sleep_until_ns(t); }  // (mach_wait_until doesn't spin)

int plat_cpu_count(void) { int n = (int)sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? n : 1; }

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

void plat_thread_elevate(void) {}

const char *plat_log_path(void) { return g_log_path; }  // ("": dev mode, the log goes to stderr)

bool plat_exe_path(char *buf, size_t size) {
    uint32_t len = (uint32_t)size;
    return _NSGetExecutablePath(buf, &len) == 0;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    if (plat_exe_path(path, sizeof path)) execv(path, *_NSGetArgv());
}

void plat_exec_wait(char *const argv[]) { plat_exec(argv); }  // (exec keeps the exit status)

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

// Another program on its own (platform.h): its own session (it stays when this process and its group end), stdin and
// output to /dev/null
bool plat_start_program(const char *const argv[]) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
#else
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
#endif
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc == 0;
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

bool plat_file_flush(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

bool plat_guest_sparse_heap_enabled(void) { return false; }

void plat_accessibility_shortcuts(bool allow) { (void)allow; }  // (Windows only)
