// Guest setjmp / longjmp (musl's, in sdk). libjpeg and libpng in nn::image leave their errors with longjmp: the
// JPEG encoder of the autosave's screenshot, for one. The guest longjmp returns into the middle of the function
// that called setjmp, and recompiled code can't resume there (dispatch only knows function entries). So the
// generator (tools/recomp/gen.py setjmp_call) wraps every call to the guest setjmp in a host setjmp, in the C function of
// the caller: rt_setjmp_slot gives it a host jmp_buf for the guest one (per thread, by guest address). The guest
// setjmp still runs and fills the guest jmp_buf. The guest longjmp is HLE (runtime/hle/hle_symbols.txt): it
// restores the registers musl saved there and jumps to the host jmp_buf, back into that C function with x0 set.
//   musl aarch64 jmp_buf: x19-x28 at 0x00, x29 0x50, x30 0x58, sp 0x68, d8-d15 0x70
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "../hle.h"

#define SLOTS 32
typedef struct {
    uint64_t gbuf;  // guest jmp_buf (0: free)
    uint64_t gsp;   // guest sp at the setjmp: the sp musl saves in it
    jmp_buf hb;
} JmpSlot;
static __thread JmpSlot slots[SLOTS];
static __thread unsigned slot_next;

jmp_buf *rt_setjmp_slot(Cpu *c, uint64_t gbuf) {
    JmpSlot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++)
        if (slots[i].gbuf == gbuf) s = &slots[i];
    // a new buffer: a free slot, else one set up by a deeper frame (its sp below ours: that function has returned)
    for (int i = 0; i < SLOTS && !s; i++)
        if (!slots[i].gbuf || slots[i].gsp < c->sp) s = &slots[i];
    if (!s) s = &slots[slot_next++ % SLOTS];
    s->gbuf = gbuf;
    s->gsp = c->sp;
    return &s->hb;
}

static uint64_t g_longjmps;  // (stats)

void hle_longjmp(Cpu *c) {
    uint64_t gbuf = c->x[0];
    uint32_t val = (uint32_t)c->x[1];
    JmpSlot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++)
        if (slots[i].gbuf == gbuf) s = &slots[i];
    if (!s) FATAL("[setjmp] longjmp to %llx: no setjmp of it on this thread (lr=%llx)\n", gbuf, c->x[30]);
    if (LD64(gbuf + 0x68) != s->gsp)
        FATAL("[setjmp] longjmp to %llx: its sp %llx isn't the setjmp's %llx\n", gbuf, LD64(gbuf + 0x68), s->gsp);
    if (g_longjmps++ < 8 || ENV("SAVAGEWIND_LOG_LONGJMP"))
        LOG("[setjmp] longjmp to %llx val %u (lr=%llx, back to %llx)\n", gbuf, val, c->x[30], LD64(gbuf + 0x58));
    for (int r = 19; r <= 30; r++) c->x[r] = LD64(gbuf + 8 * (r - 19));
    c->sp = s->gsp;
    for (int r = 8; r <= 15; r++) c->v[r] = LD64(gbuf + 0x70 + 8 * (r - 8));  // (ldp d: upper half zeroed)
    c->x[0] = val ? val : 1;
    longjmp(s->hb, 1);
}

// SAVAGEWIND_LOG_JPEG=1: log each nn::image::JpegEncoder::Encode (its image, quality, output room and result: the
// autosave's caption.jpg goes through it); SAVAGEWIND_LOG_JPEG=dump also saves the first image's pixels to
// build/jpeg_in.raw. JpegEncoder: pixels at +0x18, pixel format +0x20, width/height +0x24, quality +0x30.
static GuestFn jpeg_encode;
static void jpeg_encode_logged(Cpu *c) {
    uint64_t self = c->x[0], out_size = c->x[1], room = c->x[3];
    uint32_t w = LD32(self + 0x24), h = LD32(self + 0x28);
    static int dumped;
    if (!dumped && !strcmp(ENV("SAVAGEWIND_LOG_JPEG"), "dump")) {
        FILE *f = fopen("build/jpeg_in.raw", "wb");
        if (f) { fwrite((void *)(uintptr_t)LD64(self + 0x18), 4, (size_t)w * h, f); fclose(f); }
        dumped = 1;
    }
    uint64_t lr = c->x[30], px = LD64(self + 0x18);
    static int described;
    if (!described++) {
        extern void nvn_debug_textures_at(uint64_t a), backend_debug_copies_at(uint64_t a);
        LOG("[jpeg] pixels at %llx, line alignment %u\n", px, LD32(self + 0x2c));
        nvn_debug_textures_at(px);
        backend_debug_copies_at(px);
    }
    jpeg_encode(c);
    LOG("[jpeg] Encode %ux%u format %u quality %u room %llu -> %d, %llu bytes (lr=%llx)\n", w, h, LD32(self + 0x20),
        LD32(self + 0x30), room, (int)c->x[0], out_size ? LD64(out_size) : 0, lr);
}

void jpeg_debug_init(void) {
    if (!ENV("SAVAGEWIND_LOG_JPEG") || jpeg_encode) return;
    uint64_t a = guest_sym("_ZN2nn5image11JpegEncoder6EncodeEPmPvmPNS0_11ExifBuilderE");
    if (!(jpeg_encode = dispatch_lookup(a))) return;
    hle_override(a, jpeg_encode_logged, "JpegEncoder::Encode");
}
