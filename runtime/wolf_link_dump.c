#include "wolf_link_dump.h"
#include "platform.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libavutil/aes.h>
#include <libavutil/hmac.h>
#include <libavutil/mem.h>

typedef struct {
    uint8_t hmac_key[16];
    char type[14];
    uint8_t reserved, magic_size, magic[16], xor_pad[32];
} MasterKey;
_Static_assert(sizeof(MasterKey) == 80, "amiibo master key layout");

static void wipe(void *data, size_t size) {
    volatile uint8_t *bytes = data;
    while (size--) *bytes++ = 0;
}

static bool keyed_hash(const uint8_t key[16], const uint8_t *first, size_t first_size,
                       const uint8_t *second, size_t second_size, uint8_t out[32]) {
    AVHMAC *context = av_hmac_alloc(AV_HMAC_SHA256);
    if (!context) return false;
    av_hmac_init(context, key, 16);
    av_hmac_update(context, first, (unsigned)first_size);
    if (second_size) av_hmac_update(context, second, (unsigned)second_size);
    bool ok = av_hmac_final(context, out, 32) == 32;
    av_hmac_free(context);
    return ok;
}

static bool derive_key(const MasterKey *master, const uint8_t tag[0x208], uint8_t out[48]) {
    uint8_t seed[64], prepared[128], block[32];
    memcpy(seed, tag + 0x29, 2);
    memset(seed + 2, 0, 14);
    memcpy(seed + 16, tag + 0x1d4, 8);
    memcpy(seed + 24, tag + 0x1d4, 8);
    memcpy(seed + 32, tag + 0x1e8, 32);
    size_t size = strnlen(master->type, sizeof master->type);
    memcpy(prepared, master->type, size);
    prepared[size++] = 0;
    size_t leading = 16 - master->magic_size;
    memcpy(prepared + size, seed, leading);
    size += leading;
    memcpy(prepared + size, master->magic, master->magic_size);
    size += master->magic_size;
    memcpy(prepared + size, seed + 16, 16);
    size += 16;
    for (int index = 0; index < 32; index++) prepared[size++] = seed[32 + index] ^ master->xor_pad[index];
    bool ok = true;
    for (int index = 0; index < 2; index++) {
        uint8_t counter[] = {0, (uint8_t)index};
        if (!keyed_hash(master->hmac_key, counter, sizeof counter, prepared, size, block)) { ok = false; break; }
        memcpy(out + index * 32, block, index ? 16 : 32);
    }
    wipe(seed, sizeof seed);
    wipe(prepared, sizeof prepared);
    wipe(block, sizeof block);
    return ok;
}

static bool cipher(const uint8_t key[16], const uint8_t initial[16], uint8_t *data, size_t size) {
    struct AVAES *context = av_aes_alloc();
    if (!context) return false;
    if (av_aes_init(context, key, 128, 0) < 0) { av_free(context); return false; }
    uint8_t counter[16], stream[16];
    memcpy(counter, initial, 16);
    for (size_t offset = 0; offset < size; offset += 16) {
        av_aes_crypt(context, stream, counter, 1, NULL, 0);
        for (size_t index = 0; index < 16 && offset + index < size; index++) data[offset + index] ^= stream[index];
        for (int index = 15; index >= 0 && !++counter[index]; index--) {}
    }
    wipe(counter, sizeof counter);
    wipe(stream, sizeof stream);
    av_free(context);
    return true;
}

static void unpack(const uint8_t *tag, uint8_t *internal) {
    memcpy(internal, tag + 8, 8);
    memcpy(internal + 8, tag + 0x80, 0x20);
    memcpy(internal + 0x28, tag + 0x10, 0x24);
    memcpy(internal + 0x4c, tag + 0xa0, 0x168);
    memcpy(internal + 0x1b4, tag + 0x34, 0x20);
    memcpy(internal + 0x1d4, tag, 8);
    memcpy(internal + 0x1dc, tag + 0x54, 0x2c);
}

static bool equal_hash(const uint8_t *first, const uint8_t *second) {
    unsigned difference = 0;
    for (int index = 0; index < 32; index++) difference |= first[index] ^ second[index];
    return difference == 0;
}

WolfLinkStatus wolf_link_dump_read(const char *dump, const char *keys, WolfLinkDump *out) {
    memset(out, 0, sizeof *out);
    uint8_t tag[573];
    FILE *file = fopen(dump, "rb");
    if (!file) return WOLF_DUMP_MISSING;
    size_t size = fread(tag, 1, sizeof tag, file);
    bool readable = !ferror(file);
    fclose(file);
    if (!readable || (size != 532 && size != 540 && size != 572) || tag[0] != 4 ||
        tag[3] != (uint8_t)(0x88 ^ tag[0] ^ tag[1] ^ tag[2]) ||
        tag[8] != (uint8_t)(tag[4] ^ tag[5] ^ tag[6] ^ tag[7])) return WOLF_DUMP_INVALID;
    const uint8_t model[8] = {1, 3, 0, 0, 2, 0x4f, 9, 2};
    if (memcmp(tag + 0x54, model, sizeof model)) return WOLF_DUMP_NOT_WOLF;
    file = fopen(keys, "rb");
    if (!file) return WOLF_KEYS_MISSING;
    MasterKey masters[2];
    size = fread(masters, 1, sizeof masters, file);
    bool valid_keys = size == sizeof masters && fgetc(file) == EOF && !ferror(file) &&
                      masters[0].magic_size <= 16 && masters[1].magic_size <= 16;
    fclose(file);
    if (!valid_keys) { wipe(masters, sizeof masters); return WOLF_KEYS_INVALID; }
    uint8_t internal[0x208], plain[0x208], data_key[48], tag_key[48], hash[32];
    unpack(tag, internal);
    WolfLinkStatus status = WOLF_DUMP_MEMORY;
    bool ok = derive_key(&masters[0], internal, data_key) && derive_key(&masters[1], internal, tag_key);
    if (ok) {
        memcpy(plain, internal, sizeof plain);
        ok = cipher(data_key, data_key + 16, plain + 0x2c, 0x188) &&
             keyed_hash(tag_key + 32, plain + 0x1d4, 0x34, NULL, 0, hash);
        if (ok) {
            status = WOLF_AUTH_FAILED;
            bool tag_valid = equal_hash(hash, internal + 0x1b4);
            memcpy(plain + 0x1b4, hash, 32);
            ok = keyed_hash(data_key + 32, plain + 0x29, 0x1df, NULL, 0, hash);
            if (ok && tag_valid && equal_hash(hash, internal + 8)) {
                memcpy(out->uid, tag, 3);
                memcpy(out->uid + 3, tag + 4, 4);
                out->flags = plain[0x2c];
                for (int index = 0; index < 10; index++)
                    out->nickname[index] = (uint16_t)(plain[0x38 + index * 2] << 8 | plain[0x39 + index * 2]);
                out->app_id = (uint32_t)plain[0xb6] << 24 | (uint32_t)plain[0xb7] << 16 | plain[0xb8] << 8 | plain[0xb9];
                memcpy(out->area, plain + 0xdc, sizeof out->area);
                status = WOLF_DUMP_OK;
            }
        }
    }
    wipe(masters, sizeof masters);
    wipe(data_key, sizeof data_key);
    wipe(tag_key, sizeof tag_key);
    wipe(plain, sizeof plain);
    return status;
}

static void save_path(const WolfLinkDump *tag, char out[80]) {
    snprintf(out, 80, "amiibo_data/wolf_link-%02x%02x%02x%02x%02x%02x%02x.bin",
             tag->uid[0], tag->uid[1], tag->uid[2], tag->uid[3], tag->uid[4], tag->uid[5], tag->uid[6]);
}

WolfLinkStatus wolf_link_dump_load(WolfLinkDump *out) {
    WolfLinkStatus status = wolf_link_dump_read("amiibo/wolf_link.bin", "amiibo/key_retail.bin", out);
    if (status != WOLF_DUMP_OK) return status;
    char path[80];
    save_path(out, path);
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) return WOLF_DUMP_OK;
        memset(out, 0, sizeof *out);
        return WOLF_SAVE_INVALID;
    }
    uint8_t saved[8 + 0xd8];
    bool valid = fread(saved, 1, sizeof saved, file) == sizeof saved && fgetc(file) == EOF && !ferror(file) &&
                 !memcmp(saved, "SWLF", 4);
    fclose(file);
    if (!valid) { memset(out, 0, sizeof *out); return WOLF_SAVE_INVALID; }
    out->app_id = saved[4] | saved[5] << 8 | saved[6] << 16 | (uint32_t)saved[7] << 24;
    memcpy(out->area, saved + 8, sizeof out->area);
    out->flags |= WOLF_HAS_AREA;
    return WOLF_DUMP_OK;
}

bool wolf_link_dump_save(const WolfLinkDump *tag) {
    if (mkdir("amiibo_data", 0700) != 0 && errno != EEXIST) return false;
    char path[80], temporary[88];
    save_path(tag, path);
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    uint8_t header[8] = {'S', 'W', 'L', 'F', tag->app_id, tag->app_id >> 8, tag->app_id >> 16, tag->app_id >> 24};
    bool ok = fwrite(header, 1, sizeof header, file) == sizeof header &&
              fwrite(tag->area, 1, sizeof tag->area, file) == sizeof tag->area;
    ok = fclose(file) == 0 && ok;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok) ok = rename(temporary, path) == 0;
#endif
    if (!ok) remove(temporary);
    return ok;
}

void wolf_link_dump_nickname(const WolfLinkDump *tag, char out[41]) {
    size_t used = 0;
    for (int index = 0; index < 10 && tag->nickname[index]; index++) {
        uint32_t character = tag->nickname[index];
        if (character >= 0xd800 && character <= 0xdbff && index < 9 &&
            tag->nickname[index + 1] >= 0xdc00 && tag->nickname[index + 1] <= 0xdfff)
            character = 0x10000 + ((character - 0xd800) << 10) + tag->nickname[++index] - 0xdc00;
        else if (character >= 0xd800 && character <= 0xdfff) character = 0xfffd;
        if (character < 0x80) out[used++] = character;
        else if (character < 0x800) { out[used++] = 0xc0 | character >> 6; out[used++] = 0x80 | (character & 63); }
        else if (character < 0x10000) {
            out[used++] = 0xe0 | character >> 12; out[used++] = 0x80 | (character >> 6 & 63); out[used++] = 0x80 | (character & 63);
        } else {
            out[used++] = 0xf0 | character >> 18; out[used++] = 0x80 | (character >> 12 & 63);
            out[used++] = 0x80 | (character >> 6 & 63); out[used++] = 0x80 | (character & 63);
        }
    }
    out[used] = 0;
}

const char *wolf_link_dump_status_text(WolfLinkStatus status) {
    switch (status) {
    case WOLF_DUMP_OK: return "Own Wolf Link data loaded.";
    case WOLF_DUMP_MISSING: return "Missing amiibo/wolf_link.bin.";
    case WOLF_DUMP_NOT_WOLF: return "The dump is not a Wolf Link amiibo.";
    case WOLF_KEYS_MISSING: return "Missing amiibo/key_retail.bin.";
    case WOLF_KEYS_INVALID: return "The amiibo keys file is invalid.";
    case WOLF_AUTH_FAILED: return "The dump failed authentication. Check your dump and keys.";
    case WOLF_DUMP_MEMORY: return "Could not read amiibo data. Try again.";
    case WOLF_SAVE_INVALID: return "The saved Wolf Link application data is invalid or unreadable.";
    default: return "The amiibo dump is invalid. Use a 532, 540 or 572-byte NTAG215 dump.";
    }
}
