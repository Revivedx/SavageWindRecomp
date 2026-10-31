// Tests for the in-game console's lines, history and completion (runtime/console_core.c).
#include <stdio.h>
#include <string.h>

#include "../runtime/console_core.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got); checks++; if (!g_ || strcmp(g_, (want))) { failures++; printf("FAIL line %d: got \"%s\" want \"%s\"\n", __LINE__, g_ ? g_ : "(null)", (want)); } } while (0)

int main(void) {
    ConKind k;
    // ---- lines
    CHECK(console_line_count() == 0 && !console_line(0, &k));
    unsigned rev = console_revision();
    console_print(CON_INPUT, "pos");
    CHECK(console_revision() != rev);
    console_print(CON_TEXT, "a\nb\r\n\nlast");
    CHECK(console_line_count() == 5);
    CHECK_STR(console_line(1, &k), "a"); CHECK(k == CON_TEXT);
    CHECK_STR(console_line(2, NULL), "b");        // \r\n
    CHECK_STR(console_line(3, NULL), "");         // empty lines stay
    CHECK_STR(console_line(4, NULL), "last");
    CHECK(!console_line(5, NULL) && !console_line(-1, NULL));
    for (int i = 0; i < CON_LINES + 50; i++) { char t[16]; snprintf(t, sizeof t, "n%d", i); console_print(CON_NOTE, t); }
    CHECK(console_line_count() == CON_LINES);
    CHECK_STR(console_line(CON_LINES - 1, NULL), "n449");   // the newest stay
    CHECK_STR(console_line(0, NULL), "n50");
    console_clear();
    CHECK(console_line_count() == 0);

    // ---- history
    CHECK(console_history_step(-1, "x") == NULL);           // nothing yet
    console_history_add("pos");
    console_history_add("pos");                             // a repeat of the last is not stored twice
    console_history_add("");
    console_history_add("hour 12");
    console_history_add("goto 1 2 3");
    CHECK_STR(console_history_step(-1, "typing"), "goto 1 2 3");
    CHECK_STR(console_history_step(-1, "ignored"), "hour 12");
    CHECK_STR(console_history_step(-1, "ignored"), "pos");
    CHECK_STR(console_history_step(-1, "ignored"), "pos");  // stays on the oldest
    CHECK_STR(console_history_step(1, ""), "hour 12");
    CHECK_STR(console_history_step(1, ""), "goto 1 2 3");
    CHECK_STR(console_history_step(1, ""), "typing");       // down past the newest brings the half typed line back
    CHECK(console_history_step(1, "typing") == NULL);       // and no further
    console_history_step(-1, "again");
    console_history_rewind();
    CHECK_STR(console_history_step(-1, "new"), "goto 1 2 3");   // a rewound walk starts at the newest again
    console_history_add("report");                          // sending ends a walk
    CHECK_STR(console_history_step(-1, ""), "report");
    for (int i = 0; i < CON_HISTORY + 10; i++) { char t[16]; snprintf(t, sizeof t, "c%d", i); console_history_add(t); }
    for (int i = 0; i < CON_HISTORY + 5; i++) console_history_step(-1, "");
    CHECK_STR(console_history_step(-1, ""), "c10");         // only the last CON_HISTORY stay

    // ---- completion
    const char *names[] = {"camera", "cell", "echo", "exit", "fly", "frames", "get", "goto"};
    char out[64];
    CHECK(console_complete("ca", names, 8, out, sizeof out) == 1);  CHECK_STR(out, "camera ");
    CHECK(console_complete("c", names, 8, out, sizeof out) == 2);   CHECK_STR(out, "c");        // camera, cell: nothing more in common
    CHECK(console_complete("e", names, 8, out, sizeof out) == 2);   CHECK_STR(out, "e");
    CHECK(console_complete("ex", names, 8, out, sizeof out) == 1);  CHECK_STR(out, "exit ");
    CHECK(console_complete("f", names, 8, out, sizeof out) == 2);   CHECK_STR(out, "f");
    CHECK(console_complete("fr", names, 8, out, sizeof out) == 1);  CHECK_STR(out, "frames ");
    CHECK(console_complete("", names, 8, out, sizeof out) == 8);    CHECK_STR(out, "");
    CHECK(console_complete("zz", names, 8, out, sizeof out) == 0);  CHECK_STR(out, "zz");
    CHECK(console_complete("goto 1", names, 8, out, sizeof out) == 0); CHECK_STR(out, "goto 1");   // only the first word
    const char *sib[] = {"teleport", "teleport2", "telemetry"};
    CHECK(console_complete("t", sib, 3, out, sizeof out) == 3);     CHECK_STR(out, "tele");        // the common start
    CHECK(console_complete("tele", sib, 3, out, sizeof out) == 3);  CHECK_STR(out, "tele");
    CHECK(console_complete("telep", sib, 3, out, sizeof out) == 2); CHECK_STR(out, "teleport");

    printf("console core: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
