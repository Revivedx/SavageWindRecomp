// #271: the UI's UTF-8 name must be returned in the encoding requested by ShowKeyboardArg.
// Synthetic guest buffers and a replacement prompt exercise the actual HLE without a game or window.
#include <assert.h>
#include <uchar.h>
#include "../runtime/hle/swkbd.c"

static const char *answer;
static int checks;

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    assert(initial[0] == 0);
    assert(max_len == 9);
    snprintf(out, (size_t)cap, "%s", answer);
    return 1;
}

static size_t u16bytes(const char16_t *s) {
    size_t n = 0;
    while (s[n]) n++;
    return (n + 1) * sizeof *s;
}

static void check(const char *input, int utf8, size_t cap, const void *expected, size_t bytes) {
    uint64_t arg[0x550 / 8] = {0};
    struct { unsigned char before[8], data[64], after[8]; } buf;
    memset(&buf, 0xa5, sizeof buf);
    assert(cap <= sizeof buf.data && bytes <= cap);
    uint64_t result[2] = {(uint64_t)(uintptr_t)buf.data, cap};
    Cpu cpu = {0};
    cpu.x[0] = (uint64_t)(uintptr_t)result;
    cpu.x[1] = (uint64_t)(uintptr_t)arg;
    ST32(cpu.x[1] + 0x3ac, 9);
    ST8(cpu.x[1] + 0x3bd, utf8);
    answer = input;
    swkbd_show(&cpu);
    assert(cpu.x[0] == 0);
    if (bytes && memcmp(buf.data, expected, bytes)) {
        fprintf(stderr, "wrong name bytes: input [%s], UTF-%s, capacity %zu\n", input, utf8 ? "8" : "16LE", cap);
        abort();
    }
    for (size_t i = bytes; i < cap; i++) assert(buf.data[i] == 0);
    for (size_t i = cap; i < sizeof buf.data; i++) assert(buf.data[i] == 0xa5);
    for (size_t i = 0; i < sizeof buf.before; i++) {
        assert(buf.before[i] == 0xa5);
        assert(buf.after[i] == 0xa5);
    }
    checks++;
}

int main(void) {
    // C's UTF-16 literals supply the independent expected bytes, including surrogate pairs.
    static const struct { const char *text; const char16_t *wide; } names[] = {
        {"Talon", u"Talon"}, {"Nintendo", u"Nintendo"}, {"AA", u"AA"}, {"A", u"A"}, {"", u""},
        {"Stormwind", u"Stormwind"}, {"\xc3\x89pona", u"\u00c9pona"},
        {"\xe9\xa9\xac", u"\u9a6c"}, {"A\xf0\x9f\x90\x8e" "B", u"A\U0001f40eB"},
        {"\x7f\xc2\x80", u"\x007f\x0080"}, {"\xdf\xbf\xe0\xa0\x80", u"\u07ff\u0800"},
        {"\xed\x9f\xbf\xee\x80\x80", u"\ud7ff\ue000"},
        {"\xef\xbf\xbf\xf0\x90\x80\x80", u"\uffff\U00010000"},
        {"\xf4\x8f\xbf\xbf", u"\U0010ffff"},
    };
    for (int repeat = 0; repeat < 2; repeat++)
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            check(names[i].text, 0, 64, names[i].wide, u16bytes(names[i].wide));
            check(names[i].text, 1, 64, names[i].text, strlen(names[i].text) + 1);
        }

    // Byte capacities include the terminator. Odd capacities must not split a UTF-16 code unit.
    check("Talon", 0, 12, u"Talon", 12);
    check("Talon", 0, 11, u"Talo", 10);
    check("Talon", 0, 9, u"Tal", 8);
    check("Talon", 1, 6, "Talon", 6);
    check("Talon", 1, 5, "Talo", 5);
    check("A\xf0\x9f\x90\x8e" "B", 0, 10, u"A\U0001f40eB", 10);
    check("A\xf0\x9f\x90\x8e" "B", 0, 8, u"A\U0001f40e", 8);
    check("A\xf0\x9f\x90\x8e" "B", 0, 7, u"A", 4);
    check("A\xf0\x9f\x90\x8e" "B", 1, 6, "A\xf0\x9f\x90\x8e", 6);
    check("A\xf0\x9f\x90\x8e" "B", 1, 5, "A", 2);
    check("\xc3\x89pona", 1, 2, "", 1);
    check("\xc3\x89pona", 1, 3, "\xc3\x89", 3);
    for (int utf8 = 0; utf8 <= 1; utf8++) {
        check("Talon", utf8, 0, NULL, 0);
        check("Talon", utf8, 1, "", 1);
        check("Talon", utf8, 2, utf8 ? (const void *)"T" : (const void *)u"", 2);
    }

    // Malformed input cannot introduce unpaired surrogates or invalid UTF-8 into the guest result.
    check("\xff", 0, 64, u"\ufffd", 4);
    check("\xff", 1, 64, "\xef\xbf\xbd", 4);
    check("\xe2\x82", 0, 64, u"\ufffd\ufffd", 6);
    check("\xed\xa0\x80", 0, 64, u"\ufffd\ufffd\ufffd", 8);
    check("\xf4\x90\x80\x80", 0, 64, u"\ufffd\ufffd\ufffd\ufffd", 10);
    check("\xc0\xaf", 0, 64, u"\ufffd\ufffd", 6);
    printf("swkbd: %d output-encoding checks passed\n", checks);
    return 0;
}
