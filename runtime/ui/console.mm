// The in-game console (runtime/ui/console.h).
#include "console.h"

#include <pthread.h>
#include <stdio.h>
#include <math.h>
#include <atomic>
#include <string>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../console_core.h"
#include "../control.h"
}

static std::atomic<bool> g_open{false};
static std::atomic<bool> g_appearing{false};  // just opened: the field starts empty, the lines scroll to the end
static pthread_mutex_t g_copy_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_copy;  // a command's text for the clipboard, until the main thread takes it

bool console_is_open(void) { return g_open.load(); }
void console_set_open(bool open) {
    if (open && !g_open.load()) g_appearing.store(true);
    g_open.store(open);
}

bool console_take_copy(char **text) {
    pthread_mutex_lock(&g_copy_lock);
    *text = g_copy;
    g_copy = NULL;
    pthread_mutex_unlock(&g_copy_lock);
    return *text != NULL;
}

// (test) SAVAGEWIND_CONSOLE_TEST="help;version;report": opens the console and runs these commands, one every half
// second from about 30 s in, and echoes every line to stderr as "[console] ...", so a run without hands can be read.
static void test_hook(void) {
    static const char *list = getenv("SAVAGEWIND_CONSOLE_TEST");
    static int tick, next;
    if (!list || !*list) return;
    tick++;
    if (tick < 1800 || (tick - 1800) % 30) return;
    if (!console_is_open()) { console_set_open(true); return; }
    const char *c = list;
    for (int i = 0; i < next && c; i++) { c = strchr(c, ';'); if (c) c++; }
    if (!c || !*c) return;
    const char *end = strchr(c, ';');
    char line[256];
    snprintf(line, sizeof line, "%.*s", end ? (int)(end - c) : (int)strlen(c), c);
    next++;
    console_print(CON_INPUT, line);
    fprintf(stderr, "[console] > %s\n", line);
    if (!control_console_submit(line)) fprintf(stderr, "[console] not accepted: %s\n", line);
}

void console_collect(void) {
    test_hook();
    ControlConsoleResult r;
    while (control_console_poll(&r)) {
        if (getenv("SAVAGEWIND_CONSOLE_TEST")) fprintf(stderr, "[console] %s%s\n", r.failed ? "(failed) " : "", r.text ? r.text : "");
        if (r.text && *r.text) console_print(r.failed ? CON_ERROR : CON_TEXT, r.text);
        else if (r.failed) console_print(CON_ERROR, "Failed");
        if (r.copy) {
            console_print(CON_NOTE, "Copied to the clipboard");
            pthread_mutex_lock(&g_copy_lock);
            free(g_copy);
            g_copy = r.copy;
            r.copy = NULL;
            pthread_mutex_unlock(&g_copy_lock);
        }
        control_console_free(&r);
    }
}

static int input_callback(ImGuiInputTextCallbackData *d) {
    auto replace = [&](const char *text) { d->DeleteChars(0, d->BufTextLen); d->InsertChars(0, text); };
    if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const char *t = console_history_step(d->EventKey == ImGuiKey_UpArrow ? -1 : 1, d->Buf);
        if (t) replace(t);
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        const char *names[128];
        int total = control_console_names("", names, 128), n = total < 128 ? total : 128;
        char out[256];
        int matches = console_complete(d->Buf, names, n, out, sizeof out);
        if (strcmp(out, d->Buf)) replace(out);
        else if (matches > 1) {  // nothing more to add: list what fits
            std::string list;
            size_t len = strlen(d->Buf);
            for (int i = 0; i < n; i++)
                if (!strncmp(names[i], d->Buf, len)) list += std::string(list.empty() ? "" : "  ") + names[i];
            console_print(CON_NOTE, list.c_str());
        }
    }
    return 0;
}

static void submit(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    char shown[320];
    snprintf(shown, sizeof shown, "> %s", line);
    console_print(CON_INPUT, shown);
    console_history_add(line);
    if (!strcmp(line, "clear") || !strcmp(line, "cls")) { console_clear(); return; }
    if (!control_console_submit(line)) console_print(CON_ERROR, "Command too long or queue full");
}

void console_draw(ImGuiIO &io, float scale, ImFont *mono) {
    static char buf[256];
    static unsigned seen;
    bool appearing = g_appearing.exchange(false);
    if (appearing) {
        buf[0] = 0;
        console_history_rewind();
        if (!console_line_count()) console_print(CON_NOTE, "Type help for the commands. Up/Down: history. Tab: complete. Ctrl+V: paste. clear: empty the console. ` or Esc: close.");
    }
    float w = io.DisplaySize.x, h = fminf(io.DisplaySize.y * 0.5f, 760 * scale);
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##console", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (mono) ImGui::PushFont(mono);
    float input_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##lines", ImVec2(0, -input_h), false, ImGuiWindowFlags_NavFlattened);
    ImGui::PushTextWrapPos(0);
    for (int i = 0, n = console_line_count(); i < n; i++) {
        ConKind kind;
        const char *t = console_line(i, &kind);
        ImVec4 c = kind == CON_INPUT ? ImVec4(0.93f, 0.76f, 0.33f, 1) : kind == CON_ERROR ? ImVec4(1.0f, 0.45f, 0.40f, 1)
                 : kind == CON_NOTE ? st.Colors[ImGuiCol_TextDisabled] : st.Colors[ImGuiCol_Text];
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextUnformatted(*t ? t : " ");
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (console_revision() != seen || appearing) { ImGui::SetScrollHereY(1.0f); seen = console_revision(); }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-1);
    ImGui::SetKeyboardFocusHere();
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                ImGuiInputTextFlags_CallbackCompletion;
    if (ImGui::InputText("##input", buf, sizeof buf, flags, input_callback)) {
        submit(buf);
        buf[0] = 0;
    }
    if (mono) ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
}
