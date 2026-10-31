// Tests for the command layer behind the control endpoint and the in-game console (runtime/control.c): who may run
// what, the console's queue and its text, the table help prints, completion. No game needed; run it twice:
//   test_control                    a normal player: only the player safe commands exist for the console
//   SAVAGEWIND_DEV=1 test_control dev   developer commands too (make test-control runs both)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/control.h"

// ---- stubs for what control.c takes from the rest of the runtime
uint64_t plat_now_ns(void) { return 1; }
void rt_fatal_trace(void) { abort(); }
void control_game_frame(uint64_t frame) { (void)frame; }

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { checks++; if (strcmp((got), (want))) { failures++; printf("FAIL line %d: got \"%s\" want \"%s\"\n", __LINE__, (got), (want)); } } while (0)

static void cmd_place(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    control_field_num(r, "x", 0.1);               // 0.10000000000000001 in the JSON
    control_field_num(r, "y", 1234.5678912345);
    control_field_int(r, "frame", 3000000000ll);
    control_field_bool(r, "held", true);
    control_field_str(r, "name", "quote \" and \\ and \n newline");
    control_copy(r, "swr:1 x=1.00 y=2.00 z=3.00");
}
static void cmd_lines(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    control_field_int(r, "hidden", 1);
    control_text(r, "first line");
    control_text(r, "second %d", 2);
}
static void cmd_echo(int argc, char **argv, ControlReply *r) {
    for (int i = 1; i < argc; i++) { char k[16]; snprintf(k, sizeof k, "a%d", i); control_field_str(r, k, argv[i]); }
    control_field_int(r, "argc", argc);
}
static void cmd_boom(int argc, char **argv, ControlReply *r) { (void)argc; (void)argv; control_fail(r, "boom: it failed"); }
static void cmd_secret(int argc, char **argv, ControlReply *r) { (void)argc; (void)argv; control_field_bool(r, "ran", true); }
static void cmd_nested(int argc, char **argv, ControlReply *r) {
    (void)argc; (void)argv;
    control_field_int(r, "n", 2);
}

void control_game_init(void) {
    static const ControlCmd cmds[] = {
        {"place", "", "a place", cmd_place, CONTROL_PLAYER},
        {"lines", "", "text lines", cmd_lines, CONTROL_PLAYER},
        {"echo", "<words...>", "echo the arguments", cmd_echo, CONTROL_PLAYER},
        {"boom", "", "always fails", cmd_boom, CONTROL_PLAYER},
        {"nested", "", "a number", cmd_nested, CONTROL_PLAYER},
        {"secret", "<x> <y> <z>", "developer only", cmd_secret, 0},
        {"sneaky", "", "developer only, no flags written", cmd_secret},
    };
    control_register(cmds, (int)(sizeof cmds / sizeof *cmds));
}

static ControlConsoleResult run(const char *line) {
    ControlConsoleResult r;
    control_exec_line(line, true, &r);
    return r;
}
static void expect(const char *line, bool failed, const char *text) {
    ControlConsoleResult r = run(line);
    if (r.failed != failed || strcmp(r.text ? r.text : "", text)) {
        failures++;
        printf("FAIL \"%s\": failed=%d text=\"%s\"\n   want failed=%d text=\"%s\"\n", line, r.failed, r.text ? r.text : "(null)", failed, text);
    }
    checks++;
    control_console_free(&r);
}

int main(int argc, char **argv) {
    bool dev = argc > 1 && !strcmp(argv[1], "dev");
    // (the environment says which mode: the Makefile sets SAVAGEWIND_DEV=1 for the dev run; no setenv, Windows has none)
    if (getenv("SAVAGEWIND_CONTROL") || (getenv("SAVAGEWIND_DEV") != NULL) != dev) {
        printf("run it through make test-control (SAVAGEWIND_DEV=1 and the argument dev go together, SAVAGEWIND_CONTROL unset)\n");
        return 2;
    }
    control_init();
    CHECK(control_enabled() == dev);
    CHECK(!control_endpoint_on());

    // ---- text of a reply: fields as key: value, numbers shortened, strings unescaped
    expect("place", false, "x: 0.1\ny: 1234.568\nframe: 3000000000\nheld: true\nname: quote \" and \\ and \n newline");
    {
        ControlConsoleResult r = run("place");
        CHECK(r.copy && !strcmp(r.copy, "swr:1 x=1.00 y=2.00 z=3.00"));  // for the clipboard
        control_console_free(&r);
    }
    expect("lines", false, "first line\nsecond 2");           // a command's own lines replace the field listing
    expect("boom", true, "boom: it failed");
    expect("nested", false, "n: 2");
    expect("echo", false, "argc: 1");
    expect("echo a \"b c\" d", false, "a1: a\na2: b c\na3: d\nargc: 4");
    expect("echo \"q\\\"uote\" \"back\\\\slash\"", false, "a1: q\"uote\na2: back\\slash\nargc: 3");
    expect("echo \"open", true, "unterminated double quote");
    expect("   echo    spaced   ", false, "a1: spaced\nargc: 2");

    // ---- who may run what
    if (dev) {
        expect("secret 1 2 3", false, "ran: true");
        expect("sneaky", false, "ran: true");
    } else {
        expect("secret 1 2 3", true, "Unknown command: secret");
        expect("sneaky", true, "Unknown command: sneaky");   // no flags written: developer only, never by accident
        expect("ping", true, "Unknown command: ping");
    }
    expect("nonsense", true, "Unknown command: nonsense");
    {   // the socket side (not the console) always has the full set, and the same word reaches the same handler
        ControlConsoleResult r;
        control_exec_line("secret 1 2 3", false, &r);
        CHECK(!r.failed && r.text && !strcmp(r.text, "ran: true"));
        control_console_free(&r);
    }

    // ---- help: sorted table, only what the console may run
    {
        ControlConsoleResult r = run("help");
        CHECK(!r.failed && r.text);
        CHECK(strstr(r.text, "place") != NULL);
        CHECK(strstr(r.text, "echo <words...>") != NULL);
        CHECK(strstr(r.text, "secret") == NULL ? !dev : dev);
        const char *b = strstr(r.text, "boom"), *e = strstr(r.text, "echo"), *h = strstr(r.text, "help");
        CHECK(b && e && h && b < e && e < h);                      // sorted
        control_console_free(&r);
    }

    // ---- completion
    {
        const char *names[16];
        int n = control_console_names("", names, 16);
        CHECK(n == (dev ? 10 : 7));   // help, ping, exit and 7 test commands, or only the player safe ones (help, exit, 5 test ones)
        n = control_console_names("e", names, 16);
        CHECK(n >= 1 && !strcmp(names[0], "echo"));
        n = control_console_names("sec", names, 16);
        CHECK(n == (dev ? 1 : 0));
        n = control_console_names("zzz", names, 16);
        CHECK(n == 0);
        n = control_console_names("", names, 2);
        CHECK(n == (dev ? 10 : 7));   // the count is the full count; only `max` pointers are written
    }

    // ---- the console's queue: lines run in control_frame(), results come back in order
    {
        ControlConsoleResult r;
        CHECK(!control_console_poll(&r));
        CHECK(!control_console_submit("   "));
        CHECK(control_console_submit("echo one"));
        CHECK(control_console_submit("boom"));
        CHECK(control_console_submit("lines"));
        CHECK(!control_console_poll(&r));   // nothing runs until a frame
        control_frame(1);
        CHECK(control_console_poll(&r) && !r.failed && !strcmp(r.text, "a1: one\nargc: 2"));
        control_console_free(&r);
        CHECK(control_console_poll(&r) && r.failed && !strcmp(r.text, "boom: it failed"));
        control_console_free(&r);
        CHECK(control_console_poll(&r) && !r.failed && !strcmp(r.text, "first line\nsecond 2"));
        control_console_free(&r);
        CHECK(!control_console_poll(&r));
        // a full queue says no instead of growing
        int accepted = 0;
        for (int i = 0; i < 100; i++) accepted += control_console_submit("echo x");
        CHECK(accepted == 16);
        control_frame(2);
        int got = 0;
        while (control_console_poll(&r)) { got++; control_console_free(&r); }
        CHECK(got == 16);
        CHECK(control_console_submit("echo again"));   // room again once read
        control_frame(3);
        CHECK(control_console_poll(&r)); control_console_free(&r);
        char huge[2000];
        memset(huge, 'a', sizeof huge - 1); huge[sizeof huge - 1] = 0;
        CHECK(!control_console_submit(huge));
    }

    printf("%s: %d checks, %d failed\n", dev ? "dev" : "player", checks, failures);
    return failures ? 1 : 0;
}
