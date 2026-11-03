// The start screen's Saves tab (Settings, menu.mm tab 3; never in the in-game menu: the game must not be running while
// its saves change). It lists the slots of the save folder with their pictures, imports a Wii U / Cemu save
// (runtime/services/save_convert.c), backs the saves up and opens their folder.
//
// An import runs on a worker thread, in two steps with the player's yes between them: the save is converted into a new
// staging folder next to the save folder (the saves are not touched), then, once confirmed, the save folder is moved
// whole to a dated backup folder and the staged one takes its place. Nothing is deleted at any point. Close to 200
// small files and the game's flag tables: well under a second on an SSD, but reading Bootup.pack can take a few on a
// slow disk, so it is not done on the UI thread.
//
// Dates come from the file times of caption.sav (when the game wrote the slot): the save has no date of its own that
// is known without the game's data. The play time is caption.sav's PlayTime flag when it has one, else not shown.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <atomic>
#include <string>
#include <vector>

#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"
#include "saves_tab.h"

#include "imageload.h"  // (stb_image: the pictures are JPEG)

extern "C" {
#include "../paths.h"
#include "../services/save_convert.h"
SDL_Window *ui_window(void);
#ifdef __APPLE__
void *ui_metal_device(void);
#else
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.cpp
void vk_imgui_texture_free(uint64_t tex);
#endif
}
void reveal_folder(const std::string &dir);  // launcher.mm: Explorer / Finder / the file manager

namespace {

enum Phase { IDLE, WORKING, CONFIRM, DONE, LOADING };  // of the worker; DONE: the result stays shown until the next job
enum Kind { NONE, IMPORTED, IMPORT_FAILED, BACKED_UP, BACKUP_FAILED, CANCELLED };
enum Job { JOB_CONVERT, JOB_INSTALL, JOB_BACKUP, JOB_LIST };

std::string g_game_root;                // <root>/romfs/Pack/Bootup.pack has the flag tables
std::atomic<int> g_phase{IDLE};         // written by the worker, read here (release / acquire: the fields below follow it)
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_picked;                   // the folder the dialog gave (SDL calls back on any thread)
pthread_t g_worker;
bool g_joinable;                       // only the UI / exit thread starts and joins workers
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_dialog{false};      // the folder dialog is open

// the last job's outcome
Kind g_kind = NONE;
int g_code;                             // ScCode of a failure
std::string g_detail;                   // its variable part (a path or a reason)
std::string g_backup;                   // the backup folder made (or to be made)
ScReport g_rep;                         // an import: the slots and the flag counts
ScStage *g_stage;                       // converted, waiting for the player's yes
std::string g_stage_dir;
bool g_had_saves;                       // the save folder is there: the import backs it up
int g_warnings, g_files;
bool g_seen_done = true, g_popup_shown;

// the slots in the save folder
struct Thumb { uint64_t tex = 0; int w = 0, h = 0; bool tried = false; };
std::vector<ScSlot> g_slots;
std::vector<ScSlot> g_loaded_slots;      // worker writes; the UI takes it after the phase is published
bool g_slots_ready;
int g_list_return_phase;
ScRecovery g_recovery;                  // worker writes it with the slot list
ScCode g_cleanup_code = SC_OK;
std::string g_cleanup_detail;
std::vector<Thumb> g_thumbs;
int g_last_frame = -100;                // the frame the tab was last drawn in: a gap means it was left and is back

std::string save_dir() {
    const char *e = getenv("SAVAGEWIND_SAVE_DIR");  // (tests: the same folder the game would use)
    std::string d = e && *e ? e : "save";
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d;
}

// ------------------------------------------------------------------ pictures
#ifdef __APPLE__
std::vector<id<MTLTexture>> g_mtl;  // (ImGui only holds the pointer)
#endif

uint64_t texture_rgba(const unsigned char *px, int w, int h) {
#ifdef __APPLE__
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:w height:h mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    if (!t) return 0;
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)w * 4];
    g_mtl.push_back(t);
    return (uint64_t)(intptr_t)(__bridge void *)t;
#else
    return vk_imgui_texture_rgba(px, w, h);
#endif
}

void texture_free(uint64_t tex) {
#ifdef __APPLE__
    for (size_t i = 0; i < g_mtl.size(); i++)
        if ((uint64_t)(intptr_t)(__bridge void *)g_mtl[i] == tex) { g_mtl.erase(g_mtl.begin() + i); break; }
#else
    vk_imgui_texture_free(tex);
#endif
}

void free_thumbs() {
    for (Thumb &t : g_thumbs)
        if (t.tex) texture_free(t.tex);
    g_thumbs.clear();
}

std::string slot_picture(const ScSlot &s) { return save_dir() + "/save_0/" + s.name + "/caption.jpg"; }

// One picture per frame, so a long list doesn't hold the frame up
void load_one_thumb() {
    for (size_t i = 0; i < g_slots.size(); i++) {
        Thumb &t = g_thumbs[i];
        if (t.tried || !g_slots[i].has_picture) continue;
        t.tried = true;
        std::string path = slot_picture(g_slots[i]);
        int w, h;
        if (!rt_image_info(path.c_str(), &w, &h) || w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
        unsigned char *px = rt_image_rgba(path.c_str(), &w, &h);
        if (!px) return;
        t.tex = texture_rgba(px, w, h);
        t.w = w, t.h = h;
        rt_image_free(px);
        return;
    }
}

void read_slots() {  // (worker only: no UI or texture calls)
    ScReport r = {};
    g_cleanup_code = sc_cleanup_stages(save_dir().c_str(), g_stage ? g_stage_dir.c_str() : NULL, &g_recovery, &r);
    g_cleanup_detail = r.detail;
    ScSlot *v = NULL;
    int n = sc_list_slots((save_dir() + "/save_0").c_str(), &v);
    g_loaded_slots.clear();
    if (n) g_loaded_slots.assign(v, v + n);
    free(v);
    g_slots_ready = true;
}

void take_slots() {
    free_thumbs();
    g_slots.swap(g_loaded_slots);
    g_loaded_slots.clear();
    g_thumbs.assign(g_slots.size(), Thumb());
    g_slots_ready = false;
}

// ------------------------------------------------------------------ the worker
struct Work { int what; std::string src; };

bool cancel_work(void *) { return g_cancel.load(std::memory_order_relaxed); }

void *worker(void *p) {
    Work *w = (Work *)p;
    std::string save = save_dir();
    ScReport r = {};
    if (w->what == JOB_LIST) {
        read_slots();
        g_phase.store(g_list_return_phase, std::memory_order_release);
    } else if (w->what == JOB_CONVERT) {
        ScFormat *fmt = NULL;
        ScStage *st = NULL;
        char stage[1100], backup[1100];
        sc_stage_path(save.c_str(), stage, sizeof stage);
        sc_backup_path(save.c_str(), backup, sizeof backup);
        ScLock *probe = NULL;  // (only a look: the install takes the lock again for the swap itself)
        bool in_use = sc_lock_take(save.c_str(), &probe) == SC_LOCK_BUSY;
        sc_lock_drop(probe);
        if (in_use) {  // (a game is running: not worth converting for)
            r.code = SC_IN_USE;
        } else if (cancel_work(NULL)) {
            r.code = SC_CANCELLED;
        } else if (rt_is_link(save.c_str()) || rt_is_link((save + "/save_0").c_str())) {
            r.code = SC_LINKED;
        } else if (sc_format_load((g_game_root + "/romfs/Pack/Bootup.pack").c_str(), &fmt, &r) == SC_OK) {
            ScCancel cancel = {cancel_work, NULL};
            sc_convert_cancel(w->src.c_str(), stage, fmt, 0, &cancel, &st, &r);
            sc_format_free(fmt);
        }
        g_code = r.code, g_detail = r.detail;
        g_rep = r;
        if (r.code == SC_OK) {
            g_stage = st, g_stage_dir = stage, g_backup = backup;
            struct stat sb;
            g_had_saves = stat(save.c_str(), &sb) == 0;
            g_phase.store(CONFIRM, std::memory_order_release);
        } else {
            g_kind = r.code == SC_CANCELLED ? CANCELLED : IMPORT_FAILED;
            g_phase.store(DONE, std::memory_order_release);
        }
    } else if (w->what == JOB_INSTALL) {
        ScCode c = sc_stage_install(g_stage, save.c_str(), g_backup.c_str(), &r);
        g_code = c, g_detail = r.detail, g_warnings = r.warnings;
        if (c == SC_OK) {
            g_stage = NULL;
            g_kind = IMPORTED;
            if (!g_had_saves) g_backup.clear();
        } else {
            if (c == SC_MOVED) sc_stage_keep(g_stage);  // (both copies must stay available for recovery)
            else sc_stage_discard(g_stage);
            g_stage = NULL;
            g_kind = IMPORT_FAILED;
        }
        sc_report_free(&r);
        if (c == SC_OK || c == SC_MOVED) read_slots();
        g_phase.store(DONE, std::memory_order_release);
    } else {
        char backup[1100];
        sc_backup_path(save.c_str(), backup, sizeof backup);
        ScCode c = sc_backup(save.c_str(), backup, &r);
        g_code = c, g_detail = r.detail, g_files = r.n_written, g_backup = backup;
        g_kind = c == SC_OK ? BACKED_UP : BACKUP_FAILED;
        sc_report_free(&r);
        g_phase.store(DONE, std::memory_order_release);
    }
    delete w;
    return NULL;
}

// No worker takes g_lock or waits for the UI. Join without holding that lock, including on exit from the launcher.
void join_worker() {
    if (g_joinable) pthread_join(g_worker, NULL), g_joinable = false;
}

void drop_stage_at_exit() {
    g_cancel.store(true, std::memory_order_relaxed);  // conversion can stop between files; installation always finishes
    join_worker();
    if (g_stage) sc_stage_discard(g_stage), g_stage = NULL;
}

void start(int what, const std::string &src = "") {
    static bool registered;
    if (!registered) registered = true, atexit(drop_stage_at_exit);
    join_worker();  // (the last job has published its result and can now be reaped)
    g_cancel.store(false, std::memory_order_relaxed);
    if (what == JOB_LIST) g_list_return_phase = g_phase.load();
    if (what != JOB_INSTALL && what != JOB_LIST) {
        sc_report_free(&g_rep);
        g_kind = NONE, g_code = 0, g_detail.clear(), g_backup.clear(), g_warnings = 0, g_files = 0;
    }
    g_seen_done = false;
    g_phase.store(what == JOB_LIST ? LOADING : WORKING, std::memory_order_release);
    Work *w = new Work{what, src};
    int err = pthread_create(&g_worker, NULL, worker, w);
    if (!err) g_joinable = true;
    else {
        delete w;
        if (g_stage) sc_stage_discard(g_stage), g_stage = NULL;
        g_code = SC_IO, g_detail = strerror(err);
        g_kind = what == JOB_BACKUP ? BACKUP_FAILED : IMPORT_FAILED;
        g_phase.store(DONE, std::memory_order_release);
    }
}

void SDLCALL folder_picked(void *, const char *const *files, int) {
    if (files && files[0]) {
        pthread_mutex_lock(&g_lock);
        g_picked = files[0];
        pthread_mutex_unlock(&g_lock);
    }
    g_dialog = false;
}

// ------------------------------------------------------------------ text
// A folder as the player finds it: relative ones are inside the data folder
std::string shown_path(const std::string &p) {
    bool abs = !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
    std::string s = abs ? p : std::string(rt_data_root()) + "/" + p;
#ifdef _WIN32
    for (char &c : s)
        if (c == '/') c = '\\';
#endif
    return s;
}

// "2026-10-03 15:40", or just the day (the same in every language)
std::string fmt_time(time_t t, bool day_only = false) {
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32];
    strftime(b, sizeof b, day_only ? "%Y-%m-%d" : "%Y-%m-%d %H:%M", &tmv);
    return b;
}

std::string fmt_play(int64_t sec) {
    char b[48];
    snprintf(b, sizeof b, tr("%d h %02d min"), (int)(sec / 3600), (int)(sec % 3600 / 60));
    return b;
}

std::string fmt(const char *en, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char *en, ...) {
    char b[1600];
    va_list ap;
    va_start(ap, en);
    vsnprintf(b, sizeof b, tr(en), ap);
    va_end(ap);
    return b;
}

std::string slot_list(const ScReport &r) {
    std::string s;
    for (int i = 0; i < r.nslots; i++) s += (i ? ", " : "") + std::string(r.slots[i]);
    return s;
}

// What went wrong: a headline and what to do about it (translated here, when it is shown)
void failure_text(int code, bool backup, std::string *head, std::string *hint) {
    *hint = "";
    switch (code) {
    case SC_NO_SAVE:
        if (backup) { *head = tr("There are no saves to back up yet"); break; }
        *head = tr("No Breath of the Wild save found in that folder");
        *hint = tr("Choose the Cemu profile folder (.../user/80000001) or a folder above it.");
        break;
    case SC_SEARCH_LIMIT:
        *head = tr("The search took too long. Choose a folder closer to the save.");
        break;
    case SC_SWITCH_SAVE:
        *head = tr("That is a Switch save already");
        *hint = tr("Savage Wind uses those as they are: copy its files into the save folder instead.");
        break;
    case SC_BAD_SAVE: *head = tr("A save file is damaged or incomplete"); break;
    case SC_NO_BOOTUP:
        *head = tr("The game files weren't found");
        *hint = tr("Set up the game files first (Prelaunch tab).");
        break;
    case SC_BAD_BOOTUP:
        *head = tr("The game files can't be read");
        *hint = tr("Pack/Bootup.pack looks damaged or is not from version 1.9.0. Set up the game files again.");
        break;
    case SC_LINKED:
        *head = tr("The save folder is a link");
        *hint = tr("Savage Wind doesn't replace a linked folder. Use a normal save folder.");
        break;
    case SC_IN_USE:
        *head = tr("Savage Wind is running in a game");
        *hint = tr("Close the game first, then try again.");
        break;
    case SC_MOVED:
        *head = tr("The import stopped halfway");
        *hint = tr("Your previous saves are safe in the backup folder shown below. Rename that folder to save to get them back.");
        break;
    default:
        *head = tr("Files could not be read or written");
        *hint = tr("Close anything that is using the save folder and try again.");
    }
}

// The last job's result, as lines in the rows' pane
void draw_result(int phase) {
    const ImVec4 gold(0.93f, 0.76f, 0.33f, 1), bad(1, 0.5f, 0.4f, 1);
    ImGui::PushTextWrapPos(0);
    if (phase == WORKING) {
        ImGui::TextColored(gold, "%s", tr("Working..."));
    } else if (g_kind == IMPORTED) {
        ImGui::TextColored(gold, "%s", tr("Import finished"));
        ImGui::TextWrapped("%s", fmt("Slots imported: %s", slot_list(g_rep).c_str()).c_str());
        ImGui::TextDisabled("%s", fmt("From: %s", shown_path(g_rep.root).c_str()).c_str());
        ImGui::TextWrapped("%s", fmt("Flags: %u kept, %u set to defaults (not in the old save), %u dropped (not in this game "
                                     "version).", g_rep.kept, g_rep.defaulted, g_rep.dropped).c_str());
        for (int i = 0; i < g_rep.nslots; i++) {  // (per slot: what the older game did not have, and what the new one dropped)
            unsigned def = 0, drop = 0;
            for (int k = 0; k < g_rep.nfiles; k++) {
                const ScFlagFile &f = g_rep.files[k];
                size_t n = strlen(g_rep.slots[i]);
                if (!strncmp(f.file, g_rep.slots[i], n) && f.file[n] == '/') def += f.defaulted, drop += f.dropped;
            }
            ImGui::TextDisabled("%s", fmt("Slot %s: %u defaulted, %u dropped", g_rep.slots[i], def, drop).c_str());
        }
        ImGui::TextWrapped("%s", fmt("Hero's Path: %d files. Album: %d pictures. Compendium: %d pictures.", g_rep.n_tracker,
                                     g_rep.n_album, g_rep.n_pict).c_str());
        if (!g_backup.empty())
            ImGui::TextWrapped("%s", fmt("Your previous saves are kept in %s", shown_path(g_backup).c_str()).c_str());
        if (g_warnings)
            ImGui::TextColored(bad, "%s", fmt("%d files of the old save folder could not be copied over; they are in the backup.",
                                              g_warnings).c_str());
    } else if (g_kind == BACKED_UP) {
        ImGui::TextColored(gold, "%s", tr("Backup finished"));
        ImGui::TextWrapped("%s", fmt("%d files copied to %s", g_files, shown_path(g_backup).c_str()).c_str());
    } else if (g_kind == CANCELLED) {
        ImGui::TextColored(gold, "%s", tr("Import cancelled"));
        ImGui::TextWrapped("%s", tr("Nothing was changed."));
    } else if (g_kind == IMPORT_FAILED || g_kind == BACKUP_FAILED) {
        std::string head, hint;
        failure_text(g_code, g_kind == BACKUP_FAILED, &head, &hint);
        ImGui::TextColored(bad, "%s", head.c_str());
        if (!hint.empty()) ImGui::TextWrapped("%s", hint.c_str());
        if (!g_detail.empty() && g_code != SC_NO_SAVE && g_code != SC_SWITCH_SAVE)
            ImGui::TextDisabled("%s", (g_code == SC_MOVED ? shown_path(g_detail) : g_detail).c_str());  // (that one is the backup folder)
        if (g_code != SC_MOVED) ImGui::TextWrapped("%s", tr("Nothing was changed."));
    }
    ImGui::PopTextWrapPos();
}

// "Import this save?": what comes in, and where the old saves go first
void draw_confirm(int phase) {
    if (phase == CONFIRM && !g_popup_shown) ImGui::OpenPopup("##importok"), g_popup_shown = true;
    if (phase != CONFIRM) g_popup_shown = false;
    if (!ImGui::BeginPopupModal("##importok", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) return;
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Import this save?"));
    ImGui::PushTextWrapPos(fminf(ImGui::GetFontSize() * 34, ImGui::GetIO().DisplaySize.x * 0.7f));  // (a small window too)
    ImGui::TextWrapped("%s", fmt("Slots: %s", slot_list(g_rep).c_str()).c_str());
    ImGui::TextDisabled("%s", fmt("From: %s", shown_path(g_rep.root).c_str()).c_str());
    ImGui::Spacing();
    if (g_had_saves) {
        ImGui::TextWrapped("%s", tr("Your current saves are moved to this backup folder first. Nothing is deleted:"));
        ImGui::TextDisabled("%s", shown_path(g_backup).c_str());
    } else {
        ImGui::TextWrapped("%s", tr("You have no saves yet, so there is nothing to back up."));
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    bool yes = ImGui::Button(tr("Import"), ImVec2(ImGui::GetFontSize() * 9, 0));
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    bool no = ImGui::Button(tr("Cancel"), ImVec2(ImGui::GetFontSize() * 9, 0)) ||
              ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (yes) {
        ImGui::CloseCurrentPopup();
        start(JOB_INSTALL);
    } else if (no) {
        ImGui::CloseCurrentPopup();
        sc_stage_discard(g_stage);  // (the converted copy: removed again, the saves were never touched)
        g_stage = NULL;
        g_kind = CANCELLED;
        g_seen_done = true;
        g_phase.store(DONE, std::memory_order_release);
    }
    ImGui::EndPopup();
}

}  // namespace

void saves_tab_set_game_files(const char *root) { g_game_root = root; }

bool saves_tab_busy(void) {
    int p = g_phase.load(std::memory_order_acquire);
    return p == WORKING || p == CONFIRM || p == LOADING;
}

void saves_tab_unload(void) {
    drop_stage_at_exit();
    free_thumbs();
    g_slots.clear();
    g_loaded_slots.clear();
    g_slots_ready = false;
    g_phase.store(IDLE);
    sc_report_free(&g_rep);
    g_kind = NONE;
    g_last_frame = -100;
}

void saves_tab_draw(void) {
    int frame = ImGui::GetFrameCount();
    if (g_last_frame + 3 < frame) {  // first time, or back after another tab (files may have changed)
        int p = g_phase.load(std::memory_order_acquire);
        if (p == IDLE || p == DONE) start(JOB_LIST);
        pthread_mutex_lock(&g_lock);
        g_picked.clear();  // (a folder chosen while the tab was away: not started by surprise)
        pthread_mutex_unlock(&g_lock);
    }
    g_last_frame = frame;
    int phase = g_phase.load(std::memory_order_acquire);
    if (phase != WORKING && phase != LOADING && g_slots_ready) take_slots();
    if (phase == DONE && !g_seen_done) g_seen_done = true;
    std::string picked;
    pthread_mutex_lock(&g_lock);
    picked.swap(g_picked);
    pthread_mutex_unlock(&g_lock);
    if (!picked.empty() && (phase == IDLE || phase == DONE) && !g_recovery.imported[0]) start(JOB_CONVERT, picked), phase = WORKING;
    load_one_thumb();

    bool working = phase == WORKING || phase == CONFIRM || phase == LOADING;
    bool recovery = !working && g_recovery.imported[0];
    bool busy = working || recovery;
    opt_begin("##saves", ImGui::GetFontSize() / 34);
    opt_section("Import");
    if (busy) opt_disable_next();
    if (opt_action("Import from Wii U / Cemu...", "",
                   "Imports a Breath of the Wild save from the Wii U or from Cemu. Choose the Cemu profile folder "
                   "(.../user/80000001) or any folder above it. The Hero's Path, the album and the compendium pictures "
                   "come along. Nothing changes until you confirm, and your current saves are backed up first.") && !busy &&
        !g_dialog.exchange(true))
        SDL_ShowOpenFolderDialog(folder_picked, NULL, ui_window(), NULL, false);
    opt_section("Backup");
    if (busy) opt_disable_next();
    if (opt_action("Back up saves", "",
                   "Copies the whole save folder to a new dated folder in save-backups, next to it. Your saves stay "
                   "where they are.") && !busy)
        start(JOB_BACKUP), phase = WORKING, working = busy = true;
    if (working) opt_disable_next();
    if (opt_action("Open save folder", "", "Opens the folder with your saves in your file manager.") && !working) {
        if (recovery) reveal_folder(g_recovery.imported);
        else {
            SDL_CreateDirectory(save_dir().c_str());  // (no save yet: the folder is made so there is something to open)
            reveal_folder(save_dir());
        }
    }
    if (recovery) {
        opt_section("Result");
        ImGui::TextWrapped("%s", tr("An earlier import was interrupted. The save folder is missing."));
        if (g_recovery.backup[0])
            ImGui::TextWrapped("%s", fmt("Previous saves (if any): %s", shown_path(g_recovery.backup).c_str()).c_str());
        ImGui::TextWrapped("%s", fmt("Imported saves: %s", shown_path(g_recovery.imported).c_str()).c_str());
        ImGui::TextWrapped("%s", tr("Close Savage Wind and rename one of these folders to save to recover it. These folders have been kept."));
    } else if (!working && g_cleanup_code != SC_OK) {
        opt_section("Result");
        ImGui::TextWrapped("%s", tr("Files could not be read or written"));
        ImGui::TextDisabled("%s", g_cleanup_detail.c_str());
    }
    if (phase == WORKING || phase == DONE) {  // (the worker's fields are read only once it has published them)
        opt_section("Result");
        draw_result(phase);
    }
    opt_section("Slots");
    if (phase == LOADING) {
        opt_disable_next();
        opt_action("Working...", "", "");
    } else if (g_slots.empty()) {
        opt_disable_next();
        opt_action("No saves yet", "",
                   "There are no saves in the save folder yet. They show up here once you have played, or imported one.");
    }
    for (size_t i = 0; i < g_slots.size(); i++) {
        const ScSlot &s = g_slots[i];
        std::string label = fmt("Slot %s", s.name), when = s.saved ? fmt_time(s.saved) : "";
        // the row shows the most of date, time and play time that fits beside the label (the pane has all of it)
        std::string options[] = {s.play_seconds >= 0 ? when + "   " + fmt_play(s.play_seconds) : when, when,
                                 s.saved ? fmt_time(s.saved, true) : ""};
        float room = ImGui::GetContentRegionAvail().x - 2 * ImGui::GetFontSize() - ImGui::CalcTextSize(tr_upper(label.c_str()).c_str()).x;
        std::string value = options[2];
        for (const std::string &o : options)
            if (ImGui::CalcTextSize(o.c_str()).x <= room) { value = o; break; }
        std::string desc = s.play_seconds >= 0 ? fmt("Saved %s\nPlay time: %s", when.c_str(), fmt_play(s.play_seconds).c_str())
                                               : fmt("Saved %s", when.c_str());
        opt_action(label.c_str(), value.c_str(), desc.c_str());
        if (g_thumbs[i].tex) opt_desc_image(g_thumbs[i].tex, g_thumbs[i].w, g_thumbs[i].h);
    }
    opt_end();
    draw_confirm(phase);
}
