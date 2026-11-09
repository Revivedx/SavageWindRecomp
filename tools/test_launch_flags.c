// Tests for launch options (runtime/launch_flags.c): checking, the file, the environment, suggestions, and the
// generated table (no game files, no window).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../runtime/launch_flags.h"
#ifdef _WIN32
#include "../runtime/platform/win_compat.h"  // (setenv, unsetenv)
#endif

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got); checks++; if (!g_ || strcmp(g_, (want))) { failures++; printf("FAIL line %d: got \"%s\" want \"%s\"\n", __LINE__, g_ ? g_ : "(null)", (want)); } } while (0)

static const char *first_of(LfKind kind, LfGroup group) {
    for (int i = 0; i < LF_TABLE_N; i++)
        if (LF_TABLE[i].kind == kind && LF_TABLE[i].group == group) return LF_TABLE[i].name;
    return NULL;
}

int main(void) {
    // ---- the generated table
    CHECK(LF_TABLE_N > 100);
    for (int i = 0; i < LF_TABLE_N; i++) {
        const LfInfo *f = &LF_TABLE[i];
        CHECK(!strncmp(f->name, "SAVAGEWIND_", 11));
        if (i) CHECK(strcmp(LF_TABLE[i - 1].name, f->name) < 0);  // sorted, no duplicates
        CHECK(!strstr(f->desc, "\xE2\x80\x94") && !strstr(f->desc, "\xE2\x80\x93"));  // no dashes in copy
        if (f->group == LF_GROUP_PLAY) CHECK(f->desc[0]);
        CHECK(strlen(f->name) < LF_NAME_MAX && strlen(f->example) < LF_VALUE_MAX);
        // what is suggested can be added (unless it is a switch with an example that turns it off)
        if (f->group <= LF_GROUP_DEV) CHECK(lf_check(f->name, f->example) == LF_OK);
        else CHECK(lf_check(f->name, "1") != LF_OK);
    }
    CHECK(lf_find("SAVAGEWIND_REEXEC") && lf_find("SAVAGEWIND_REEXEC")->group == LF_GROUP_INTERNAL);
    CHECK(lf_find("savagewind_fsr3") == lf_find("SAVAGEWIND_FSR3") && lf_find("SAVAGEWIND_FSR3"));
    CHECK(!lf_find("SAVAGEWIND_NOT_A_THING"));
    CHECK(lf_find("SAVAGEWIND_NO_OCCLUSION_QUERY")->kind == LF_KIND_SWITCH);
    CHECK(lf_find("SAVAGEWIND_GPU_OVERLAP")->kind == LF_KIND_NUMBER);

    // ---- names and values
    CHECK(lf_check("SAVAGEWIND_FSR3", "1") == LF_OK);
    CHECK(lf_check("SAVAGEWIND_MADE_UP_BY_A_DEVELOPER", "1") == LF_OK);  // any custom flag works
    CHECK(lf_check("MESA_DEBUG", "1") == LF_OK);
    CHECK(lf_check("", "1") == LF_ERR_EMPTY);
    CHECK(lf_check("1ABC", "1") == LF_ERR_BAD_NAME);
    CHECK(lf_check("A B", "1") == LF_ERR_BAD_NAME);
    CHECK(lf_check("A=B", "1") == LF_ERR_BAD_NAME);
    CHECK(lf_check("A-B", "1") == LF_ERR_BAD_NAME);
    char longname[200];
    memset(longname, 'A', sizeof longname), longname[199] = 0;
    CHECK(lf_check(longname, "1") == LF_ERR_NAME_LONG);
    CHECK(lf_check("SAVAGEWIND_X1", "a\nb") == LF_ERR_BAD_VALUE);
    CHECK(lf_check("SAVAGEWIND_X1", "a\tb") == LF_ERR_BAD_VALUE);
    char longval[300];
    memset(longval, '1', sizeof longval), longval[299] = 0;
    CHECK(lf_check("SAVAGEWIND_X1", longval) == LF_ERR_VALUE_LONG);
    // the ones that would lock a player out, or break the start
    CHECK(lf_check("SAVAGEWIND_NO_LAUNCHER", "1") == LF_ERR_LAUNCHER);
    CHECK(lf_check("savagewind_no_launcher", "1") == LF_ERR_LAUNCHER);
    CHECK(lf_check("SAVAGEWIND_INPUT", "x") == LF_ERR_LAUNCHER);
    CHECK(lf_check("SAVAGEWIND_HIDDEN", "1") == LF_ERR_LAUNCHER);
    CHECK(lf_check("SAVAGEWIND_LAUNCHER_SOMETHING_NEW", "1") == LF_ERR_LAUNCHER);
    CHECK(lf_check("SAVAGEWIND_REEXEC", "1") == LF_ERR_INTERNAL);
    CHECK(lf_check("SAVAGEWIND_DATA_ROOT", "/x") == LF_ERR_INTERNAL);
    CHECK(lf_check("SAVAGEWIND_SETTINGS", "/x") == LF_ERR_INTERNAL);
    CHECK(lf_check(LF_APPLIED_ENV, "x") == LF_ERR_INTERNAL);
    CHECK(lf_check("SAVAGEWIND_GEN", "x") == LF_ERR_BUILD);
    CHECK(lf_check("PATH", "x") == LF_ERR_SYSTEM && lf_check("path", "x") == LF_ERR_SYSTEM);
    CHECK(lf_check("LD_PRELOAD", "x") == LF_ERR_SYSTEM && lf_check("DYLD_INSERT_LIBRARIES", "x") == LF_ERR_SYSTEM);
    CHECK(lf_check("XDG_DATA_HOME", "/x") == LF_ERR_SYSTEM);
    CHECK(lf_check("WAYLAND_DISPLAY", "x") == LF_ERR_SYSTEM);
    // a switch is on or removed, never 0
    const char *sw = first_of(LF_KIND_SWITCH, LF_GROUP_OTHER);
    CHECK(sw && lf_check(sw, "0") == LF_ERR_SWITCH_OFF && lf_check(sw, "off") == LF_ERR_SWITCH_OFF);
    CHECK(sw && lf_check(sw, "1") == LF_OK && lf_check(sw, "") == LF_OK);
    CHECK(lf_check("SAVAGEWIND_GPU_OVERLAP", "0") == LF_OK);  // a number: 0 is a real value
    CHECK(lf_value_is_off("0") && lf_value_is_off("Off") && lf_value_is_off("FALSE") && !lf_value_is_off("1") && !lf_value_is_off(""));

    // ---- the list
    static LfList l;
    CHECK(l.n == 0);
    CHECK(lf_set(&l, "  savagewind_fsr3 ", "  ") == LF_OK);  // tidied; empty value is 1
    CHECK(l.n == 1 && !strcmp(l.flag[0].name, "SAVAGEWIND_FSR3") && !strcmp(l.flag[0].value, "1"));
    CHECK(lf_set(&l, "MY_CUSTOM_flag", " a b ") == LF_OK);  // a custom name keeps its case; the value's inside spaces stay
    CHECK(l.n == 2 && !strcmp(l.flag[1].name, "MY_CUSTOM_flag") && !strcmp(l.flag[1].value, "a b"));
    CHECK(lf_set(&l, "SAVAGEWIND_FSR3", "2") == LF_OK && l.n == 2 && !strcmp(l.flag[0].value, "2"));  // edit in place
    CHECK(lf_index(&l, "savagewind_fsr3") == 0 && lf_index(&l, "NOPE") == -1);
    CHECK(lf_set(&l, "SAVAGEWIND_REEXEC", "1") == LF_ERR_INTERNAL && l.n == 2);
    // short names mean the listed SAVAGEWIND_ flag (and are checked as it)
    CHECK(lf_set(&l, "no_launcher", "1") == LF_ERR_LAUNCHER && l.n == 2);
    CHECK(lf_set(&l, "REEXEC", "1") == LF_ERR_INTERNAL && l.n == 2);
    CHECK(lf_set(&l, "fps_cap", "30") == LF_OK && l.n == 3 && !strcmp(l.flag[2].name, "SAVAGEWIND_FPS_CAP"));
    CHECK(lf_set(&l, "FPS_CAP", "45") == LF_OK && l.n == 3 && !strcmp(l.flag[2].value, "45"));  // same entry
    lf_remove(&l, 2);
    lf_remove(&l, 0);
    CHECK(l.n == 1 && !strcmp(l.flag[0].name, "MY_CUSTOM_flag"));
    lf_remove(&l, 5), lf_remove(&l, -1);
    CHECK(l.n == 1);
    lf_clear(&l);
    for (int i = 0; i < LF_MAX_FLAGS; i++) { char n[32]; snprintf(n, sizeof n, "SAVAGEWIND_T%d", i); CHECK(lf_set(&l, n, "1") == LF_OK); }
    CHECK(lf_set(&l, "SAVAGEWIND_ONE_TOO_MANY", "1") == LF_ERR_FULL);
    CHECK(lf_set(&l, "SAVAGEWIND_T3", "5") == LF_OK);  // editing a full list still works
    lf_clear(&l);

    // ---- the file
    LfList p;
    int bad = lf_parse(&p, "\xEF\xBB\xBF# a comment\r\n\r\nSAVAGEWIND_FSR3\r\nSAVAGEWIND_GPU_OVERLAP = 0 \nFOO=a=b\nSAVAGEWIND_NO_LAUNCHER=1\n"
                           "1BAD=2\nSAVAGEWIND_FSR3=7\n   # indented comment\nBARE_NAME_WITHOUT_END");
    CHECK(bad == 2);  // NO_LAUNCHER and 1BAD
    CHECK(p.n == 4);
    CHECK(!strcmp(p.flag[0].name, "SAVAGEWIND_FSR3") && !strcmp(p.flag[0].value, "7"));  // the later line replaced it
    CHECK(!strcmp(p.flag[1].name, "SAVAGEWIND_GPU_OVERLAP") && !strcmp(p.flag[1].value, "0"));
    CHECK(!strcmp(p.flag[2].name, "FOO") && !strcmp(p.flag[2].value, "a=b"));
    CHECK(!strcmp(p.flag[3].name, "BARE_NAME_WITHOUT_END") && !strcmp(p.flag[3].value, "1"));
    CHECK(lf_parse(&p, "") == 0 && p.n == 0);
    CHECK(lf_parse(&p, "\n\n# only comments\n") == 0 && p.n == 0);

    char text[4096];
    LfList q;
    lf_set(&q, "SAVAGEWIND_FSR3", "1");
    lf_set(&q, "FOO", "x y");
    size_t len = lf_format(&q, text, sizeof text);
    CHECK(len > 0 && len == strlen(text) && strstr(text, "SAVAGEWIND_FSR3=1\nFOO=x y\n"));
    CHECK(text[0] == '#' && !strstr(text, "\xE2\x80\x94"));
    CHECK(lf_format(&q, text, 10) == 0);  // too small
    LfList r;
    lf_format(&q, text, sizeof text);
    CHECK(lf_parse(&r, text) == 0 && r.n == 2 && !strcmp(r.flag[1].value, "x y"));

    char path[200];
#ifdef _WIN32
    snprintf(path, sizeof path, "%s/sw_lf_test_%d.txt", getenv("TEMP") ? getenv("TEMP") : ".", (int)getpid());
#else
    snprintf(path, sizeof path, "/tmp/sw_lf_test_%d.txt", (int)getpid());
#endif
    CHECK(!lf_load(&r, path) && r.n == 0);  // no file = empty
    CHECK(lf_save(&q, path));
    CHECK(lf_load(&r, path) && r.n == 2 && !strcmp(r.flag[0].name, "SAVAGEWIND_FSR3"));
    lf_set(&q, "FOO", "new");
    CHECK(lf_save(&q, path) && lf_load(&r, path) && !strcmp(r.flag[1].value, "new"));  // replaced
    LfList empty = {.n = 0};
    CHECK(lf_save(&empty, path) && !lf_load(&r, path) && access(path, F_OK) != 0);  // nothing saved = no file
    CHECK(lf_save(&empty, path));  // and again with no file
    CHECK(!lf_save(&q, "/nonexistent-folder/x.txt"));

    // ---- the environment
    unsetenv(LF_APPLIED_ENV), unsetenv("SAVAGEWIND_FSR3"), unsetenv("FOO"), unsetenv("SAVAGEWIND_NO_OCCLUSION_QUERY");
    CHECK_STR(lf_applied(), "");
    CHECK(lf_in_effect(&empty));  // nothing saved, nothing applied
    LfList a;
    lf_set(&a, "SAVAGEWIND_FSR3", "1");
    lf_set(&a, "FOO", "x y");
    CHECK(!lf_in_effect(&a));
    CHECK(lf_apply(&a) == 2);
    CHECK_STR(getenv("SAVAGEWIND_FSR3"), "1");
    CHECK_STR(getenv("FOO"), "x y");
    CHECK(lf_in_effect(&a) && strchr(lf_applied(), ':'));
    LfList reordered;
    lf_set(&reordered, "FOO", "x y");
    lf_set(&reordered, "SAVAGEWIND_FSR3", "1");
    CHECK(lf_in_effect(&reordered));  // the order of the file doesn't matter
    lf_set(&reordered, "FOO", "other");
    CHECK(!lf_in_effect(&reordered));
    // removing a flag takes it out of the environment
    LfList b;
    lf_set(&b, "SAVAGEWIND_FSR3", "1");
    CHECK(lf_apply(&b) == 1 && getenv("SAVAGEWIND_FSR3") && !getenv("FOO"));
    CHECK(lf_in_effect(&b) && !lf_in_effect(&a));
    CHECK(lf_apply(&empty) == 0 && !getenv("SAVAGEWIND_FSR3") && !getenv(LF_APPLIED_ENV) && lf_in_effect(&empty));
    // a hand-built list holding a switch with an off value: skipped, not set to 0 (presence would turn it on)
    LfList sw_off = {.n = 1};
    snprintf(sw_off.flag[0].name, sizeof sw_off.flag[0].name, "SAVAGEWIND_NO_OCCLUSION_QUERY");
    snprintf(sw_off.flag[0].value, sizeof sw_off.flag[0].value, "0");
    CHECK(lf_skipped(&sw_off.flag[0]));
    CHECK(lf_apply(&sw_off) == 0 && !getenv("SAVAGEWIND_NO_OCCLUSION_QUERY") && lf_in_effect(&sw_off) && lf_in_effect(&empty));
    snprintf(sw_off.flag[0].value, sizeof sw_off.flag[0].value, "1");
    CHECK(!lf_skipped(&sw_off.flag[0]) && lf_apply(&sw_off) == 1 && getenv("SAVAGEWIND_NO_OCCLUSION_QUERY"));
    lf_apply(&empty);
    CHECK(!getenv("SAVAGEWIND_NO_OCCLUSION_QUERY"));

    // ---- suggestions
    const LfInfo *s[8];
    int n = lf_suggest("", s, 8);
    CHECK(n == 8 && s[0]->group == LF_GROUP_PLAY && s[7]->group == LF_GROUP_PLAY);  // nothing typed: what testers are asked for
    n = lf_suggest("fsr3", s, 8);
    CHECK(n >= 1 && !strcmp(s[0]->name, "SAVAGEWIND_FSR3"));  // the shorter, exact match first
    n = lf_suggest("SAVAGEWIND_VK_NO_F", s, 8);
    CHECK(n >= 1 && !strcmp(s[0]->name, "SAVAGEWIND_VK_NO_FEEDBACK_LOOP"));
    n = lf_suggest("feedback", s, 8);  // in the middle of a name
    CHECK(n >= 2);
    bool all_have = true;
    for (int i = 0; i < n; i++) all_have &= strstr(s[i]->name, "FEEDBACK") != NULL;
    CHECK(all_have);
    for (int i = 1; i < n; i++) CHECK(s[i - 1]->group <= s[i]->group || strncmp(s[i - 1]->name, s[i]->name, 1) == 0);
    n = lf_suggest("no_launcher", s, 8);  // never offered
    for (int i = 0; i < n; i++) CHECK(strcmp(s[i]->name, "SAVAGEWIND_NO_LAUNCHER") && strcmp(s[i]->name, "SAVAGEWIND_REEXEC"));
    n = lf_suggest("reexec", s, 8);
    CHECK(n == 0);
    n = lf_suggest("zzzzqq", s, 8);
    CHECK(n == 0);
    n = lf_suggest("savage", s, 3);
    CHECK(n == 3 && s[0]->group == LF_GROUP_PLAY);  // play flags before the rest
    n = lf_suggest("SAVAGEWIND_", s, 8);
    for (int i = 0; i < n; i++) CHECK(s[i]->group <= LF_GROUP_DEV);
    const char *dev = first_of(LF_KIND_SWITCH, LF_GROUP_DEV);
    CHECK(dev);  // developer flags are offered too (with a warning in the screen)
    n = lf_suggest(dev, s, 4);
    CHECK(n >= 1 && !strcmp(s[0]->name, dev));
    CHECK(lf_suggest("fsr", NULL, 0) == 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
