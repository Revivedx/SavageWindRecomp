// Host OS layer: everything the runtime needs that differs between macOS and Windows.
// Implementations: runtime/platform/macos.c, runtime/platform/windows.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#elif defined(__linux__)
#include "platform/linux_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim [lo, hi) for the guest at exactly that address, readable/writable (pages may be backed lazily).
bool plat_reserve_guest(uint64_t lo, uint64_t hi);
// Make sure [addr, addr+size) inside the guest window is backed (before the OS itself writes there, e.g.
// file reads); a no-op where the whole window is backed from the start.
void plat_commit(uint64_t addr, uint64_t size);
// Give the backing of [addr, addr+size) inside the guest window back to the OS (the guest unmapped it): the pages
// read as zero if anything touches them again (Windows: committed again on the first access). The window stays claimed.
void plat_decommit(uint64_t addr, uint64_t size);
// The first run of committed memory at or after addr and before end: *a .. *e (false: none). For reading the game's
// memory from a thread of ours without committing what the game never touched (runtime/rupees.c).
bool plat_committed_range(uint64_t addr, uint64_t end, uint64_t *a, uint64_t *e);
// In-place reboot: every page of [lo, hi) back to zero (and unbacked), the window still claimed.
bool plat_reset_guest(uint64_t lo, uint64_t hi);
// Vulkan sparse guest-heap backing: report committed 64 KiB pages in a reserved guest range.
bool plat_guest_sparse_heap_enabled(void);
#ifdef _WIN32
uint64_t plat_guest_commit_generation(void);
void plat_guest_sparse_heap_configure(bool intel_integrated_gpu);
void plat_guest_sparse_heap_range(uint64_t base, uint64_t size);
uint64_t plat_guest_sparse_heap_committed_word(uint64_t word);
bool plat_guest_commit_sparse_range(uint64_t addr, uint64_t size);
#endif
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t addr, uint64_t size, int prot);  // 0 on success
#define PLAT_PAGE 0x4000ull  // largest page size we deal with (macOS 16K; multiple of Windows' 4K)
// The 4K pages of [addr, addr + size) (in the guest window) written since the last call for them, which the call
// resets: their addresses into pages (ascending, at most max). -1: not known here (only Windows tracks them: the
// window is reserved with MEM_WRITE_WATCH), or more than max were written (then any page may have been).
long plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, size_t max);
// Whether any 4K page of [addr, addr + size) was written since plat_written_pages last reset it (1 / 0), without
// resetting anything. -1: not known here.
int plat_any_written(uint64_t addr, uint64_t size);

// ---- time
uint64_t plat_now_ns(void);                // monotonic
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t
void plat_wait_until_ns(uint64_t t);       // wait for plat_now_ns() >= t without spinning (the OS timer's precision)
void plat_thread_realtime(uint64_t period_ns);  // periodic timing thread (vsync): don't coalesce its timers
int plat_cpu_count(void);       // logical processors this process may run on (its affinity, after plat_cpu_placement)
void plat_cpu_placement(void);  // (game start) one logical processor per core where plat_one_per_core_default says
// Whether the game runs on one logical processor per core when SAVAGEWIND_ONE_PER_CORE doesn't say (cores: physical
// cores the process may use, logical: its logical processors). Measured on Windows with a Ryzen 5 5600X: against all
// logical processors it was 6% faster on 6 cores and 18% on 4, and 24% slower on 2 (half the 1% lows: too few
// processors for the game's busy threads). On Linux the Steam Deck (4 cores, 8 threads) lost 3% and a quarter of its
// 1% lows with it, so Linux keeps every logical processor.
static inline bool plat_one_per_core_default(int cores, int logical) {
#ifdef _WIN32
    return logical > cores && cores >= 4;
#else
    (void)cores, (void)logical;
    return false;
#endif
}

// ---- process
void plat_init(void);                                // first thing in main()
bool plat_exe_path(char *buf, size_t size);
const char *plat_log_path(void);  // the log file this run writes ("": stderr goes to a console, pipe or file)
void plat_exec(char *const argv[]);  // replace the process (NULL = same command line); returns only on failure
#if defined(_WIN32) || defined(__linux__)
void plat_terminate(void);  // end the process now, skipping DLL teardown (exit status 0)
#endif

// Child processes (game code generator and compiler, game-file setup). stdout + stderr go to log_path (NULL = /dev/null).
typedef intptr_t PlatProc;  // 0 = failed to start
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log);
int plat_proc_wait(PlatProc p, bool block);  // exit code, or -1 while still running (block = false)
void plat_proc_kill(PlatProc p);             // end it and what it started (then it's gone: no wait)
bool plat_remove_tree(const char *dir);      // rm -rf
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#define PLAT_VENV_PYTHON "venv/Scripts/python.exe"  // relative to the venv's parent
#else
#define PLAT_EXE_SUFFIX ""
#define PLAT_VENV_PYTHON "venv/bin/python"
#endif

// ---- diagnostics
// Name of the host function containing pc (guest functions are f_<addr>), NULL if unknown.
const char *plat_symbol(uint64_t pc, uint64_t *offset);
void plat_print_backtrace(void);
void plat_fatal_notice(void);  // a fatal error is about to end the process: tell the user (Windows release: message box)
void plat_crash_handler_install(void);  // per thread where the OS needs it (alternate signal stack)
// Called by the crash handler (runtime/dispatch.c): true = fault handled (watchpoint), resume.
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc);
extern int g_fault_is_write;  // set by the platform handler before rt_fault_watch: 1 write, 0 read (or unknown)
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
