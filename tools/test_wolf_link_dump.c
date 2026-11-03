#include <assert.h>
#include <unistd.h>
#include "../runtime/wolf_link_dump.c"

static void write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void fixture(MasterKey masters[2], uint8_t tag[572]) {
    memset(masters, 0, 2 * sizeof *masters);
    memcpy(masters[0].type, "test data", 10);
    memcpy(masters[1].type, "test tag", 9);
    for (int key = 0; key < 2; key++) {
        masters[key].magic_size = 8;
        for (int index = 0; index < 16; index++) masters[key].hmac_key[index] = index + key * 31;
        for (int index = 0; index < 8; index++) masters[key].magic[index] = index + 17;
        for (int index = 0; index < 32; index++) masters[key].xor_pad[index] = index + 43;
    }
    uint8_t plain[0x208] = {0};
    const uint8_t identity[8] = {4, 0x11, 0x22, 0x88 ^ 4 ^ 0x11 ^ 0x22, 0x33, 0x44, 0x55, 0x66};
    const uint8_t model[8] = {1, 3, 0, 0, 2, 0x4f, 9, 2};
    memcpy(plain + 0x1d4, identity, 8);
    memcpy(plain + 0x1dc, model, 8);
    plain[0] = 0x33 ^ 0x44 ^ 0x55 ^ 0x66;
    for (int index = 0; index < 32; index++) plain[0x1e8 + index] = index;
    plain[0x2c] = WOLF_REGISTERED | WOLF_HAS_AREA;
    for (int index = 0; index < 4; index++) plain[0x39 + index * 2] = "Wolf"[index];
    plain[0xb6] = 0x10; plain[0xb7] = 0x19; plain[0xb8] = 0xc8;
    plain[0xdc + 0x11] = 20;
    plain[0xdc + 0x12] = 36;
    uint8_t data_key[48], tag_key[48];
    assert(derive_key(&masters[0], plain, data_key) && derive_key(&masters[1], plain, tag_key));
    assert(keyed_hash(tag_key + 32, plain + 0x1d4, 0x34, NULL, 0, plain + 0x1b4));
    assert(keyed_hash(data_key + 32, plain + 0x29, 0x1df, NULL, 0, plain + 8));
    assert(cipher(data_key, data_key + 16, plain + 0x2c, 0x188));
    memset(tag, 0, 572);
    memcpy(tag + 8, plain, 8);
    memcpy(tag + 0x80, plain + 8, 0x20);
    memcpy(tag + 0x10, plain + 0x28, 0x24);
    memcpy(tag + 0xa0, plain + 0x4c, 0x168);
    memcpy(tag + 0x34, plain + 0x1b4, 0x20);
    memcpy(tag, plain + 0x1d4, 8);
    memcpy(tag + 0x54, plain + 0x1dc, 0x2c);
}

static void crypto_vectors(void) {
    const uint8_t key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    const uint8_t counter[16] = {0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff};
    uint8_t data[16] = {0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a};
    const uint8_t expected[16] = {0x87,0x4d,0x61,0x91,0xb6,0x20,0xe3,0x26,0x1b,0xef,0x68,0x64,0x99,0x0d,0xb6,0xce};
    assert(cipher(key, counter, data, sizeof data) && !memcmp(data, expected, sizeof data));
    const uint8_t hmac_key[16] = {'J','e','f','e'};
    const uint8_t digest[32] = {0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,
                                0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43};
    uint8_t hash[32];
    const char *message = "what do ya want for nothing?";
    assert(keyed_hash(hmac_key, (const uint8_t *)message, strlen(message), NULL, 0, hash));
    assert(!memcmp(hash, digest, sizeof hash));
}

int main(int argc, char **argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    crypto_vectors();
    MasterKey masters[2];
    uint8_t tag[572];
    fixture(masters, tag);
    WolfLinkDump imported;
    assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_DUMP_MISSING);
    write_file("wolf.bin", tag, 540);
    assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_KEYS_MISSING);
    write_file("keys.bin", masters, sizeof masters);
    const size_t sizes[] = {532, 540, 572};
    for (int index = 0; index < 3; index++) {
        write_file("wolf.bin", tag, sizes[index]);
        assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_DUMP_OK);
        const uint8_t uid[7] = {4, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        assert(!memcmp(imported.uid, uid, sizeof uid));
        assert(imported.flags == (WOLF_REGISTERED | WOLF_HAS_AREA));
        assert(imported.app_id == 0x1019c800 && imported.area[0x12] == 36);
        char nickname[41];
        wolf_link_dump_nickname(&imported, nickname);
        assert(!strcmp(nickname, "Wolf"));
    }
    write_file("wolf.bin", tag, 531);
    assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_DUMP_INVALID);
    tag[0x55] ^= 1;
    write_file("wolf.bin", tag, 540);
    assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_DUMP_NOT_WOLF);
    tag[0x55] ^= 1;
    tag[0xa0] ^= 1;
    write_file("wolf.bin", tag, 540);
    assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_AUTH_FAILED);
    assert(imported.app_id == 0 && imported.uid[0] == 0);
    tag[0xa0] ^= 1;
    write_file("wolf.bin", tag, 540);
    masters[0].hmac_key[0] ^= 1;
    write_file("keys.bin", masters, sizeof masters);
    assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_AUTH_FAILED);
    masters[0].hmac_key[0] ^= 1;
    masters[0].magic_size = 17;
    write_file("keys.bin", masters, sizeof masters);
    assert(wolf_link_dump_read("wolf.bin", "keys.bin", &imported) == WOLF_KEYS_INVALID);
    masters[0].magic_size = 8;
    assert(mkdir("amiibo", 0700) == 0);
    write_file("amiibo/wolf_link.bin", tag, 540);
    write_file("amiibo/key_retail.bin", masters, sizeof masters);
    assert(wolf_link_dump_load(&imported) == WOLF_DUMP_OK);
    imported.area[0x12] = 60;
    assert(wolf_link_dump_save(&imported));
    assert(wolf_link_dump_load(&imported) == WOLF_DUMP_OK && imported.area[0x12] == 60);
    imported.area[0x12] = 40;
    assert(wolf_link_dump_save(&imported));
    assert(wolf_link_dump_load(&imported) == WOLF_DUMP_OK && imported.area[0x12] == 40);
    uint8_t original[540];
    FILE *file = fopen("amiibo/wolf_link.bin", "rb");
    assert(file && fread(original, 1, sizeof original, file) == sizeof original);
    fclose(file);
    assert(!memcmp(original, tag, sizeof original));
    char path[80];
    save_path(&imported, path);
    write_file(path, "bad", 3);
    assert(wolf_link_dump_load(&imported) == WOLF_SAVE_INVALID && !imported.uid[0]);
    assert(remove(path) == 0);
    assert(wolf_link_dump_load(&imported) == WOLF_DUMP_OK && imported.area[0x12] == 36);
    memset(imported.nickname, 0, sizeof imported.nickname);
    imported.nickname[0] = 'A'; imported.nickname[1] = 0xd83d; imported.nickname[2] = 0xdc3a;
    char nickname[41];
    wolf_link_dump_nickname(&imported, nickname);
    assert(!strcmp(nickname, "A\xf0\x9f\x90\xba"));
    puts("PASS: own Wolf Link crypto, authenticated dump parsing, identity, nickname and private sidecar persistence");
}
