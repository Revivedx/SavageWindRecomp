// (test and development only) Residency queries never touch guest pages. Guarded reads tolerate unload races.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "memscan.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

bool ms_resident(uint64_t addr, size_t npages, uint8_t *resident) {
    PSAPI_WORKING_SET_EX_INFORMATION pages[4096];
    if (npages > (UINT64_MAX - addr) / 4096) return false;
    for (size_t base = 0; base < npages;) {
        size_t count = npages - base;
        if (count > 4096) count = 4096;
        for (size_t i = 0; i < count; i++) {
            pages[i].VirtualAddress = (void *)(uintptr_t)(addr + (base + i) * 4096);
            pages[i].VirtualAttributes.Flags = 0;
        }
        if (!K32QueryWorkingSetEx(GetCurrentProcess(), pages, (DWORD)(count * sizeof *pages))) return false;
        for (size_t i = 0; i < count; i++) resident[base + i] = pages[i].VirtualAttributes.Valid ? 1 : 0;
        base += count;
    }
    return true;
}

bool ms_guarded(const void *p, size_t n, MsFn fn, void *ctx, size_t *ret) {
    // Clang's x64 SEH scope encloses the call instruction, so callee faults unwind into this handler too.
    // The runtime's vectored lazy-commit handler runs first: callers must exclude uncommitted guest pages.
    __try {
        *ret = fn(p, n, ctx);
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                 GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR) ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}

#else
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/uio.h>
#endif

bool ms_resident(uint64_t addr, size_t npages, uint8_t *resident) {
    if (!npages) return true;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 4096 || page_size % 4096 || addr % 4096 ||
        npages > (UINTPTR_MAX - addr) / 4096) return false;
    uint64_t native = (uint64_t)page_size;
    // macOS can use 16 KB pages. Translate their residency bits back to the caller's 4 KB pages.
    for (size_t base = 0; base < npages;) {
        size_t count = npages - base;
        if (count > 4096) count = 4096;
        uint64_t start = addr + base * 4096, aligned = start - start % native;
        size_t bytes = (size_t)(start - aligned) + count * 4096;
#ifdef __APPLE__
        char vec[4097];
#else
        unsigned char vec[4097];
#endif
        if (mincore((void *)(uintptr_t)aligned, bytes, vec)) return false;
        for (size_t i = 0; i < count; i++)
            resident[base + i] = (uint8_t)(vec[(start - aligned + i * 4096) / native] & 1);
        base += count;
    }
    return true;
}

bool ms_guarded(const void *p, size_t n, MsFn fn, void *ctx, size_t *ret) {
    // Unix callers pass at most 1 MB. One snapshot preserves fn's length and byte-offset semantics.
    // Each worker owns its buffer; the callback must not recursively call ms_guarded on this thread.
    static _Thread_local union { max_align_t align; unsigned char bytes[1024 * 1024]; } copy;
    if (n > sizeof copy.bytes) return false;
    if (n) {
#ifdef __APPLE__
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)p, n,
                                  (mach_vm_address_t)(uintptr_t)copy.bytes, &got) != KERN_SUCCESS || got != n)
            return false;
#else
        struct iovec local = {copy.bytes, n}, remote = {(void *)p, n};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)n) return false;
#endif
    }
    *ret = fn(copy.bytes, n, ctx);
    return true;
}
#endif
