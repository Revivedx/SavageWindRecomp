// Runs the start screen's Saves tab (runtime/ui/saves_tab.mm) without a window or a renderer: Dear ImGui builds its
// frames and draw lists as usual (its asserts catch an unbalanced ID stack, a popup that isn't closed, ...), the SDL
// folder dialog and the GPU textures are replaced by stand-ins, and a scenario clicks through the tab with synthetic
// keys and a click. tools/test_save_convert.py builds and drives it (skipped with --no-ui) and checks the files afterwards.
//
//   test_saves_tab jpeg <path>                 a small real JPEG (the synthetic saves hold random bytes as pictures)
//   test_saves_tab <scenario> --save <save folder> --game <game files folder> [--src <folder>]
//     scenario: import   pick the folder, click Import in the question, wait for the result
//               cancel   pick the folder, then Escape in the question
//               pick     only pick the folder (a refusal shows as the result)
//               backup   the Back up saves button
//               locked-import, locked-backup   the same, with another "game" holding the save folder's lock
// Output: lines "KEY value" for the script to read (KIND is the Kind enum, CODE an ScCode); exit status 1 when the tab
// itself misbehaved.
#ifdef _WIN32
#include <direct.h>  // _mkdir
#include <stdlib.h>  // _putenv_s
#include <windows.h>
#endif
#include <chrono>
#include <thread>

#include "imgui_internal.h"  // (a window by name)
#include "../runtime/services/save_convert.h"
static ScCode tab_convert(const char *, const char *, const ScFormat *, unsigned, const ScCancel *, ScStage **, ScReport *);
#define sc_convert_cancel tab_convert
#include "../runtime/ui/saves_tab.mm"  // (the whole file: the test reaches its state)
#undef sc_convert_cancel

#include "stb_image_write.h"

// Pause at an actual file boundary, or between the installation renames. Exit must release the worker and join it.
static std::atomic<bool> g_pause_convert{false}, g_pause_install{false}, g_paused{false};
struct Gate { const ScCancel *cancel; ScReport *report; };
static bool gated_cancel(void *p) {
    Gate *g = (Gate *)p;
    if (g_pause_convert && g->report->n_written) {
        g_paused = true;
        while (!g->cancel->requested(g->cancel->data)) std::this_thread::yield();
    }
    return g->cancel->requested(g->cancel->data);
}
static ScCode tab_convert(const char *src, const char *dst, const ScFormat *fmt, unsigned flags,
                          const ScCancel *cancel, ScStage **stage, ScReport *r) {
    Gate gate = {cancel, r};
    ScCancel check = {gated_cancel, &gate};
    return sc_convert_cancel(src, dst, fmt, flags, &check, stage, r);
}
extern "C" void sc_test_before_install(void) {
    if (!g_pause_install) return;
    g_paused = true;
    while (!g_cancel.load()) std::this_thread::yield();
}

// ---------------------------------------------------------------- stand-ins
static int g_created, g_freed;

extern "C" {
SDL_Window *ui_window(void) { return NULL; }
uint64_t vk_imgui_texture_rgba(const void *, int, int) { return 1000 + (uint64_t)++g_created; }
void vk_imgui_texture_free(uint64_t) { g_freed++; }
const char *rt_data_root(void) { return "."; }
bool rt_is_link(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);  // (a junction or symlink is a reparse point)
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}
#ifdef _WIN32
bool SDL_CreateDirectory(const char *p) { return _mkdir(p) == 0 || errno == EEXIST; }
#else
bool SDL_CreateDirectory(const char *p) { return mkdir(p, 0755) == 0 || errno == EEXIST; }
#endif
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback, void *, SDL_Window *, const char *, bool) {}
}
void reveal_folder(const std::string &dir) { printf("REVEAL %s\n", dir.c_str()); }
const char *tr(const char *en) { return en; }
std::string tr_upper(const char *s) {
    std::string r = s;
    for (char &c : r) c = (char)toupper((unsigned char)c);
    return r;
}
bool ui_input_text(const char *, char *, size_t, ImGuiInputTextFlags, const char *, const char *) { return false; }
bool ui_input_text_cancelled(void) { return false; }
bool osk_is_open(void) { return false; }

// ---------------------------------------------------------------- the frames
static ImGuiIO *g_io;
static bool g_picture_drawn;  // some frame drew one of the save pictures

static void frame() {
    g_io->DeltaTime = 1 / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1500, 1000));
    ImGui::Begin("settings", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    saves_tab_draw();
    ImGui::End();
    ImGui::Render();
    for (ImDrawList *l : ImGui::GetDrawData()->CmdLists)
        for (const ImDrawCmd &c : l->CmdBuffer)
            if (c.GetTexID() >= 1000) g_picture_drawn = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
}

static void frames(int n) {
    for (int i = 0; i < n; i++) frame();
}

static bool until(bool (*done)(), int max_frames = 3000) {  // (the worker runs on its own thread)
    for (int i = 0; i < max_frames && !done(); i++) frame();
    return done();
}

static void key(ImGuiKey k) {
    g_io->AddKeyEvent(k, true);
    frame();
    g_io->AddKeyEvent(k, false);
    frame();
}

static void pick(const std::string &folder) {
    const char *list[] = {folder.c_str(), NULL};
    folder_picked(NULL, list, 0);
}

static bool in_confirm() { return g_phase.load() == CONFIRM; }
static bool asks_or_ends() { int p = g_phase.load(); return p == CONFIRM || p == DONE; }
static bool is_done() { return g_phase.load() == DONE; }
static bool is_working() { return g_phase.load() == WORKING; }


static int run(const std::string &scenario, const std::string &src) {
    int bad = 0;
    auto check = [&](bool ok, const char *what) {
        printf("%s %s\n", ok ? "ok" : "BAD", what);
        bad += !ok;
    };
    frame();
    check(until([] { return g_phase.load() != LOADING; }), "the slot list loads on the worker");
    frames(40);  // (the list and its pictures, one a frame)
    printf("SLOTS %zu\n", g_slots.size());
    int shown = 0;
    for (const Thumb &t : g_thumbs) shown += t.tex != 0;
    printf("PICTURES %d\n", shown);
    // the mouse over the rows: a slot's picture comes up in the pane on the right
    for (int y = 40; y < 900 && !g_picture_drawn; y += 14) {
        g_io->AddMousePosEvent(300, (float)y);
        frames(2);
    }
    check(g_slots.empty() || shown == 0 || g_picture_drawn, "a slot's picture is drawn when its row is hovered");
    g_io->AddMousePosEvent(-1000, -1000);
    if (scenario == "recovery") {
        check(g_recovery.imported[0] && g_recovery.backup[0], "the recovery message names both folders");
        check(!saves_tab_busy(), "recovery has no running worker");
        printf("RECOVERY_IMPORT %s\nRECOVERY_BACKUP %s\n", g_recovery.imported, g_recovery.backup);
        return bad;
    }
    if (scenario == "quit-convert") {
        g_pause_convert = true;
        start(JOB_CONVERT, src);
        check(until([] { return g_paused.load(); }), "conversion has written a file and is still working");
        check(is_working(), "exit begins during WORKING");
        exit(bad ? 1 : 0);  // (the launcher's Quit and window close use this same path)
    }
    ScLock *game = NULL;  // (held to the end of the run: the running game of another Savage Wind)
    if (scenario == "locked-import" || scenario == "locked-backup")
        check(sc_lock_take(save_dir().c_str(), &game) == SC_LOCK_OK, "the stand-in game takes the lock");
    if (scenario == "backup" || scenario == "locked-backup") {
        start(JOB_BACKUP);
        check(until(is_done), "the backup finishes");
        frames(10);
        printf("KIND %d\nCODE %d\nFILES %d\nBACKUP %s\n", (int)g_kind, g_code, g_files, g_backup.c_str());
        return bad;
    }
    pick(src);
    frames(2);
    if (!until(asks_or_ends) || !in_confirm()) {
        check(is_done(), "an import that cannot start ends with a result");
        frames(10);
        printf("KIND %d\nCODE %d\nDETAIL %s\n", (int)g_kind, g_code, g_detail.c_str());
        return bad;
    }
    frames(10);  // (the question is up)
    printf("STAGE %s\nBACKUP %s\nHADSAVES %d\n", g_stage_dir.c_str(), g_backup.c_str(), (int)g_had_saves);
    if (scenario == "quit-confirm") exit(bad ? 1 : 0);
    if (scenario == "quit-install") {
        g_pause_install = true;
        start(JOB_INSTALL);
        check(until([] { return g_paused.load(); }), "installation is between the two renames");
        check(is_working(), "exit begins during WORKING");
        drop_stage_at_exit();  // also runs again at exit: joining and cleanup must be idempotent
        check(!g_joinable && !g_stage, "cleanup joins the installer and releases the stage");
        check(g_kind == IMPORTED, "installation finishes before cleanup returns");
        return bad;
    }
    if (scenario == "pick") {
        key(ImGuiKey_Escape);
        until(is_done, 60);
        return bad;
    }
    if (scenario == "cancel") {
        g_last_frame = -100;  // returning to the tab must not clean up the copy waiting for confirmation
        frame();
        struct stat st;
        check(in_confirm() && g_stage && stat(g_stage_dir.c_str(), &st) == 0, "a waiting stage survives reopening the tab");
        key(ImGuiKey_Escape);
        check(until(is_done, 120), "Escape in the question ends it");
        frames(10);
        printf("KIND %d\n", (int)g_kind);
        return bad;
    }
    // a click on the Import button: the question's window is found by name, the button is the first one on its last line
    ImGuiWindow *ask = ImGui::FindWindowByName("##importok");
    check(ask != NULL, "the question is a window of its own");
    if (ask) {
        const ImGuiStyle &st = ImGui::GetStyle();
        float bh = ImGui::GetFontSize() + 2 * st.FramePadding.y;
        g_io->AddMousePosEvent(ask->Pos.x + st.WindowPadding.x + ImGui::GetFontSize() * 4.5f,
                               ask->Pos.y + ask->Size.y - st.WindowPadding.y - bh / 2);
        frames(2);
        g_io->AddMouseButtonEvent(0, true);
        frame();
        g_io->AddMouseButtonEvent(0, false);
        for (int i = 0; i < 20 && !is_working() && !is_done(); i++) frame();
    }
    bool clicked = is_working() || is_done();
    check(clicked, "a click on Import in the question starts the import");
    if (!clicked) start(JOB_INSTALL);  // (the click missed: the rest of the scenario still runs)
    check(until(is_done), "the import finishes");
    frames(60);  // (the result, the new list and its pictures)
    printf("KIND %d\nCODE %d\nDETAIL %s\nWARNINGS %d\n", (int)g_kind, g_code, g_detail.c_str(), g_warnings);
    printf("SLOTS_AFTER %zu\n", g_slots.size());
    printf("IMPORTED_SLOTS %s\nFLAGS %u %u %u\n", slot_list(g_rep).c_str(), g_rep.kept, g_rep.defaulted, g_rep.dropped);
    return bad;
}

int main(int argc, char **argv) {
    std::string scenario = argc > 1 ? argv[1] : "", save, game, src;
    if (argc >= 3 && scenario == "jpeg") {  // a real picture for the slots (the synthetic saves hold random bytes)
        std::vector<unsigned char> px(64 * 36 * 3);
        for (size_t i = 0; i < px.size(); i++) px[i] = (unsigned char)(i * 7);
        return stbi_write_jpg(argv[2], 64, 36, 3, px.data(), 90) ? 0 : 1;
    }
    for (int i = 2; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--save")) save = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--game")) game = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--src")) src = argv[++i];
    }
    if (scenario.empty() || save.empty() || game.empty()) {
        fprintf(stderr, "usage: test_saves_tab jpeg <path> | import|cancel|pick|backup --save <dir> --game <dir> [--src <dir>]\n");
        return 2;
    }
#ifdef _WIN32
    _putenv_s("SAVAGEWIND_SAVE_DIR", save.c_str());
#else
    setenv("SAVAGEWIND_SAVE_DIR", save.c_str(), 1);
#endif
    saves_tab_set_game_files(game.c_str());
    ImGui::CreateContext();
    g_io = &ImGui::GetIO();
    g_io->IniFilename = NULL;
    g_io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    g_io->DisplaySize = ImVec2(1920, 1080);
    g_io->Fonts->AddFontDefault();
    unsigned char *atlas;
    int aw, ah;
    g_io->Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
    int bad = run(scenario, src);
    printf("TEXTURES created=%d freed=%d\n", g_created, g_freed);
    saves_tab_unload();
    ImGui::DestroyContext();
    printf("DONE 1\n");
    return bad ? 1 : 0;
}
