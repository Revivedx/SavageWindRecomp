// Small ABI bridge for NVIDIA's MSVC-built NGX wrapper when linking Savage
// Wind with the MinGW/LLD toolchain. The wrapper's normal path uses the
// Windows APIs and the redistributable DLL; these symbols only replace MSVC
// CRT glue that is absent from a MinGW link.
#if defined(_WIN32) && DLSS_SDK_PRESENT  // (Makefile: the SDK found)
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int _fltused;
uintptr_t __security_cookie = (uintptr_t)0x2B992DDFA232ull;

void __security_check_cookie(uintptr_t value) { (void)value; }
void __GSHandlerCheck(void) {}
void __GSHandlerCheck_EH(void) {}
void __CxxFrameHandler3(void) {}
static void ngx_guard_check(void) {}
void (*__guard_check_icall_fptr)(void) = ngx_guard_check;

void *ngx_msvc_new(size_t size) __asm__("??2@YAPEAX_K@Z");
void *ngx_msvc_new(size_t size) { return malloc(size); }

void ngx_msvc_delete(void *ptr) __asm__("??3@YAXPEAX@Z");
void ngx_msvc_delete(void *ptr) { free(ptr); }

void ngx_bad_alloc(void) __asm__("?_Xbad_alloc@std@@YAXXZ");
void ngx_bad_alloc(void) { abort(); }

void ngx_length_error(const char *msg) __asm__("?_Xlength_error@std@@YAXPEBD@Z");
void ngx_length_error(const char *msg) { (void)msg; abort(); }

void ngx_throw(void *exception, void *throw_info) __asm__("_CxxThrowException");
void ngx_throw(void *exception, void *throw_info) { (void)exception; (void)throw_info; abort(); }

void ngx_init_thread_header(long *guard) __asm__("_Init_thread_header");
void ngx_init_thread_header(long *guard) { if (guard && *guard == -1) *guard = 0; }

void ngx_init_thread_footer(void) __asm__("_Init_thread_footer");
void ngx_init_thread_footer(void) {}

long ngx_init_thread_epoch(void) __asm__("_Init_thread_epoch");
long ngx_init_thread_epoch(void) { return 0; }

#endif
