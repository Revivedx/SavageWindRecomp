// The camera settings: the Classic Camera mod's styles put in the game's camera program. The program here is a small one, laid out as
// the game's own (AAMP 2): root -> Action -> a list per camera, each with Def (its class) and SInst (its values), and,
// as in the game's, equal values kept once.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CAMERA_TUNE_TEST
#include "../runtime/camera_tune.c"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static uint8_t file[8192];
static uint32_t top;

static void put32(uint32_t at, uint32_t v) { memcpy(file + at, &v, 4); }
static void put16(uint32_t at, uint16_t v) { memcpy(file + at, &v, 2); }
static uint32_t take(uint32_t n) { uint32_t at = top; top += n; return at; }

static void list(uint32_t at, const char *name, uint32_t lists, uint16_t nlists, uint32_t objs, uint16_t nobjs) {
    put32(at, name_crc(name));
    put16(at + 4, (uint16_t)(lists ? (lists - at) / 4 : 0)), put16(at + 6, nlists);
    put16(at + 8, (uint16_t)(objs ? (objs - at) / 4 : 0)), put16(at + 10, nobjs);
}

static void object(uint32_t at, const char *name, uint32_t params, uint16_t n) {
    put32(at, name_crc(name));
    put16(at + 4, (uint16_t)((params - at) / 4)), put16(at + 6, n);
}

static void param(uint32_t at, const char *name, uint32_t data, uint8_t type) {
    put32(at, name_crc(name));
    put32(at + 4, ((data - at) / 4) | (uint32_t)type << 24);
}

// The value of parameter i of the block at params, wherever it is now
static float got(uint32_t params, int i) {
    uint32_t at = params + (uint32_t)i * 8, info;
    memcpy(&info, file + at + 4, 4);
    float v;
    memcpy(&v, file + at + (info & 0xFFFFFF) * 4, 4);
    return v;
}

static uint32_t begin(int cameras, uint32_t *cams) {
    memset(file, 0, sizeof file);
    memcpy(file, "AAMP", 4);
    put32(4, 2), put32(8, 3), put32(0x14, 4);
    memcpy(file + 0x30, "xml", 4);
    top = 0x34;
    uint32_t root = take(12), lists = take(12);
    *cams = take(12 * (uint32_t)cameras);
    list(root, "param_root", lists, 1, 0, 0);
    list(lists, "Action", *cams, (uint16_t)cameras, 0, 0);
    return root;
}

// A camera; shared: its values are those of another camera, one place for both. Returns its SInst's parameters.
static uint32_t camera(uint32_t at, const char *cls, uint32_t shared) {
    uint32_t objs = take(16), defp = take(8), sinst = take(40), data = take(64);
    list(at, "Cam", 0, 0, objs, 2);
    object(objs, "Def", defp, 1);
    object(objs + 8, "SInst", sinst, 5);
    strcpy((char *)file + data, cls);
    param(defp, "ClassName", data, 7);
    uint32_t values = shared ? shared : take(20);
    static const char *const names[] = {"radius", "radiusMin", "radiusMax", "OffsetYMin", "OffsetYMax"};
    static const float start[] = {3, 2, 4, 0, 0.5f};
    for (int i = 0; i < 5; i++) {
        param(sinst + (uint32_t)i * 8, names[i], values + (uint32_t)i * 4, 1);
        memcpy(file + values + i * 4, &start[i], 4);
    }
    return sinst;
}

static bool get_float(const Aamp *a, size_t obj, const char *name, float *out) {
    size_t p = find_param(a, obj, name_crc(name)), at = p ? param_data(a, p, 4) : 0;
    if (!at || u32_at(a, p + 4) >> 24 != 1) return false;
    memcpy(out, a->d + at, 4);
    return true;
}

static bool is_class(const Aamp *a, size_t def, const char *name) {
    size_t p = find_param(a, def, name_crc("ClassName")), s = p ? param_data(a, p, strlen(name) + 1) : 0;
    return s && !memcmp(a->d + s, name, strlen(name) + 1);
}

int main(void) {
    // A style: values set where they are, and ones the object didn't have added to it (a camera turned into another
    // kind), its other values kept
    uint32_t cams;
    begin(2, &cams);
    uint32_t chase = camera(cams, "CameraChase", 0);
    uint32_t next = camera(cams + 12, "CameraChase", 0);
    uint32_t act = name_crc("Action"), cam = name_crc("Cam"), si = name_crc("SInst");
    const CamStyleParam style[] = {
        {act, cam, si, name_crc("radius"), CAM_F32, {7}},
        {act, cam, si, name_crc("fovy"), CAM_F32, {58}},
        {act, cam, si, name_crc("worldBaseOffset"), CAM_VEC3, {0, 0.4f, 0}},
        {act, cam, si, name_crc("gyro"), CAM_INT, {1}},
        {act, cam, name_crc("Def"), name_crc("ClassName"), CAM_STR32, {0}, "CameraLockOnAimingAt"},
        {act, name_crc("Nope"), si, name_crc("radius"), CAM_F32, {9}},  // (no such camera: left out)
    };
    Aamp s = {file, top, sizeof file};
    size_t sroot = 0x30 + 4;
    assert(apply_style(&s, sroot, style, 6) == 5);
    size_t obj = find_object(&s, list_child(&s, list_child(&s, sroot, 0), 0), si), def;
    float f;
    assert(u16_at(&s, obj + 6) == 8);  // (5 and 3 new)
    assert(get_float(&s, obj, "radius", &f) && NEAR(f, 7) && get_float(&s, obj, "fovy", &f) && NEAR(f, 58));
    assert(get_float(&s, obj, "radiusMin", &f) && NEAR(f, 2) && get_float(&s, obj, "OffsetYMax", &f) && NEAR(f, 0.5f));
    size_t p = find_param(&s, obj, name_crc("worldBaseOffset"));
    assert(p && u32_at(&s, p + 4) >> 24 == 4 && param_data(&s, p, 12) > p);
    memcpy(&f, s.d + param_data(&s, p, 12) + 4, 4);
    assert(NEAR(f, 0.4f));
    p = find_param(&s, obj, name_crc("gyro"));
    assert(p && u32_at(&s, p + 4) >> 24 == 2 && s.d[param_data(&s, p, 4)] == 1);
    def = find_object(&s, list_child(&s, list_child(&s, sroot, 0), 0), name_crc("Def"));
    assert(is_class(&s, def, "CameraLockOnAimingAt"));
    assert(NEAR(got(next, 0), 3));  // (the other camera, with the same name, isn't the first's)
    assert(NEAR(got(chase, 0), 3));  // (the old parameters stay where they were, for no one)
    // ... and with no room, nothing
    begin(1, &cams);
    camera(cams, "CameraChase", 0);
    s = (Aamp){file, top, top + 16};
    assert(apply_style(&s, sroot, style, 6) == 0);
    // The mod's styles: the base and the options over it, each something
    assert(sizeof style_classic / sizeof style_classic[0] > 300);
    for (int i = 2; i < CAMERA_STYLES; i++) assert(style_over[i].n > 0);
    // The whole program: the game's camera (style 0), what isn't a program and a style out of range change nothing; a
    // style says the file's new size
    begin(1, &cams);
    camera(cams, "CameraChase", 0);
    size_t size = top;
    assert(camera_style_program(file, &size, sizeof file, 0) == 0 && size == top);
    assert(camera_style_program(file, &size, sizeof file, CAMERA_STYLES) == 0 && size == top);
    uint8_t junk[64] = "not aamp";
    size_t junk_size = sizeof junk;
    assert(camera_style_program(junk, &junk_size, sizeof junk, 1) == 0);
    camera_style_program(file, &size, sizeof file, 1);  // (a made-up program: none of the mod's cameras, nothing set)
    uint32_t total;
    memcpy(&total, file + 0xC, 4);
    assert(total == size);
    for (int i = 0; i < CAMERA_STYLES; i++) assert(g_camera_style_names[i] && *g_camera_style_names[i]);

    // What a program holds: each camera's numbers in order, and whether two programs' cameras are the same kinds
    begin(2, &cams);
    camera(cams, "CameraChase", 0);
    camera(cams + 12, "CameraAiming", 0);
    static uint8_t other[sizeof file];
    size = top;
    memcpy(other, file, top);
    CameraValue vals[16];
    assert(camera_program_values(file, top, vals, 16) == 10);
    assert(vals[0].action == 0 && vals[5].action == 1 && vals[0].crc == name_crc("radius") && vals[0].type == CAM_F32);
    float r0;
    memcpy(&r0, vals[0].v, 4);
    assert(NEAR(r0, 3) && camera_program_values(file, top, vals, 3) == 3);
    assert(camera_program_same_kinds(file, top, other, top));
    s = (Aamp){other, top, sizeof other};
    assert(apply_style(&s, sroot, style, 4) == 4);  // (numbers only: still the same kinds)
    assert(camera_program_same_kinds(file, top, other, s.n));
    assert(apply_style(&s, sroot, style + 4, 1) == 1);  // (the first camera made another kind)
    assert(!camera_program_same_kinds(file, top, other, s.n));
    assert(!camera_program_same_kinds(file, top, junk, sizeof junk) && !camera_program_values(junk, sizeof junk, vals, 16));

    // Room in a SARC: the files after the program move, the ones before stay
    static uint8_t sarc[0x400];
    memcpy(sarc, "SARC", 4);
    uint32_t sz = 0x200, doff = 0x100;
    memcpy(sarc + 8, &sz, 4), memcpy(sarc + 0xC, &doff, 4);
    sarc[0x1A] = 3;
    uint32_t ranges[3][2] = {{0, 0x20}, {0x20, 0x40}, {0x40, 0x80}};
    for (uint32_t i = 0; i < 3; i++) memcpy(sarc + 0x20 + i * 16 + 8, ranges[i], 8);
    memset(sarc + doff + 0x40, 0xAB, 0x40);
    assert(!camera_sarc_grow(sarc, sz, sz + 0x10, doff, 0x20, 0x40, 0x20));  // (no room)
    assert(camera_sarc_grow(sarc, sz, sizeof sarc, doff, 0x20, 0x40, 0x20));
    uint32_t r[2];
    memcpy(r, sarc + 0x20 + 8, 8);
    assert(r[0] == 0 && r[1] == 0x20);
    memcpy(r, sarc + 0x30 + 8, 8);
    assert(r[0] == 0x20 && r[1] == 0x60);
    memcpy(r, sarc + 0x40 + 8, 8);
    assert(r[0] == 0x60 && r[1] == 0xA0);
    memcpy(&sz, sarc + 8, 4);
    assert(sz == 0x220 && sarc[doff + 0x60] == 0xAB && !sarc[doff + 0x40]);

    puts("PASS: camera_tune");
}
