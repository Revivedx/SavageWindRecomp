// movie:: (nnMedia) implemented with FFmpeg: an Android MediaExtractor/MediaCodec-style API.
// The game owns the objects (vtable at +0); we keep host state at +8.
//
// Extractor slots: 2 SetDataSource(const char* path), 4 GetTrackCount(int*), 5 GetTrackConfiguration(int, MediaData*),
//   6 SelectTrack(int), 9 GetSampleTrackIndex(size_t*), 10 Read(Buffer*), 11 Advance(), 12 GetSampleTime(int64*),
//   15 SetDataSource(DataSource*, const char* path).
// Decoder slots: 2 Configure(MediaData*), 3 RegisterClientEvents(EventType*[4]), 4 GetOutputFormat(MediaData*),
//   6 Start, 9 AcquireInputBufferIndex(int*, int*), 10 GetInputBuffer(int, Buffer*),
//   11 SendInputBufferForDecode(int, int off, int size, int64 time, uint32 flags),
//   12 AcquireOutputBufferIndex(int*, int64*, uint32*, int*), 13 GetOutputBuffer(int, Buffer*),
//   15 ReleaseOutputBufferIndex(int), 18 GetOutputBufferFormat(int, MediaData*).
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

#define MOVIE_EOS 4u

// ------------------------------------------------------------------ MediaData (8-byte handle)
typedef struct { char *key; char *sval; int64_t ival; bool is_str; } MdEntry;
typedef struct { MdEntry e[32]; int n; void *codecpar; } MediaData;

static MediaData *md(uint64_t guest) {
    MediaData **p = (MediaData **)(uintptr_t)guest;
    if (!*p) *p = calloc(1, sizeof(MediaData));
    return *p;
}
static void md_clear(MediaData *m) {
    for (int i = 0; i < m->n; i++) { free(m->e[i].key); free(m->e[i].sval); }
    m->n = 0;
    m->codecpar = NULL;
}
static MdEntry *md_slot(MediaData *m, const char *k) {
    for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].key, k)) return &m->e[i];
    if (m->n == 32) return NULL;
    MdEntry *e = &m->e[m->n++];
    memset(e, 0, sizeof *e);
    e->key = strdup(k);
    return e;
}
static void md_set_int(MediaData *m, const char *k, int64_t v) { MdEntry *e = md_slot(m, k); if (e) { e->ival = v; e->is_str = false; } }
static void md_set_str(MediaData *m, const char *k, const char *v) {
    MdEntry *e = md_slot(m, k);
    if (e) { free(e->sval); e->sval = strdup(v); e->is_str = true; }
}

HLE_FUNC("_ZN5movie9MediaDataC1Ev", md_ctor) { *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)(uintptr_t)calloc(1, sizeof(MediaData)); }
HLE_FUNC("_ZN5movie9MediaDataC1ERKS0_", md_copy) {
    MediaData *src = md(c->x[1]), *dst = calloc(1, sizeof(MediaData));
    for (int i = 0; i < src->n; i++) {
        dst->e[i].key = strdup(src->e[i].key);
        dst->e[i].sval = src->e[i].sval ? strdup(src->e[i].sval) : NULL;
        dst->e[i].ival = src->e[i].ival;
        dst->e[i].is_str = src->e[i].is_str;
    }
    dst->n = src->n;
    dst->codecpar = src->codecpar;
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)(uintptr_t)dst;
}
HLE_FUNC("_ZN5movie9MediaDataD1Ev", md_dtor) {
    MediaData *m = *(MediaData **)(uintptr_t)c->x[0];
    if (m) { md_clear(m); free(m); }
    *(uint64_t *)(uintptr_t)c->x[0] = 0;
}
HLE_FUNC("_ZN5movie9MediaData5ClearEv", md_clear_hle) { md_clear(md(c->x[0])); }
HLE_FUNC("_ZNK5movie9MediaData9FindInt32EPKcPi", md_find_int32) {
    MediaData *m = md(c->x[0]);
    const char *k = (const char *)(uintptr_t)c->x[1];
    for (int i = 0; i < m->n; i++)
        if (!m->e[i].is_str && !strcmp(m->e[i].key, k)) { *(int32_t *)(uintptr_t)c->x[2] = (int32_t)m->e[i].ival; c->x[0] = 1; return; }
    c->x[0] = 0;
}
HLE_FUNC("_ZNK5movie9MediaData10FindStringEPKcPS2_", md_find_string) {
    MediaData *m = md(c->x[0]);
    const char *k = (const char *)(uintptr_t)c->x[1];
    for (int i = 0; i < m->n; i++)
        if (m->e[i].is_str && !strcmp(m->e[i].key, k)) { *(uint64_t *)(uintptr_t)c->x[2] = (uint64_t)(uintptr_t)m->e[i].sval; c->x[0] = 1; return; }
    c->x[0] = 0;
}

// ------------------------------------------------------------------ Buffer (opaque, <= 0x40 bytes)
typedef struct {
    uint64_t base, capacity;
    int32_t offset, size;
    int64_t i64;
    uint32_t u32;
    int32_t i32;
} MBuffer;
_Static_assert(sizeof(MBuffer) <= 0x40, "movie::Buffer size");
#define BUF(x) ((MBuffer *)(uintptr_t)(x))

HLE_FUNC("_ZN5movie6BufferC1Ev", buf_ctor) { memset(BUF(c->x[0]), 0, sizeof(MBuffer)); }
HLE_FUNC("_ZN5movie6BufferC1EPvm", buf_ctor_ext) {
    MBuffer *b = BUF(c->x[0]);
    memset(b, 0, sizeof *b);
    b->base = c->x[1];
    b->capacity = c->x[2];
    b->size = (int32_t)c->x[2];
}
HLE_FUNC("_ZN5movie6BufferC1Em", buf_ctor_alloc) {
    MBuffer *b = BUF(c->x[0]);
    memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)calloc(1, c->x[1] + 64);
    b->capacity = c->x[1];
    b->size = (int32_t)c->x[1];
}
HLE_FUNC("_ZN5movie6BufferC1ERKS0_", buf_copy) { *BUF(c->x[0]) = *BUF(c->x[1]); }
HLE_FUNC("_ZN5movie6BufferaSERKS0_", buf_assign) { *BUF(c->x[0]) = *BUF(c->x[1]); }
HLE_FUNC("_ZN5movie6BufferD1Ev", buf_dtor) { (void)c; }
HLE_FUNC("_ZNK5movie6Buffer4BaseEv", buf_base) { c->x[0] = BUF(c->x[0])->base; }
HLE_FUNC("_ZNK5movie6Buffer4SizeEv", buf_size) { c->x[0] = (uint32_t)BUF(c->x[0])->size; }
HLE_FUNC("_ZNK5movie6Buffer6OffsetEv", buf_offset) { c->x[0] = (uint32_t)BUF(c->x[0])->offset; }
HLE_FUNC("_ZNK5movie6Buffer8CapacityEv", buf_capacity) { c->x[0] = BUF(c->x[0])->capacity; }
HLE_FUNC("_ZN5movie6Buffer8SetRangeEii", buf_set_range) { BUF(c->x[0])->offset = (int32_t)c->x[1]; BUF(c->x[0])->size = (int32_t)c->x[2]; }
HLE_FUNC("_ZN5movie6Buffer18SetDataAndCapacityEPvm", buf_set_data) {
    MBuffer *b = BUF(c->x[0]);
    b->base = c->x[1];
    b->capacity = c->x[2];
}
HLE_FUNC("_ZN5movie6Buffer12SetInt64DataEl", buf_set_i64) { BUF(c->x[0])->i64 = (int64_t)c->x[1]; }
HLE_FUNC("_ZNK5movie6Buffer12GetInt64DataEv", buf_get_i64) { c->x[0] = (uint64_t)BUF(c->x[0])->i64; }
HLE_FUNC("_ZN5movie6Buffer13SetUint32DataEj", buf_set_u32) { BUF(c->x[0])->u32 = (uint32_t)c->x[1]; }
HLE_FUNC("_ZNK5movie6Buffer13GetUint32DataEv", buf_get_u32) { c->x[0] = BUF(c->x[0])->u32; }
HLE_FUNC("_ZN5movie6Buffer12SetInt32DataEi", buf_set_i32) { BUF(c->x[0])->i32 = (int32_t)c->x[1]; }

HLE_FUNC("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", movie_set_allocator) { (void)c; }
HLE_FUNC("_ZN5movie17SetThreadPriorityEi", movie_set_thread_priority) { (void)c; }
HLE_FUNC("_ZN5movie17GetThreadPriorityEv", movie_get_thread_priority) { c->x[0] = 16; }

// ------------------------------------------------------------------ helpers
static uint64_t make_vtable(HostFn *fns, int n, const char *prefix) {
    uint64_t *t = hle_alloc(64 * 8);
    char name[96];
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof name, "<%s slot %d>", prefix, i);
        t[i] = hle_register(name, i < n ? fns[i] : NULL);
    }
    return (uint64_t)(uintptr_t)t;
}

// The game reads the audio and video tracks from different threads; FFmpeg's demuxer is not thread-safe.
static pthread_mutex_t ex_lock = PTHREAD_MUTEX_INITIALIZER;

static void *self_state(Cpu *c) { return (void *)(uintptr_t)((uint64_t *)(uintptr_t)c->x[0])[1]; }

static void signal_guest_event(Cpu *c, uint64_t event) {
    if (!event) return;
    uint64_t saved[8];
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = event;
    guest_call(c, guest_sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE"));
    memcpy(c->x, saved, sizeof saved);
}

static void ok(Cpu *c) { c->x[0] = 0; }

// ------------------------------------------------------------------ Extractor
typedef struct {
    AVFormatContext *fmt;
    int map[16], ntracks;  // exposed track -> stream
    uint64_t selected;  // bitmask of streams
    AVPacket *pkt;
    bool have_pkt, eof;
    bool skippable;  // the launch opening (forced on the first launch, no save yet): A / B / + ends it
} Extractor;

static bool g_skip_held;           // ex_fill: A / B held at the last look
static double g_skip_pressed_at;    // when they were pressed (0 = not yet)

static void ex_fill(Extractor *e) {
    if (e->skippable && !e->eof) {
        // the game skips the opening with Plus (once a save exists); A and B do too (we press Plus for it)
        extern uint64_t input_buttons_now(void);
        extern volatile int g_inject_plus;
        bool held = g_skip_held;
        double pressed_at = g_skip_pressed_at;
        uint64_t b = input_buttons_now();
        bool now = (b & ((1u << 0) | (1u << 1) | (1u << 10))) != 0;  // A / B / Plus
        if (now && !held) {
            LOG("[movie] opening: A/B/+ -> Plus (skip)\n");
            if (b & 3) g_inject_plus = 1;
            if (!pressed_at) pressed_at = av_gettime_relative() / 1e6;
        }
        held = now;
        // With no save file yet the game ignores Plus (first viewing can't be skipped): if the movie still plays
        // half a second after the press, jump to its last half second so it ends the normal way (ending the
        // stream outright makes the game restart it).
        if (pressed_at && av_gettime_relative() / 1e6 - pressed_at > 0.5 && e->fmt->duration > 1000000) {
            pressed_at = 0;
            int64_t to = e->fmt->duration - 500000;
            if (av_seek_frame(e->fmt, -1, to, AVSEEK_FLAG_BACKWARD) >= 0) {
                if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
                LOG("[movie] opening: still playing -> seek to the end\n");
            }
            e->skippable = false;  // (done: later presses on the title / file select are the menus' own)
        }
        g_skip_held = held, g_skip_pressed_at = pressed_at;
    }
    while (!e->have_pkt && !e->eof) {
        if (av_read_frame(e->fmt, e->pkt) < 0) { e->eof = true; break; }
        if (e->selected & (1ull << e->pkt->stream_index)) e->have_pkt = true;
        else av_packet_unref(e->pkt);
    }
}

static void ex_open(Cpu *c, const char *path) {
    Extractor *e = self_state(c);
    // "rom:/region_common/movie/title.webm" -> extracted/romfs/region_common/movie/title.webm
    const char *rel = path && strstr(path, ":/") ? strstr(path, ":/") + 2 : path;
    char host[1024];
    snprintf(host, sizeof host, "%s/%s", g_romfs_dir, rel ? rel : "");
    LOG("[movie] open %s -> %s\n", path ? path : "(null)", host);
    if (avformat_open_input(&e->fmt, host, NULL, NULL) < 0 || avformat_find_stream_info(e->fmt, NULL) < 0) {
        LOG("[movie] cannot open %s\n", host);
        c->x[0] = 1;
        return;
    }
    e->pkt = av_packet_alloc();
    e->ntracks = 0;
    // Link's Awakening's opening at launch could always be skipped (with no save yet the game forces it); no movie of
    // this game is made skippable yet
    e->skippable = rel && strstr(rel, "movie/opening.webm");
    if (e->skippable) g_skip_pressed_at = 0;  // (a press that ended the title's attract replay isn't for this one)
    for (unsigned i = 0; i < e->fmt->nb_streams && e->ntracks < 16; i++)
        if (e->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            (e->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && g_settings.movie_audio && !getenv("SAVAGEWIND_NO_MOVIE_AUDIO")))
            e->map[e->ntracks++] = (int)i;
    c->x[0] = 0;
}

// slot 2: SetDataSource(const char* path). Savage Wind's player (0x70012e95f0) opens its movies with this one,
// right after the constructor, and fails the movie on a nonzero result.
static void ex_set_data_source_path(Cpu *c) { ex_open(c, (const char *)(uintptr_t)c->x[1]); }
// slot 15: SetDataSource(DataSource*, const char* path) (Link's Awakening)
static void ex_set_data_source(Cpu *c) { ex_open(c, (const char *)(uintptr_t)c->x[2]); }

static void ex_get_track_count(Cpu *c) {
    Extractor *e = self_state(c);
    *(int32_t *)(uintptr_t)c->x[1] = e->fmt ? e->ntracks : 0;
    c->x[0] = 0;
}

static void ex_get_track_config(Cpu *c) {
    Extractor *e = self_state(c);
    int i = (int)c->x[1];
    MediaData *m = md(c->x[2]);
    md_clear(m);
    if (!e->fmt || i >= e->ntracks) { c->x[0] = 1; return; }
    AVStream *s = e->fmt->streams[e->map[i]];
    AVCodecParameters *p = s->codecpar;
    m->codecpar = p;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        md_set_str(m, "mime", p->codec_id == AV_CODEC_ID_VP9 ? "video/x-vnd.on2.vp9" :
                              p->codec_id == AV_CODEC_ID_VP8 ? "video/x-vnd.on2.vp8" : "video/avc");
        md_set_int(m, "width", p->width);
        md_set_int(m, "height", p->height);
        int fps = s->avg_frame_rate.den ? s->avg_frame_rate.num / s->avg_frame_rate.den : 30;
        md_set_int(m, "frame-rate", fps ? fps : 30);
    } else {
        md_set_str(m, "mime", p->codec_id == AV_CODEC_ID_VORBIS ? "audio/vorbis" : "audio/mp4a-latm");
        md_set_int(m, "sample-rate", p->sample_rate);
        md_set_int(m, "channel-count", p->ch_layout.nb_channels);
    }
    if (e->fmt->duration > 0) md_set_int(m, "durationUs", e->fmt->duration);
    c->x[0] = 0;
}

static void ex_select_track(Cpu *c) {
    Extractor *e = self_state(c);
    int i = (int)c->x[1];
    if (i >= 0 && i < e->ntracks) e->selected |= 1ull << e->map[i];
    c->x[0] = 0;
}

static void ex_get_sample_track(Cpu *c) {
    Extractor *e = self_state(c);
    ex_fill(e);
    if (!e->have_pkt) { c->x[0] = 1; return; }
    uint64_t t = 0;
    for (int i = 0; i < e->ntracks; i++) if (e->map[i] == e->pkt->stream_index) t = (uint64_t)i;
    *(uint64_t *)(uintptr_t)c->x[1] = t;
    c->x[0] = 0;
}

static void ex_read(Cpu *c) {
    Extractor *e = self_state(c);
    MBuffer *b = BUF(c->x[1]);
    ex_fill(e);
    if (!e->have_pkt) { c->x[0] = 1; return; }
    uint32_t n = (uint32_t)e->pkt->size;
    if (n > b->capacity) { LOG("[movie] packet %u > buffer %llu\n", n, b->capacity); n = (uint32_t)b->capacity; }
    memcpy((void *)(uintptr_t)b->base, e->pkt->data, n);
    b->offset = 0;
    b->size = (int32_t)n;
    c->x[0] = 0;
}

static void ex_advance(Cpu *c) {
    Extractor *e = self_state(c);
    if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
    c->x[0] = 0;
}

static void ex_get_sample_time(Cpu *c) {
    Extractor *e = self_state(c);
    ex_fill(e);
    if (!e->have_pkt) { c->x[0] = 1; return; }
    AVStream *s = e->fmt->streams[e->pkt->stream_index];
    int64_t ts = e->pkt->pts != AV_NOPTS_VALUE ? e->pkt->pts : e->pkt->dts;
    *(int64_t *)(uintptr_t)c->x[1] = av_rescale_q(ts, s->time_base, (AVRational){1, 1000000});
    c->x[0] = 0;
}

// slot 8: SeekTo(int64 timeUs)
static void ex_seek(Cpu *c) {
    Extractor *e = self_state(c);
    if (!e->fmt) { c->x[0] = 1; return; }
    if (e->have_pkt) { av_packet_unref(e->pkt); e->have_pkt = false; }
    e->eof = false;
    av_seek_frame(e->fmt, -1, (int64_t)c->x[1], AVSEEK_FLAG_BACKWARD);
    c->x[0] = 0;
}

// slot 14: GetCachedDuration(out1, int64* durationUs) -- local files are fully cached
static void ex_cached_duration(Cpu *c) {
    Extractor *e = self_state(c);
    int64_t dur = e->fmt && e->fmt->duration > 0 ? e->fmt->duration : INT64_MAX / 4;
    if (c->x[1]) *(int64_t *)(uintptr_t)c->x[1] = 1;
    if (c->x[2]) *(int64_t *)(uintptr_t)c->x[2] = dur;
    c->x[0] = 0;
}

// Movie objects of this boot (host side; a reboot mid-movie closes them): vtables live in the HLE heap.
static uint64_t g_ex_vt, g_dec_vt;
#define MAX_MOVIE_OBJS 32
static void *g_movie_objs[2][MAX_MOVIE_OBJS];  // [0] Extractor, [1] Decoder
static pthread_mutex_t g_movie_lock = PTHREAD_MUTEX_INITIALIZER;
static void movie_track(int kind, void *p, bool add) {
    pthread_mutex_lock(&g_movie_lock);
    for (int i = 0; i < MAX_MOVIE_OBJS; i++)
        if (g_movie_objs[kind][i] == (add ? NULL : p)) { g_movie_objs[kind][i] = add ? p : NULL; break; }
    pthread_mutex_unlock(&g_movie_lock);
}

static void ex_dtor(Cpu *c) {
    Extractor *e = self_state(c);
    if (e && e->fmt) avformat_close_input(&e->fmt);
    if (e) movie_track(0, e, false);
    c->x[0] = 0;
}

static void movie_log_slot(Cpu *c) {
    static int n;
    if (n++ < 20) LOG("[movie] unimplemented virtual (lr=%llx)\n", c->x[30]);
    c->x[0] = 0;
}

#define EX_LOCKED(fn) \
    static void fn##_l(Cpu *c) { pthread_mutex_lock(&ex_lock); fn(c); pthread_mutex_unlock(&ex_lock); }
EX_LOCKED(ex_dtor)
EX_LOCKED(ex_get_track_count)
EX_LOCKED(ex_get_track_config)
EX_LOCKED(ex_select_track)
EX_LOCKED(ex_seek)
EX_LOCKED(ex_get_sample_track)
EX_LOCKED(ex_read)
EX_LOCKED(ex_advance)
EX_LOCKED(ex_get_sample_time)
EX_LOCKED(ex_cached_duration)
EX_LOCKED(ex_set_data_source)
EX_LOCKED(ex_set_data_source_path)

HLE_FUNC("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", movie_extractor_ctor) {
    uint64_t vt = g_ex_vt;
    if (!vt) {
        HostFn f[16] = {ex_dtor_l, ex_dtor_l, ex_set_data_source_path_l, movie_log_slot, ex_get_track_count_l,
                        ex_get_track_config_l, ex_select_track_l, movie_log_slot, ex_seek_l, ex_get_sample_track_l,
                        ex_read_l, ex_advance_l, ex_get_sample_time_l, movie_log_slot, ex_cached_duration_l,
                        ex_set_data_source_l};
        vt = g_ex_vt = make_vtable(f, 16, "Extractor");
    }
    uint64_t *self = (uint64_t *)(uintptr_t)c->x[0];
    self[0] = vt;
    Extractor *e = calloc(1, sizeof(Extractor));
    self[1] = (uint64_t)(uintptr_t)e;
    movie_track(0, e, true);
}

// ------------------------------------------------------------------ Decoder
#define NIN 4
#define NOUT 6
#define NPENDING 16

typedef struct {
    int idx;
    int64_t time;
    uint32_t flags;
} OutReady;

typedef struct {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    bool video;
    uint64_t events[4];  // input available, output available, format changed, error
    uint8_t *in_mem[NIN];
    uint32_t in_cap;
    bool in_busy[NIN];
    uint8_t *out_mem[NOUT];
    uint32_t out_cap, out_size[NOUT];
    bool out_busy[NOUT];
    OutReady ready[NOUT];
    int nready;
    int width, height, stride, sample_rate, channels;
    struct SwsContext *sws;
    uint8_t *rgba;
    AVFrame *pending[NPENDING];
    int64_t pending_time[NPENDING];
    int npending;
    bool eos_pending;
    int64_t eos_time;
    bool stopped;  // Stop / destroyed: no more events (see dec_stop)
    bool playing;  // a started video decoder, counted in g_movie_playing
    pthread_mutex_t lock;
} Decoder;

// Input / output buffers live in the HLE heap, which only a reboot frees: buffers of destroyed decoders are kept
// for the next movie (the title's attract loop replays the opening, ~37 MB of buffers per showing).
static struct { uint8_t *mem; uint32_t size; } g_buf_pool[64];
static int g_nbuf_pool;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *buf_get(uint32_t size) {
    pthread_mutex_lock(&g_pool_lock);
    for (int i = 0; i < g_nbuf_pool; i++)
        if (g_buf_pool[i].size == size) {
            uint8_t *m = g_buf_pool[i].mem;
            g_buf_pool[i] = g_buf_pool[--g_nbuf_pool];
            pthread_mutex_unlock(&g_pool_lock);
            return m;
        }
    pthread_mutex_unlock(&g_pool_lock);
    return hle_alloc(size);
}
static void buf_put(uint8_t *m, uint32_t size) {
    if (!m) return;
    pthread_mutex_lock(&g_pool_lock);
    if (g_nbuf_pool < 64) g_buf_pool[g_nbuf_pool++].mem = m, g_buf_pool[g_nbuf_pool - 1].size = size;
    pthread_mutex_unlock(&g_pool_lock);
}

// Signal one of the game's decoder events unless the decoder was stopped. Under the lock so a Stop on another
// thread can't slip between the check and the signal.
static void dec_signal(Cpu *c, Decoder *d, int i) {
    pthread_mutex_lock(&d->lock);
    if (!d->stopped) signal_guest_event(c, d->events[i]);
    pthread_mutex_unlock(&d->lock);
}

typedef struct NvnTexture NvnTexture;
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride);

static void dec_configure(Cpu *c) {
    Decoder *d = self_state(c);
    MediaData *m = md(c->x[1]);
    AVCodecParameters *p = m->codecpar;
    if (!p) { LOG("[movie] Configure without codec parameters\n"); c->x[0] = 1; return; }
    const AVCodec *codec = avcodec_find_decoder(p->codec_id);
    d->ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d->ctx, p);
    d->ctx->thread_count = 4;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { LOG("[movie] cannot open decoder\n"); c->x[0] = 1; return; }
    d->frame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    d->video = p->codec_type == AVMEDIA_TYPE_VIDEO;
    d->width = p->width;
    d->height = p->height;
    d->stride = (p->width + 255) & ~255;
    d->sample_rate = p->sample_rate;
    d->channels = p->ch_layout.nb_channels;
    d->in_cap = d->video ? 4 << 20 : 256 << 10;
    d->out_cap = d->video ? (uint32_t)(d->stride * ((d->height + 15) & ~15) * 3 / 2) : 64 << 10;
    for (int i = 0; i < NIN; i++) d->in_mem[i] = buf_get(d->in_cap);
    for (int i = 0; i < NOUT; i++) d->out_mem[i] = buf_get(d->out_cap);
    LOG("[movie] decoder %s configured: %dx%d %d Hz %d ch\n", codec->name, d->width, d->height, d->sample_rate, d->channels);
    c->x[0] = 0;
}

static void dec_register_events(Cpu *c) {
    Decoder *d = self_state(c);
    memcpy(d->events, (void *)(uintptr_t)c->x[1], sizeof d->events);
    c->x[0] = 0;
}

static void dec_output_format(Decoder *d, MediaData *m) {
    md_clear(m);
    if (d->video) {
        md_set_str(m, "mime", "video/raw");
        md_set_int(m, "width", d->width);
        md_set_int(m, "height", d->height);
        md_set_int(m, "nv12-y-stride", d->stride);
        md_set_int(m, "nv12-y-offset", 0);
        md_set_int(m, "nv12-uv-offset", d->stride * ((d->height + 15) & ~15));
        md_set_int(m, "nv12-colorspace", 0);
    } else {
        md_set_str(m, "mime", "audio/raw");
        md_set_int(m, "sample-rate", d->sample_rate);
        md_set_int(m, "channel-count", d->channels);
    }
}

static void dec_get_output_format(Cpu *c) { dec_output_format(self_state(c), md(c->x[1])); c->x[0] = 0; }
static void dec_get_output_buffer_format(Cpu *c) { dec_output_format(self_state(c), md(c->x[2])); c->x[0] = 0; }

// Video decoders started and not stopped: a movie is playing. The game shows one of its pictures per frame, so the
// frame rate is held at 30 meanwhile (runtime/hle/framepace.c).
volatile int g_movie_playing;

static void dec_start(Cpu *c) {
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    d->stopped = false;
    if (d->video && !d->playing) d->playing = true, __atomic_add_fetch(&g_movie_playing, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);  // all input buffers are free
    c->x[0] = 0;
}

// slot 6: Stop. The game's player (0x7000912640) stops both decoders, stops its renderer threads, deletes the
// decoders and nulls its pointers to them, and only then tells its decoder thread (0x70009114b0) to exit: an event
// of ours still set by then wakes that thread into a virtual call through the null decoder (signal 11 after the
// title's attract movie, when the thread was slow to run). A stopped hardware decoder signals nothing, so stop
// signalling here and clear what is still set.
static void dec_stop(Cpu *c) {
    Decoder *d = self_state(c);
    static uint64_t clear_event;
    if (!clear_event) clear_event = guest_sym("_ZN2nn2os10ClearEventEPNS0_9EventTypeE");
    pthread_mutex_lock(&d->lock);
    if (d->playing) d->playing = false, __atomic_sub_fetch(&g_movie_playing, 1, __ATOMIC_RELAXED);
    if (!d->stopped) {
        d->stopped = true;
        LOG("[movie] decoder %s stopped\n", d->video ? "video" : "audio");
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        for (int i = 0; i < 4; i++)
            if (d->events[i]) { c->x[0] = d->events[i]; guest_call(c, clear_event); }
        memcpy(c->x, saved, sizeof saved);
    }
    pthread_mutex_unlock(&d->lock);
    c->x[0] = 0;
}

static uint64_t n_acq_in, n_send, n_acq_out, n_get_out, n_rel_out, n_frames;
static void movie_stats(void) {
    static uint64_t last;
    uint64_t now = n_acq_in + n_send + n_acq_out + n_rel_out;
    if (now / 200 != last / 200)
        LOG("[movie] acq_in %llu send %llu frames %llu acq_out %llu get_out %llu rel_out %llu\n", n_acq_in, n_send,
            n_frames, n_acq_out, n_get_out, n_rel_out);
    last = now;
}

static void dec_acquire_input(Cpu *c) {
    Decoder *d = self_state(c);
    n_acq_in++;
    movie_stats();
    pthread_mutex_lock(&d->lock);
    int out_free = 0;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) out_free++;
    if (out_free < 2 || d->npending) { pthread_mutex_unlock(&d->lock); c->x[0] = 1; return; }  // backpressure
    int idx = -1, free_n = 0;
    for (int i = 0; i < NIN; i++)
        if (!d->in_busy[i]) { if (idx < 0) idx = i; else free_n++; }
    if (idx >= 0) d->in_busy[idx] = true;
    pthread_mutex_unlock(&d->lock);
    if (idx < 0) { c->x[0] = 1; return; }
    *(int32_t *)(uintptr_t)c->x[1] = idx;
    if (c->x[2]) *(int32_t *)(uintptr_t)c->x[2] = free_n;
    c->x[0] = 0;
}

static void dec_get_input_buffer(Cpu *c) {
    Decoder *d = self_state(c);
    int i = (int)c->x[1];
    MBuffer *b = BUF(c->x[2]);
    if (i < 0 || i >= NIN) { c->x[0] = 1; return; }
    memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)d->in_mem[i];
    b->capacity = d->in_cap;
    c->x[0] = 0;
}

// Copy a decoded frame into an output slot; caller holds the lock.
static bool emit_frame(Decoder *d, AVFrame *f, int64_t time, uint32_t flags) {
    int slot = -1;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) { slot = i; break; }
    if (slot < 0) return false;
    uint8_t *o = d->out_mem[slot];
    uint32_t size = 0;
    if (flags & MOVIE_EOS) {
        size = 0;
    } else if (d->video) {
        int h = d->height, w = d->width, uv_off = d->stride * ((h + 15) & ~15);
        for (int y = 0; y < h; y++) memcpy(o + y * d->stride, f->data[0] + y * f->linesize[0], w);
        for (int y = 0; y < h / 2; y++) {  // interleave U and V into NV12
            uint8_t *uv = o + uv_off + y * d->stride;
            const uint8_t *u = f->data[1] + y * f->linesize[1], *v = f->data[2] + y * f->linesize[2];
            for (int x = 0; x < w / 2; x++) { uv[2 * x] = u[x]; uv[2 * x + 1] = v[x]; }
        }
        size = (uint32_t)(uv_off + d->stride * (h / 2));
    } else {
        // planar float -> interleaved s16
        int ch = f->ch_layout.nb_channels, n = f->nb_samples;
        int16_t *out = (int16_t *)o;
        if ((uint32_t)(n * ch * 2) > d->out_cap) n = (int)(d->out_cap / (ch * 2));
        // FFmpeg reorders Vorbis 5.1 to FL FR FC LFE BL BR; the game expects Vorbis order (FL FC FR BL BR LFE)
        // and reorders itself.
        static const int vorbis51[6] = {0, 2, 1, 4, 5, 3};
        bool native = ch == 6 && d->ctx->codec_id == AV_CODEC_ID_VORBIS;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++) {
                int src = native ? vorbis51[k] : k;
                float s = f->format == AV_SAMPLE_FMT_FLTP ? ((const float *)f->data[src])[i] : 0.0f;
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                out[i * ch + k] = (int16_t)(s * 32767.0f);
            }
        size = (uint32_t)(n * ch * 2);
    }
    d->out_busy[slot] = true;
    d->out_size[slot] = size;
    d->ready[d->nready++] = (OutReady){slot, time, flags};
    return true;
}

// Emit queued frames (and a pending end-of-stream marker) into free output slots.
static bool flush_pending(Decoder *d) {
    bool any = false;
    while (d->npending && emit_frame(d, d->pending[0], d->pending_time[0], 0)) {
        av_frame_free(&d->pending[0]);
        memmove(d->pending, d->pending + 1, (d->npending - 1) * sizeof d->pending[0]);
        memmove(d->pending_time, d->pending_time + 1, (d->npending - 1) * sizeof d->pending_time[0]);
        d->npending--;
        any = true;
    }
    if (!d->npending && d->eos_pending && emit_frame(d, NULL, d->eos_time, MOVIE_EOS)) {
        d->eos_pending = false;
        any = true;
    }
    return any;
}

static void dec_send_input(Cpu *c) {
    Decoder *d = self_state(c);
    n_send++;
    int idx = (int)c->x[1], off = (int)c->x[2], size = (int)c->x[3];
    int64_t time = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    pthread_mutex_lock(&d->lock);
    if (size > 0 && idx >= 0 && idx < NIN) {
        av_packet_unref(d->pkt);
        d->pkt->data = d->in_mem[idx] + off;
        d->pkt->size = size;
        d->pkt->pts = time;
        avcodec_send_packet(d->ctx, d->pkt);
    }
    if (flags & MOVIE_EOS) avcodec_send_packet(d->ctx, NULL);
    bool produced = false;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        int64_t t = d->frame->pts != AV_NOPTS_VALUE ? d->frame->pts : time;
        // keep order: queue behind anything already pending
        if (d->npending || !emit_frame(d, d->frame, t, 0)) {
            if (d->npending < NPENDING) {
                d->pending[d->npending] = av_frame_clone(d->frame);
                d->pending_time[d->npending++] = t;
            } else {
                LOG("[movie] output queue full, dropping frame\n");
            }
        }
        n_frames++;
        produced = true;
    }
    if (flags & MOVIE_EOS) { d->eos_pending = true; d->eos_time = time; }
    produced |= flush_pending(d);
    if (idx >= 0 && idx < NIN) d->in_busy[idx] = false;
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);
    if (produced) dec_signal(c, d, 1);
    c->x[0] = 0;
}

static void dec_acquire_output(Cpu *c) {
    Decoder *d = self_state(c);
    n_acq_out++;
    pthread_mutex_lock(&d->lock);
    if (!d->nready) { pthread_mutex_unlock(&d->lock); c->x[0] = 1; return; }
    OutReady r = d->ready[0];
    memmove(d->ready, d->ready + 1, --d->nready * sizeof *d->ready);
    int remaining = d->nready;
    pthread_mutex_unlock(&d->lock);
    *(int32_t *)(uintptr_t)c->x[1] = r.idx;
    if (c->x[2]) *(int64_t *)(uintptr_t)c->x[2] = r.time;
    if (c->x[3]) *(uint32_t *)(uintptr_t)c->x[3] = r.flags;
    if (c->x[4]) *(int32_t *)(uintptr_t)c->x[4] = remaining;
    c->x[0] = 0;
}

static void dec_get_output_buffer(Cpu *c) {
    Decoder *d = self_state(c);
    n_get_out++;
    int i = (int)c->x[1];
    MBuffer *b = BUF(c->x[2]);
    if (i < 0 || i >= NOUT) { c->x[0] = 1; return; }
    if (d->video && b->base && b->capacity == 8) {
        // texture output mode: the buffer points at the game's NVNtexture; write RGBA into it
        NvnTexture *t = *(NvnTexture **)(uintptr_t)b->base;
        int w = d->width, h = d->height;
        if (!d->sws)
            d->sws = sws_getContext(w, h, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        if (!d->rgba) d->rgba = malloc((size_t)w * h * 4);
        const uint8_t *src[2] = {d->out_mem[i], d->out_mem[i] + d->stride * ((h + 15) & ~15)};
        int src_stride[2] = {d->stride, d->stride};
        uint8_t *dst[1] = {d->rgba};
        int dst_stride[1] = {w * 4};
        sws_scale(d->sws, src, src_stride, 0, h, dst, dst_stride);
        renderer_upload_rgba(t, d->rgba, (uint32_t)w, (uint32_t)h, (uint32_t)w * 4);
        c->x[0] = 0;
        return;
    }
    if (d->video && b->base && b->capacity >= (uint64_t)d->width * d->height * 3 / 2) {
        // Savage Wind hands its own buffer (SetDataAndCapacity, 0x70012eaa98) and copies it into its Y and UV
        // textures itself (0x70012ea664): NV12 packed tight, Y w*h at 0 and UV right after it.
        int w = d->width, h = d->height;
        uint8_t *dst = (uint8_t *)(uintptr_t)b->base, *src = d->out_mem[i];
        const uint8_t *uv = src + d->stride * ((h + 15) & ~15);
        for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, src + (size_t)y * d->stride, w);
        for (int y = 0; y < h / 2; y++) memcpy(dst + (size_t)w * h + (size_t)y * (w / 2 * 2), uv + (size_t)y * d->stride, w / 2 * 2);
        b->offset = 0;
        b->size = w * h + (w / 2) * (h / 2) * 2;
        c->x[0] = 0;
        return;
    }
    memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)d->out_mem[i];
    b->capacity = d->out_cap;
    b->size = (int32_t)d->out_size[i];
    c->x[0] = 0;
}

static void dec_release_output(Cpu *c) {
    Decoder *d = self_state(c);
    n_rel_out++;
    dec_signal(c, d, 0);  // input may be accepted again
    int i = (int)c->x[1];
    pthread_mutex_lock(&d->lock);
    if (i >= 0 && i < NOUT) d->out_busy[i] = false;
    bool produced = flush_pending(d);
    pthread_mutex_unlock(&d->lock);
    if (produced) dec_signal(c, d, 1);
    c->x[0] = 0;
}

static void dec_dtor(Cpu *c) {
    Decoder *d = self_state(c);
    if (!d) { c->x[0] = 0; return; }
    dec_stop(c);
    pthread_mutex_lock(&d->lock);
    if (d->ctx) avcodec_free_context(&d->ctx);
    for (int i = 0; i < NIN; i++) buf_put(d->in_mem[i], d->in_cap), d->in_mem[i] = NULL;
    for (int i = 0; i < NOUT; i++) buf_put(d->out_mem[i], d->out_cap), d->out_mem[i] = NULL;
    for (int i = 0; i < d->npending; i++) av_frame_free(&d->pending[i]);
    d->npending = d->nready = 0;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    if (d->sws) sws_freeContext(d->sws), d->sws = NULL;
    free(d->rgba), d->rgba = NULL;
    pthread_mutex_unlock(&d->lock);
    movie_track(1, d, false);  // (d itself stays: the game's object may still point at it)
    c->x[0] = 0;
}

// Reboot: close what a movie left open (decoder threads, the file), new vtables in the next boot's HLE heap.
RT_ON_RESET(movie_reset) {
    for (int i = 0; i < MAX_MOVIE_OBJS; i++) {
        Extractor *e = g_movie_objs[0][i];
        if (e && e->fmt) avformat_close_input(&e->fmt);
        Decoder *d = g_movie_objs[1][i];
        if (d && d->ctx) avcodec_free_context(&d->ctx);
        g_movie_objs[0][i] = g_movie_objs[1][i] = NULL;
    }
    g_ex_vt = g_dec_vt = 0;
    g_nbuf_pool = 0;  // (the HLE heap starts over)
    g_movie_playing = 0;
    g_skip_held = false, g_skip_pressed_at = 0;
}

static void dec_get_counts(Cpu *c) {  // slot 14: (int*, int64*) queried after Configure
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 0;
    if (c->x[2]) *(int64_t *)(uintptr_t)c->x[2] = 0;
    c->x[0] = 0;
}

HLE_FUNC("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", movie_decoder_ctor) {
    uint64_t vt = g_dec_vt;
    if (!vt) {
        HostFn f[19] = {dec_dtor, dec_dtor, dec_configure, dec_register_events, dec_get_output_format,
                        dec_start, dec_stop, ok, ok, dec_acquire_input, dec_get_input_buffer, dec_send_input,
                        dec_acquire_output, dec_get_output_buffer, dec_get_counts, dec_release_output, ok,
                        movie_log_slot, dec_get_output_buffer_format};
        vt = g_dec_vt = make_vtable(f, 19, "Decoder");
    }
    uint64_t *self = (uint64_t *)(uintptr_t)c->x[0];
    Decoder *d = calloc(1, sizeof *d);
    movie_track(1, d, true);
    pthread_mutex_init(&d->lock, NULL);
    self[0] = vt;
    self[1] = (uint64_t)(uintptr_t)d;
}
