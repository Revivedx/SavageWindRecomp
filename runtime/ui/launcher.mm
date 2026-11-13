// Start screen, shown before the game boots, in two modes (runtime/main.c picks):
// - setup: a plain start whose game files aren't set up yet (or whose game code was built for another version of
//   Savage Wind). Only the game files page: nothing of the game ships with Savage Wind, so the user picks the folder
//   with their own extracted game files (exefs/ and romfs/), we check the main module is an NSO of a supported version,
//   copy the files, build the game's data image and recompile the game code - then the game starts.
// - configuration ("Savage Wind Configuration", --config): PLAY, SETTINGS, GAME FILES (set up, set up again,
//   delete), QUIT, the game-files status at the lower left.
// Once the game files are ready a plain start goes straight into the game. Skipped with --play,
// SAVAGEWIND_NO_LAUNCHER=1, or any scripted test run (SAVAGEWIND_INPUT): tests boot straight in. Quit to Start Screen
// in game comes back here, to the configuration screen, without leaving the process (runtime/main.c reboots the game).
// The background: runtime/ui/splash.mm.
#ifdef __APPLE__
#import <AppKit/NSEvent.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "i18n.h"
#include "launch_flags_ui.h"
#include "file_size_cache.h"
#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner)
#include "option_rows.h"
#include "osk.h"
#include "saves_tab.h"
#include "splash.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
#endif
#include "imgui_impl_sdl3.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "platform.h"
#include "paths.h"
#include "gamebuild.h"
#include "gamelib.h"
#include "game_versions.h"
#include "dlc_check.h"
#include "cli.h"
#include "romfs_build.h"
#include "problem_report.h"
#include "post_update.h"
#include "steam_shortcuts.h"
#include "settings.h"
#include "color_grade.h"
#include "pc_cursor.h"
#include "input_bind.h"
#ifndef __APPLE__
#include "nvn/precompile.h"
#endif
void controls_bindings_draw(float scale);  // ui/controls_ui.mm
void ui_window_init(void);
SDL_Window *ui_window(void);
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
// runtime/nvn/vulkan: window frames and ImGui plumbing
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
void rec_drain(void);  // (runtime/nvn/vulkan/record.c)
#endif
}
#ifdef __APPLE__
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// (test) BGRA texture -> PNG (the game renderer's saver isn't up yet before the game boots)
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL when the file can't be created, e.g. its folder isn't there: CFRelease(NULL) traps)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    } else fprintf(stderr, "[launcher] can't write %s\n", path);
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

// The version this build is (also names the release files; Makefile / tools/bundle_macos.sh / make_dmg.sh read it).
#define SAVAGEWIND_VERSION "0.5.3-alpha.5"
extern "C" const char *rt_version(void) { return SAVAGEWIND_VERSION; }  // (the title screen's version line: fs.c)

// ------------------------------------------------------------------ game files
static std::string g_root;  // extracted game: <root>/exefs, <root>/romfs
static FileSizeCache g_file_size;

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The main module's NSO header: "NSO0" magic, build id (the first 8 of its 32 bytes, as hex) at 0x40. false: not an NSO.
static bool nso_build_id(const std::string &main_path, std::string *hex) {
    FILE *f = fopen(main_path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[0x48] = {0};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n != sizeof h || memcmp(h, "NSO0", 4)) return false;
    if (hex) {
        char b[17];
        for (int i = 0; i < 8; i++) snprintf(b + 2 * i, 3, "%02x", h[0x40 + i]);
        *hex = b;
    }
    return true;
}

// The game versions this build sets up (runtime/game_versions.h), told by the main module's build id (its first 8
// bytes; tools/sw_probe.py prints it). A known version that isn't supported yet is refused with its own message. The
// RomFS that goes with it is checked before the setup copies anything: its System/Version.txt, and how many files it
// has with the update applied over the base game and their sizes added up (check_romfs).
struct GameVersion {
    const char *name, *id;
    bool supported;
    unsigned romfs_files;            // (0: not counted)
    unsigned long long romfs_bytes;  // (0: not counted)
};
static const GameVersion VERSIONS[] = {
#define GAME_VERSION_ENTRY(v, id, ok, files, bytes) {v, id, ok, files, bytes},
    GAME_VERSIONS(GAME_VERSION_ENTRY)
#undef GAME_VERSION_ENTRY
};
static const GameVersion *main_version(const std::string &id) {
    for (const GameVersion &v : VERSIONS)
        if (*v.id && id == v.id) return &v;
    return nullptr;
}
static bool supported_main(const std::string &id) {
    const GameVersion *v = main_version(id);
    return v && v->supported;
}

// The texts that name versions have a %s: NOT_SUPPORTED_YET's is the game files' version, the others' the supported
// ones ("1.8.2", "1.8.2 / 1.9.0"). trv: tr with that filled in.
static const char *const NOT_SUPPORTED = "Game files aren't version %s";
static const char *const NOT_SUPPORTED_YET = "Version %s isn't supported yet";
static std::string g_files_version;  // (a known version not supported yet: the game files')
static std::string supported_versions() {
    std::string s;
    for (const GameVersion &v : VERSIONS)
        if (v.supported) s += (s.empty() ? "" : " / ") + std::string(v.name);
    return s;
}
static std::string trv(const std::string &key) {
    std::string s = tr(key.c_str()), arg = key == NOT_SUPPORTED_YET ? g_files_version : supported_versions();
    for (size_t at = s.find("%s"); at != std::string::npos; at = s.find("%s", at + arg.size())) s.replace(at, 2, arg);
    return s;
}

// Files under dir; the count stops at `enough` (a whole RomFS has tens of thousands: no need to walk them all)
static unsigned count_files(const std::string &dir, unsigned enough, unsigned n = 0) {
    DIR *d = opendir(dir.c_str());
    if (!d) return n;
    while (n < enough) {
        struct dirent *e = readdir(d);
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = dir + "/" + e->d_name;
        if (is_dir(p)) n = count_files(p, enough, n);
        else n++;
    }
    closedir(d);
    return n;
}

// Files under dir and their sizes added up, added to *files and *bytes
static void tally_files(const std::string &dir, unsigned *files, unsigned long long *bytes) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) tally_files(p, files, bytes);
        else ++*files, *bytes += (unsigned long long)st.st_size;
    }
    closedir(d);
}

// The cutscene videos: all 30 in Movie/. A RomFS without that folder (left out when it was extracted, or lost copying
// it) passed the file count, 30 short of ~41,000, and the game crashed at the first pre-rendered cutscene, the Great
// Plateau Tower's (#292): such files are incomplete, before the setup copies them and before a start plays them.
static const unsigned MOVIE_FILES = 30;
static bool movies_complete(const std::string &romfs) { return count_files(romfs + "/Movie", MOVIE_FILES) >= MOVIE_FILES; }

// The RomFS of version v, update applied? "" = yes, else what was found, said to the player (in the menu language).
// A common mistake used to pass this far and the game then failed with no explanation: the update's ExeFS with the
// cartridge's RomFS, which says its own older version, or with the update's RomFS alone, which has far fewer files.
static std::string check_romfs(const std::string &romfs, const GameVersion &v) {
    char found[64] = "";
    if (FILE *f = fopen((romfs + "/System/Version.txt").c_str(), "rb")) {
        size_t n = fread(found, 1, sizeof found - 1, f);
        fclose(f);
        found[n] = 0;
        char *a = found, *b = found + n;  // (trimmed: a byte order mark, spaces, line ends)
        if (!strncmp(a, "\xEF\xBB\xBF", 3)) a += 3;
        while (a < b && isspace((unsigned char)*a)) a++;
        while (b > a && isspace((unsigned char)b[-1])) *--b = 0;
        memmove(found, a, strlen(a) + 1);
    }
    // Every file and every byte: an extraction a few files short, or with some cut short, is incomplete (#472), and the
    // game fails on the first of them it reads. More is fine (a RomFS with the DLC merged into it).
    unsigned files = 0;
    unsigned long long bytes = 0;
    if (!strcmp(found, v.name) && v.romfs_files) tally_files(romfs, &files, &bytes);  // (only for the right version)
    fprintf(stderr, "[launcher] romfs: version \"%s\", %u files, %llu bytes (%s wants \"%s\", %u, %llu)\n", found, files,
            bytes, v.id, v.name, v.romfs_files, v.romfs_bytes);
    char msg[400] = "";
    if (!*found) snprintf(msg, sizeof msg, "%s", tr("It has no System/Version.txt."));
    else if (strcmp(found, v.name)) {
        for (char *c = found; *c; c++)  // (shown as text: printable, and no % - trv fills those in)
            if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e || *c == '%') *c = '?';
        snprintf(msg, sizeof msg, tr("Its System/Version.txt says %s."), found);
    } else if (files < v.romfs_files)
        snprintf(msg, sizeof msg, tr("It has %u files; with the update applied it has %u."), files, v.romfs_files);
    else if (bytes < v.romfs_bytes)
        snprintf(msg, sizeof msg, tr("Its files add up to %llu MB; with the update applied they are %llu MB."), bytes >> 20,
                 v.romfs_bytes >> 20);
    return msg;
}

// The copied game files alone ("" = there), without the game code built from them.
static std::string check_copied_files(const std::string &root) {
    if (!file_exists(root + "/exefs/main")) return "No game files";
    std::string id;
    if (!nso_build_id(root + "/exefs/main", &id)) return "Game files unreadable (exefs/main is not an NSO)";
    if (!supported_main(id)) {
        const GameVersion *v = main_version(id);
        if (v) g_files_version = v->name;
        return v ? NOT_SUPPORTED_YET : NOT_SUPPORTED;
    }
    if (!is_dir(root + "/romfs")) return "Game data missing";
    if (!movies_complete(root + "/romfs")) return "Game files incomplete";
    return "";
}

// An older version's data image: the game reads the romfs/ folder itself now (runtime/romfs_image.h), 15 GB back
static void remove_old_image(const std::string &root) {
    if (file_exists(root + "/romfs.bin") && check_copied_files(root).empty() &&
        rt_remove_game_file(root.c_str(), "romfs.bin"))
        fprintf(stderr, "[launcher] removed the old game data image (romfs.bin)\n");
}

// "" = ready to play, else what is wrong
static std::string check_game_files(const std::string &root) {
    std::string why = check_copied_files(root);
    if (why.empty() && !gamelib_present(root.c_str())) {  // (a library with another id: an update of Savage Wind, or
        if (!file_exists(root + "/" GAMELIB_NAME)) return "Game code not built yet";  // a PC with another kind of CPU)
        return gamelib_other_cpu(root.c_str()) ? "Game code built for another CPU" : "Game code built for another version";
    }
    return why;
}

// (runtime/main.c) a plain start goes straight into the game when this is true
extern "C" bool launcher_game_ready(const char *root) {
    remove_old_image(root);
    return check_game_files(root).empty();
}

#ifdef __APPLE__
// (runtime/main.c) macOS: Option held while the app opens asks for the configuration screen - what "Savage Wind
// Configuration.exe" is on Windows and the desktop entry's action on Linux. The Mac way (as Photos or Steam ask for
// their library with it), and no second app to install.
extern "C" bool launcher_config_key_held(void) { return (NSEvent.modifierFlags & NSEventModifierFlagOption) != 0; }
#endif

// ------------------------------------------------------------------ setup (background thread)
// The player brings their own game files: a folder holding exefs/ and romfs/. They're copied into the data root (APFS
// clones / hard links where the file system allows); the game reads romfs/ in place (runtime/romfs_image.h).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_src;           // the picked folder
static std::string g_job;           // current setup step / result for the status line
static std::string g_job_hint;      // a failure: what to do about it (shown under it)
static bool g_busy, g_job_failed;
static bool g_confirm_delete;       // game-files page: "Delete the game files?" shown

static void set_job(const std::string &s, bool failed = false, const std::string &hint = "") {
    if (failed) fprintf(stderr, "[launcher] FAILED: %s | %s\n", s.c_str(), hint.c_str());
    pthread_mutex_lock(&g_lock);
    g_job = s;
    g_job_failed = failed;
    g_job_hint = hint;
    pthread_mutex_unlock(&g_lock);
}

// Setup failures: a short headline and what to do next (also SAVAGEWIND_LAUNCHER_SHOT=<f>:fail<1-9>, screenshots)
enum {
    FAIL_NOT_FOUND = 1, FAIL_UNUSABLE, FAIL_COPY, FAIL_VERSION, FAIL_MISSING, FAIL_BUILD, FAIL_NOT_SUPPORTED,
    FAIL_NOT_YET, FAIL_ROMFS
};
static void set_romfs_failure(const GameVersion &v, const std::string &found);
static void set_failure(int k) {
    static const char *const head[] = {"", "No game files found in that folder", "Those files can't be used",
                                       "Setup didn't finish", "The game's main module can't be read", "Game files incomplete",
                                       "The game code didn't build", "Not version %s", NOT_SUPPORTED_YET};
    static const char *const hint[] = {
        "",
        "Choose the folder that holds the extracted game files (exefs and romfs).",
        "Choose the folder that holds the extracted game files (exefs and romfs), not a game package.",
        "Make sure there's enough free space for a copy of the game files and try again.",
        "exefs/main must be the decrypted, extracted main module (an NSO file).",
        "Some files are missing. Choose the folder again; if it happens again, extract the game again.",
        "Make sure there's enough free space and try again. The log has the details.",
        "Savage Wind needs the game updated to version %s: the %s ExeFS and the RomFS with the update merged "
        "in. Update the game on your console, extract it again and choose that folder.",
        "Savage Wind plays version %s for now. If you kept a copy of the game at that version, choose its folder."};
    if (k >= 1 && k <= 8) set_job(head[k], true, hint[k]);
    if (k == FAIL_ROMFS) {  // (screenshots: a cartridge's RomFS)
        char found[200];
        snprintf(found, sizeof found, tr("Its System/Version.txt says %s."), "1.0.0");
        set_romfs_failure(VERSIONS[0], found);
    }
}

// The chosen RomFS isn't the one for the ExeFS's version: what was found, and to extract it again with the update
// applied. (Already in the menu language: the version goes into the texts.)
static void set_romfs_failure(const GameVersion &v, const std::string &found) {
    char head[200], hint[700];
    snprintf(head, sizeof head, tr("The RomFS isn't version %s with the update applied"), v.name);
    snprintf(hint, sizeof hint, tr("Savage Wind needs the RomFS of version %s with the update applied over the base "
                                   "game. Extract the RomFS again that way and choose that folder."), v.name);
    set_job(head, true, found + " " + hint);
}

// A status that names the game files' version: its failure
static int version_failure(const std::string &st) {
    return st == NOT_SUPPORTED ? FAIL_NOT_SUPPORTED : st == NOT_SUPPORTED_YET ? FAIL_NOT_YET : 0;
}

// What keeps the game from starting, said on the game files page as a setup failure is (nothing when there are simply
// no game files yet: the page is the welcome then)
static void explain_status(const std::string &st) {
    if (st.empty() || st == "No game files") return;
    if (st == "Game code built for another version")
        set_job("The game code was built for another version of Savage Wind", true,
                "Savage Wind was updated: choose Set up game files to build the game code again from your copy of the "
                "game. This takes a few minutes.");
    else if (st == "Game code built for another CPU")  // (runtime/gamelib.h: the game folder came from another PC)
        set_job("The game code was built for another kind of CPU", true,
                "This PC's CPU needs the game code built for it: choose Set up game files to build it again from your "
                "copy of the game. This takes a few minutes.");
    else if (st == "Game code not built yet")
        set_job("The game code isn't built yet", true,
                "Choose Set up game files to build it from your copy of the game. This takes a few minutes.");
    else set_failure(st.find("NSO") != std::string::npos ? FAIL_VERSION
                     : version_failure(st) ? version_failure(st) : FAIL_MISSING);
}

// exefs: the folder with main and main.npdm; romfs: a folder named romfs, or one holding System/Version.txt (the
// game's data root: some tools name it after the game); dlc: the RomFS folders of downloadable content
// (System/AocVersion.txt and no Version.txt; dumps keep them next to the game's, and "dlc1" sorts before "main").
// Found in the picked folder or a few levels below it (extraction tools nest them).
static bool is_romfs_dir(const std::string &dir) {
    size_t sl = dir.find_last_of("/\\");
    std::string n = sl == std::string::npos ? dir : dir.substr(sl + 1);
    for (auto &ch : n) ch = (char)tolower((unsigned char)ch);
    return n == "romfs" || file_exists(dir + "/System/Version.txt");
}
static bool is_dlc_romfs(const std::string &dir) {
    return file_exists(dir + "/System/AocVersion.txt") && !file_exists(dir + "/System/Version.txt");
}
static void find_game(const std::string &dir, int depth, std::string &exefs, std::string &romfs,
                      std::vector<std::string> &dlc, bool &unusable) {
    if (depth > 4) return;
    if (exefs.empty() && file_exists(dir + "/main") && file_exists(dir + "/main.npdm")) exefs = dir;
    if (is_dlc_romfs(dir)) { dlc.push_back(dir); return; }            // (a DLC's own tree)
    if (romfs.empty() && is_romfs_dir(dir)) { romfs = dir; return; }  // (not searched further: the game's own tree)
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext == ".xci" || ext == ".nsp" || ext == ".nca" || ext == ".nsz" || ext == ".xcz") unusable = true;
        if (is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_game(s, depth + 1, exefs, romfs, dlc, unusable);
}

// progress (the overlay's bar): the romfs copy (bytes)
static std::atomic<unsigned long long> g_done, g_total;
// The bar: copying is the first tenth, building the game code (runtime/gamebuild.c) the rest
static std::atomic<float> g_build_frac{-1};  // -1: not building
static bool g_copy_skipped;                  // (only the game code to build: the bar is all of that)
static void setup_progress(uint64_t done, uint64_t) { g_done = done; }
static void build_progress(double f, const char *step) {
    g_build_frac = (float)f;
    static const char *last;
    if (step != last) last = step, set_job(step);
}
static float extract_progress(void) {
    unsigned long long t = g_total, d = g_done;
    float b = g_build_frac, f = b >= 0 ? (g_copy_skipped ? b : 0.1f + 0.9f * b) : t ? 0.1f * (float)((double)d / (double)t) : 0;
    return f < 0 ? 0 : f > 0.99f ? 0.99f : f;
}

// Why the game code didn't build, from what the builder and the compiler said (runtime/gamebuild.c): what the player
// can do about it, with the reason itself under it for a bug report.
static bool said(const std::string &low, std::initializer_list<const char *> words) {
    for (const char *w : words)
        if (low.find(w) != std::string::npos) return true;
    return false;
}
static void set_build_failure(const char *why) {
    std::string low = why, detail = why;
    for (auto &c : low) c = (char)tolower((unsigned char)c);
    for (auto &c : detail) if (c == '%' || (unsigned char)c < 0x20) c = ' ';  // (shown as text: trv fills % in)
    const char *head = "The game code didn't build";
    const char *hint;
    if (said(low, {"no space", "disk full", "nospaceleft", "not enough space", "enospc", "file system is full"}))
        head = "The disk is full", hint = "Building the game code ran out of disk space. Free at least 5 GB on this disk and choose Set up game files.";
    else if (said(low, {"out of memory", "outofmemory", "cannot allocate", "bad_alloc", "not enough memory", "memoryerror"}))
        head = "This PC ran out of memory", hint = "Building the game code needs a lot of RAM. Close your browser and other programs and choose Set up game files.";
    else if (said(low, {"missing from this copy", "didn't start"}))
        head = "A part of Savage Wind is missing", hint = "The compiler or the code generator (the zig and swgen folders) is missing or was blocked. An antivirus is the usual cause: add this folder as an exclusion, then get the release again.";
    else if (said(low, {"access is denied", "accessdenied", "permission denied", "being used by another"}))
        head = "A file was blocked", hint = "Windows or an antivirus blocked a file while building. Add this folder as an exclusion in your antivirus and choose Set up game files.";
    else if (said(low, {"code modules can't be loaded"}))
        head = "The game files can't be read", hint = "exefs/ in your copy of the game looks damaged. Extract the game again (version %s) and choose that folder.";
    else
        hint = "Something unexpected stopped the build. Choose Set up game files to try again. If it happens again, report it with savagewind.log.";
    set_job(head, true, trv(hint) + "\n\n" + tr("Details:") + " " + detail);
}

// Building the game code takes room for the generated code, the objects and the compiler's caches
static bool room_for_build(void) {
    static const unsigned long long NEED = 5ull << 30;  // (measured: 1.5 GB at the peak, shaders included)
    std::error_code ec;
    std::filesystem::space_info si = std::filesystem::space(rt_data_root(), ec);
    if (const char *t = getenv("SAVAGEWIND_TEST_FREE_GB")) si.available = strtoull(t, NULL, 10) << 30;
    if (ec || si.available >= NEED) return true;
    char hint[400];
    snprintf(hint, sizeof hint, tr("Building the game code needs about %.0f GB free on this disk and there are %.1f GB. Free some space and choose Set up game files."),
             NEED / 1073741824.0, si.available / 1073741824.0);
    set_job("Not enough free disk space", true, hint);
    return false;
}

// Builds the game code from the copied files (a few minutes); false = failed (the job line says so)
static bool build_game_code(void) {
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1};
    char why[800] = "";
    g_build_frac = 0;
    bool ok = gamebuild_run(g_root.c_str(), &o, why, sizeof why);
    if (!ok) {
        fprintf(stderr, "[launcher] game code: %s\n", why);
        set_build_failure(why);
    }
    return ok;
}

// ------------------------------------------------------------------ shader precompilation (runtime/nvn/precompile.h)
// After a setup when its option is on (the setup page's PRECOMPILE ALL SHADERS, settings.precompile_shaders), or on
// its own from the game files page of the configuration screen. The progress overlay shows it; Esc / B stops it
// (what is done stays in the caches).
#ifndef __APPLE__
static ShaderPrecompile g_pc;
static std::atomic<bool> g_precompiling{false}, g_pc_only{false};

static void run_precompile(void) {
    atomic_store(&g_pc.state, SP_IDLE);
    atomic_store(&g_pc.done, 0);
    atomic_store(&g_pc.total, 0);
    atomic_store(&g_pc.cancel, false);
    g_precompiling = true;
    set_job("Precompiling shaders...");
    bool ok = shader_precompile_run(&g_pc, (g_root + "/romfs").c_str());
    g_precompiling = false;
    if (ok) set_job("Shaders precompiled.");
    else if (atomic_load(&g_pc.state) == SP_CANCELLED) set_job("Shader precompilation stopped.");
    else {
        fprintf(stderr, "[launcher] shader precompilation: %s\n", g_pc.why);
        set_job("The shaders couldn't be precompiled", true, "The game runs all the same: shaders then compile the first "
                "time they are drawn. The log has the details.");
    }
}

static void *precompile_thread(void *) {
    run_precompile();
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// The overlay's step line while precompiling: the step and its count
static std::string precompile_line(void) {
    int st = atomic_load(&g_pc.state), d = atomic_load(&g_pc.done), t = atomic_load(&g_pc.total);
    const char *what = st == SP_READING ? "Reading the shader archives..." : st == SP_COMPILING ? "Translating and compiling shaders..."
                     : st == SP_PIPELINES ? "Building pipelines..." : "Precompiling shaders...";
    std::string line = tr(what);
    if ((st == SP_COMPILING || st == SP_PIPELINES) && t > 0) {
        char n[64];
        snprintf(n, sizeof n, "  %d / %d", d, t);
        line += n;
    }
    return line;
}
#endif

static std::atomic<bool> g_stay_on_setup{false}, g_dlc_checking{false};

// Linux: the update runs in this program (runtime/post_update.h). It comes back only if it failed.
// Add to Steam (runtime/steam_shortcuts.h): waits for Steam to close, so on a thread of its own
static std::atomic<bool> g_steam_busy{false};
static void *steam_thread(void *) {
    char why[700] = "";
    bool ok = steam_add_savage_wind(why, sizeof why);
    for (char *c = why; *c; c++)
        if (*c == '%') *c = ' ';  // (shown as text: trv fills % in)
    if (ok) set_job(tr("Added to Steam: Savage Wind (straight into the game) and Savage Wind Configuration."));
    else set_job(tr("Couldn't add Savage Wind to Steam"), true, why);
    g_steam_busy = false;
    return NULL;
}

static void *update_thread(void *) {
    char title[200] = "", what[900] = "";
    update_apply_here(title, sizeof title, what, sizeof what);
    for (char *c = what; *c; c++)
        if (*c == '%') *c = ' ';  // (shown as text: trv fills % in)
    set_job(title, true, what);
    g_stay_on_setup = true;
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static bool stay_on_setup_finished(void) {  // (a check, shader precompilation or failed setup: the game files page stays)
    bool stay = g_stay_on_setup.exchange(false);
#ifndef __APPLE__
    stay |= g_pc_only.exchange(false);
#endif
    return stay;
}

// ---- downloadable content (runtime/services/fs.c, runtime/dlc_check.c): each DLC's RomFS is copied to aoc<index>/ and
// mounted by the game as on the console. On the Switch the first DLC (01007EF00011F001, The Master Trials) holds the
// data of both; the second's RomFS has only its version: the one with more files is 1, and the game unlocks the DLC
// content only when it finds both (#470, #497).
static const int DLC_MAX = RT_DLC_MAX;
#define AOC_DIR "aoc%d"
#define AOC_IMAGE "aoc%d.bin"  // (images older setups built: removed with the folder)
static RtDlcCheck g_dlc;       // guarded by g_lock when updated from a worker thread
static std::string aoc_name(const char *fmt, int i) {
    char n[32];
    snprintf(n, sizeof n, fmt, i);
    return n;
}
static std::string aoc_version(const std::string &dir) {
    char v[32] = "";
    if (FILE *f = fopen((dir + "/System/AocVersion.txt").c_str(), "rb")) {
        size_t n = fread(v, 1, sizeof v - 1, f);
        fclose(f);
        v[n] = 0;
        for (char *c = v; *c; c++)
            if (!isprint((unsigned char)*c)) { *c = 0; break; }
    }
    return v;
}

static std::string dlc_folders_list(unsigned mask) {
    std::string list;
    for (int i = 1; i <= DLC_MAX; i++)
        if (mask & (1u << (i - 1))) list += (list.empty() ? "" : " + ") + std::to_string(i);
    return list;
}

static std::string dlc_value_text(const RtDlcCheck &c) {
    switch (c.state) {
    case RT_DLC_NONE: return tr("Not set up");
    case RT_DLC_READY: return dlc_folders_list(c.present_mask) + " (" + c.version + ")";
    case RT_DLC_ONLY_ONE: return tr("1 of 2");
    case RT_DLC_WRONG_VERSION: {
        char b[64];
        snprintf(b, sizeof b, tr("Version %s"), c.version);
        return b;
    }
    default: return tr("Not ready");
    }
}

static void dlc_problem_text(const RtDlcCheck &c, std::string *head, std::string *hint) {
    char b[512];
    switch (c.state) {
    case RT_DLC_ONLY_ONE:
        *head = tr("Only one of the two DLC is set up");
        *hint = tr("The game unlocks the DLC content only when it finds both DLC in separate folders (aoc1 and aoc2). "
                   "Extracting both into one folder leaves only one.");
        break;
    case RT_DLC_SWAPPED:
        *head = tr("The two DLC folders are swapped");
        *hint = tr("aoc1 must hold the first DLC's RomFS (about 1.4 GB) and aoc2 the second's (System/AocVersion.txt).");
        break;
    case RT_DLC_WRONG_VERSION:
        *head = tr("The DLC isn't version 3.0");
        snprintf(b, sizeof b,
                 tr("Its System/AocVersion.txt says %s. Savage Wind needs Switch DLC version 3.0, updated on your "
                    "console and extracted again."),
                 c.version[0] ? c.version : "?");
        *hint = b;
        break;
    case RT_DLC_MERGED:
        *head = tr("The DLC is merged into the game's romfs");
        *hint = tr("The game unlocks the DLC only when aoc1 and aoc2 are in their own folders, apart from romfs. "
                   "Extract the base game + update RomFS and the two DLC into separate folders and set them up again.");
        break;
    case RT_DLC_MISNAMED:
        *head = tr("DLC folder isn't named aoc1 / aoc2");
        snprintf(b, sizeof b,
                 tr("Found a DLC RomFS in %s, which the game doesn't read in place. Name the two DLC folders aoc1 and "
                    "aoc2, or set them up from here."),
                 c.detail[0] ? c.detail : "?");
        *hint = b;
        break;
    default:
        *head = tr("Downloadable content incomplete");
        if (c.nested)
            *hint = tr("The DLC's files are inside a subfolder: System/AocVersion.txt must be right inside aoc1/System/ "
                       "and aoc2/System/.");
        else {
            snprintf(b, sizeof b, tr("Missing or unreadable: %s. Extract both DLC again in separate folders and set them up."),
                     c.detail[0] ? c.detail : "aoc1");
            *hint = b;
        }
        break;
    }
}

static void dlc_log(const RtDlcCheck &c) {
    static RtDlcState last_state = (RtDlcState)-1;
    static std::string last_detail;
    std::string cur = std::string(c.version) + "|" + c.detail + (c.nested ? "|1" : "|0");
    if (c.state == last_state && cur == last_detail) return;
    last_state = c.state, last_detail = cur;
    if (c.state == RT_DLC_NONE) return;
    if (c.state == RT_DLC_READY)
        fprintf(stderr, "[launcher] dlc: ready (aoc1 + aoc2, version \"%s\")\n", c.version);
    else
        fprintf(stderr, "[launcher] dlc: %s (mask=0x%x version=\"%s\" detail=\"%s\"%s)\n", rt_dlc_state_name(c.state),
                c.present_mask, c.version, c.detail, c.nested ? " nested" : "");
}

static void refresh_dlc(void) {
    RtDlcCheck c = rt_dlc_check(g_root.c_str());
    pthread_mutex_lock(&g_lock);
    g_dlc = c;
    pthread_mutex_unlock(&g_lock);
    dlc_log(c);
}

static void *dlc_check_thread(void *) {
    g_dlc_checking = true;
    set_job("Checking downloadable content...");
    RtDlcCheck c = rt_dlc_verify(g_root.c_str());
    g_dlc_checking = false;
    pthread_mutex_lock(&g_lock);
    g_dlc = c;
    pthread_mutex_unlock(&g_lock);
    dlc_log(c);
    if (c.state == RT_DLC_READY) {
        fprintf(stderr, "[launcher] dlc verified: %u files, %llu bytes (version \"%s\")\n", c.files,
                (unsigned long long)c.bytes, c.version);
        char ok[256];
        snprintf(ok, sizeof ok, tr("Downloadable content verified: %u files, %llu MB (version %s)."), c.files,
                 (unsigned long long)(c.bytes >> 20), c.version);
        set_job(ok);
    } else if (c.state == RT_DLC_NONE) {
        set_job("No downloadable content is set up.");
    } else {
        std::string head, hint;
        dlc_problem_text(c, &head, &hint);
        set_job(head, true, hint);
    }
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void remove_dlc(const std::string &root) {
    for (int i = 1; i <= DLC_MAX; i++) {
        rt_remove_game_file(root.c_str(), aoc_name(AOC_DIR, i).c_str());
        rt_remove_game_file(root.c_str(), aoc_name(AOC_IMAGE, i).c_str());
    }
}

static unsigned long long dlc_bytes(const std::vector<std::string> &dlc) {
    unsigned long long n = 0;
    for (auto &d : dlc) n += romfs_dir_bytes(d.c_str());
    return n;
}

// Copies the DLCs' RomFS folders in place of the ones set up before (the game reads each from its folder, as its
// own romfs/). false: failed (the DLC set up so far is removed again)
static bool setup_dlc(std::vector<std::string> dlc) {
    std::vector<std::pair<unsigned, std::string>> by_files;
    for (auto &d : dlc) by_files.push_back({count_files(d, ~0u), d});
    std::stable_sort(by_files.begin(), by_files.end(), [](auto &a, auto &b) { return a.first > b.first; });
    remove_dlc(g_root);
    for (size_t i = 0; i < by_files.size() && (int)i < DLC_MAX; i++) {
        const std::string &d = by_files[i].second, n = aoc_name(AOC_DIR, (int)i + 1);
        std::string to = g_root + "/" + n;
        if (file_exists(to) || rt_is_link(to.c_str())) {  // (still there: a link, left alone - never copied into)
            fprintf(stderr, "[launcher] DLC: %s/%s is in the way (a link?)\n", g_root.c_str(), n.c_str());
            remove_dlc(g_root);
            return false;
        }
        fprintf(stderr, "[launcher] DLC %zu: %s (%u files, version \"%s\")\n", i + 1, d.c_str(), by_files[i].first,
                aoc_version(d).c_str());
        if (!romfs_copy_tree(d.c_str(), (g_root + "/" + n).c_str(), setup_progress)) { remove_dlc(g_root); return false; }
    }
    return true;
}

// path is folder or something inside it?
static bool path_inside(const std::string &path, const std::string &folder) {
    char a[4096], b[4096];
    if (!realpath(path.c_str(), a) || !realpath(folder.c_str(), b)) return false;
    size_t n = strlen(b);
    return !strncmp(a, b, n) && (a[n] == '/' || a[n] == '\\' || !a[n]);
}

static void *extract_thread(void *) {
    std::string src;
    pthread_mutex_lock(&g_lock);
    src = g_src;
    pthread_mutex_unlock(&g_lock);
    g_done = 0, g_total = 0, g_build_frac = -1, g_copy_skipped = false;
    std::string exefs, romfs;
    std::vector<std::string> dlc;
    bool unusable = false;  // (game packages rather than its files)
    bool dlc_only = false;  // (the shaders precompiled are the game's: nothing new to precompile)
    bool new_version = post_update_pending(SAVAGEWIND_VERSION);  // (runtime/post_update.h: a release's first start)
    if (src.empty() && check_copied_files(g_root).empty()) {  // copied already (an update needs new game code)
        g_copy_skipped = true;
        if (new_version) {  // what a wrong copy of the game would only show as a crash later (the RomFS of another version)
            std::string id;
            const GameVersion *v = nso_build_id(g_root + "/exefs/main", &id) ? main_version(id) : NULL;
            set_job("Checking the game files...");
            std::string found = v ? check_romfs(g_root + "/romfs", *v) : "";
            if (!found.empty()) { set_romfs_failure(*v, found); goto done; }
        }
        if (!new_version || !gamelib_present(g_root.c_str())) {  // (the same game code as the last version: nothing to build)
            if (!room_for_build() || !build_game_code()) goto done;
        } else fprintf(stderr, "[launcher] update: the game code is already right for this version\n");
        goto check;
    }
    set_job("Looking for the game files...");
    find_game(src, 0, exefs, romfs, dlc, unusable);
    if (exefs.empty() && romfs.empty() && !dlc.empty()) {  // DLC alone: added to the game files set up
        if (!check_copied_files(g_root).empty()) {
            set_job("Set up the game files first", true, "Choose the folder with the game files (exefs and romfs) and set "
                    "them up, then the one with the downloadable content.");
            goto done;
        }
        g_total = dlc_bytes(dlc);
        set_job("Setting up the downloadable content...");
        if (!setup_dlc(dlc)) { set_failure(FAIL_COPY); goto done; }
        g_copy_skipped = dlc_only = true;
        goto check;
    }
    if (exefs.empty() || romfs.empty()) { set_failure(unusable ? FAIL_UNUSABLE : FAIL_NOT_FOUND); goto done; }
    {
        std::string id;
        if (!nso_build_id(exefs + "/main", &id)) { set_failure(FAIL_VERSION); goto done; }
        fprintf(stderr, "[launcher] main build id %s\n", id.c_str());
        if (!supported_main(id)) {  // (before copying 15 GB)
            const GameVersion *v = main_version(id);
            if (v) g_files_version = v->name;
            set_failure(v ? FAIL_NOT_YET : FAIL_NOT_SUPPORTED);
            goto done;
        }
        set_job("Checking the game files...");
        const GameVersion *v = main_version(id);
        std::string found = check_romfs(romfs, *v);  // (the RomFS that goes with that ExeFS)
        if (!found.empty()) { set_romfs_failure(*v, found); goto done; }
        if (!movies_complete(romfs)) {
            fprintf(stderr, "[launcher] romfs: Movie/ has fewer than the game's %u videos\n", MOVIE_FILES);
            set_failure(FAIL_MISSING);
            goto done;
        }
    }
    {
        // (the picked folder can't be inside Savage Wind's own copy: setting up again deletes that first)
        mkdir(g_root.c_str(), 0755);
        if (path_inside(exefs, g_root) || path_inside(romfs, g_root)) {
            // Choosing the copy itself (a portable folder whose extracted/ is the player's own, #590): nothing to copy,
            // only the game code to build, as Set up does with no folder chosen.
            if (!path_inside(exefs, g_root + "/exefs") || !path_inside(romfs, g_root + "/romfs") ||
                !check_copied_files(g_root).empty()) {
                set_job("That folder is inside Savage Wind's game files", true,
                        "Setting up again replaces those files. Move the folder you extracted the game to out of there, "
                        "or choose another copy.");
                goto done;
            }
            bool dlc_in_place = false;  // (setup_dlc deletes the aoc folders first: never from themselves)
            for (auto &d : dlc) dlc_in_place |= path_inside(d, g_root);
            g_copy_skipped = true;
            if (!dlc.empty() && !dlc_in_place) {
                g_total = dlc_bytes(dlc);
                set_job("Setting up the downloadable content...");
                if (!setup_dlc(dlc)) { set_failure(FAIL_COPY); goto done; }
            }
            if (!build_game_code()) goto done;
            goto check;
        }
        rt_remove_game_files(g_root.c_str());  // (an earlier or interrupted setup's; removes the folder too)
        mkdir(g_root.c_str(), 0755);
        unsigned long long rbytes = romfs_dir_bytes(romfs.c_str());
        g_total = rbytes;
        set_job("Copying the game files...");
        if (!romfs_copy_tree(exefs.c_str(), (g_root + "/exefs").c_str(), NULL) ||
            !romfs_copy_tree(romfs.c_str(), (g_root + "/romfs").c_str(), setup_progress)) {
            set_failure(FAIL_COPY);
            goto done;
        }
        if (!dlc.empty()) {
            set_job("Setting up the downloadable content...");
            if (!setup_dlc(dlc)) { set_failure(FAIL_COPY); goto done; }
        }
        if (!build_game_code()) goto done;
    }
check:
    {
        std::string why = check_game_files(g_root);
#ifndef __APPLE__
        if (why.empty() && !dlc_only && (g_settings.precompile_shaders || new_version) && shader_precompile_available())
            run_precompile();  // (a new version: always, the caches were emptied at its start)
#endif
        if (why.empty()) post_update_finish(SAVAGEWIND_VERSION);
        RtDlcCheck dc = rt_dlc_check(g_root.c_str());
        pthread_mutex_lock(&g_lock);
        g_dlc = dc;
        pthread_mutex_unlock(&g_lock);
        dlc_log(dc);
        if (!why.empty())
            set_failure(why.find("NSO") != std::string::npos ? FAIL_VERSION
                        : version_failure(why) ? version_failure(why) : FAIL_MISSING);
        else if (dlc_only && dc.state != RT_DLC_READY) {
            std::string head, hint;
            dlc_problem_text(dc, &head, &hint);
            set_job(head, true, hint);
        } else
            set_job(dlc_only ? "Downloadable content ready." : "Game files ready.");
    }
done:
    pthread_mutex_lock(&g_lock);
    if (g_job_failed) {
        g_stay_on_setup = true;
        if (new_version) post_update_mark_failed(SAVAGEWIND_VERSION);
    }
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// ------------------------------------------------------------------ command line (runtime/cli.h: savagewind <command>)
// What the start screen does, without a window, for scripts and other launchers. json: one JSON object per line on
// stdout (progress, then the result); otherwise lines on stdout in the menu language. The exit status says whether it
// worked.
static void cli_str(const std::string &s) { cli_json_str(cli_out, s.c_str()); }

// The setup's stage now: copy (with bytes), build (the game code) or shaders
static const char *setup_stage(void) {
#ifndef __APPLE__
    if (g_precompiling) return "shaders";
#endif
    return g_build_frac >= 0 ? "build" : "copy";
}

// savagewind setup [<folder>]: sets the game files up from that folder (no folder: builds the game code again from
// the copy already set up), as Set up game files does. 0: the game files are ready; 130: stopped (Ctrl+C). Stopping
// leaves an unfinished copy that status reports and the next setup replaces; quitting ends the compilers (gamebuild.c).
extern "C" int launcher_setup_cli(const char *root, const char *src, bool json) {
    g_root = root;
    saves_tab_set_game_files(root);
    g_src = src ? src : "";
    g_busy = true;
    cli_catch_stop();
    pthread_t t;
    pthread_create(&t, NULL, extract_thread, NULL);
    std::string last_job;
    int last_pct = -1;
    const char *last_stage = "";
    for (bool busy = true; busy;) {
        if (cli_stop_requested()) {
#ifndef __APPLE__
            atomic_store(&g_pc.cancel, true);
#endif
            const char *stopped = tr("Stopped. Set up again to finish.");
            if (json) fprintf(cli_out, "{\"event\":\"done\",\"ok\":false,\"stopped\":true,\"message\":\"Stopped\",\"hint\":\"\"}\n");
            else fprintf(cli_out, "%s\n", stopped);
            fflush(cli_out);
            exit(130);
        }
        pthread_mutex_lock(&g_lock);
        busy = g_busy;
        std::string job = g_job;
        pthread_mutex_unlock(&g_lock);
        int pct = (int)(extract_progress() * 100);
        const char *stage = setup_stage();
        if (busy && (job != last_job || pct != last_pct || stage != last_stage)) {
            if (json) {
                fprintf(cli_out, "{\"event\":\"progress\",\"stage\":\"%s\",\"percent\":%d", stage, pct);
                if (!strcmp(stage, "copy")) fprintf(cli_out, ",\"done\":%llu,\"total\":%llu", (unsigned long long)g_done, (unsigned long long)g_total);
                fprintf(cli_out, ",\"step\":");
                cli_str(job);
                fprintf(cli_out, ",\"text\":");
                cli_str(tr(job.c_str()));
                fprintf(cli_out, "}\n");
            } else if (job != last_job) fprintf(cli_out, "%s\n", tr(job.c_str()));
            fflush(cli_out);
            last_job = job, last_pct = pct, last_stage = stage;
        }
        if (busy) SDL_Delay(250);
    }
    pthread_join(t, NULL);
    std::string why = check_game_files(g_root);
    bool ok = why.empty() && !g_job_failed;
    if (json) {
        fprintf(cli_out, "{\"event\":\"done\",\"ok\":%s,\"message\":", ok ? "true" : "false");
        cli_str(g_job);
        fprintf(cli_out, ",\"hint\":");
        cli_str(g_job_hint);
        fprintf(cli_out, ",\"text\":");
        cli_str(tr(g_job.c_str()));
        fprintf(cli_out, "}\n");
    } else {
        fprintf(cli_out, "%s\n", tr(g_job.c_str()));
        if (!g_job_hint.empty()) fprintf(cli_out, "%s\n", tr(g_job_hint.c_str()));
    }
    fflush(cli_out);
    return ok ? 0 : 1;
}

// savagewind verify: every check there is, in full - the files and their version, every RomFS file and byte, the
// movies, the game code and the DLC read through. 0: all fine.
extern "C" int launcher_verify_cli(const char *root, bool json) {
    g_root = root;
    std::string files = check_copied_files(g_root), id, romfs, version;
    const GameVersion *v = nso_build_id(g_root + "/exefs/main", &id) ? main_version(id) : NULL;
    if (v) version = v->name;
    if (files.empty() && v) romfs = check_romfs(g_root + "/romfs", *v);
    bool movies = files.empty() && movies_complete(g_root + "/romfs");
    bool code = gamelib_present(root);
    RtDlcCheck dc = rt_dlc_verify(root);
    std::string dlc_head, dlc_hint;
    bool dlc_ok = dc.state == RT_DLC_NONE || dc.state == RT_DLC_READY;
    if (!dlc_ok) dlc_problem_text(dc, &dlc_head, &dlc_hint);
    bool ok = files.empty() && v && romfs.empty() && movies && code && dlc_ok;
    if (json) {
        fprintf(cli_out, "{\"ok\":%s,\"version\":", ok ? "true" : "false");
        cli_str(version);
        fprintf(cli_out, ",\"files\":");
        cli_str(files);
        fprintf(cli_out, ",\"romfs\":");
        cli_str(romfs);
        fprintf(cli_out, ",\"movies\":%s,\"game_code\":%s,\"dlc\":\"%s\",\"dlc_problem\":", movies ? "true" : "false",
                code ? "true" : "false", rt_dlc_state_name(dc.state));
        cli_str(dlc_head);
        fprintf(cli_out, "}\n");
    } else {
        // (terminal text: English, as the log; one fprintf a row)
        std::string game = files.empty() ? version : tr(files.c_str());
        std::string dlc = dlc_ok ? rt_dlc_state_name(dc.state) : dlc_head + (dlc_hint.empty() ? "" : " - " + dlc_hint);
        fprintf(cli_out, "%-12s %s: %s\n", "Game files", files.empty() ? "ok" : "PROBLEM", game.c_str());
        if (files.empty()) {
            fprintf(cli_out, "%-12s %s%s%s\n", "RomFS", romfs.empty() ? "ok" : "PROBLEM", romfs.empty() ? "" : ": ", romfs.c_str());
            fprintf(cli_out, "%-12s %s\n", "Movies", movies ? "ok" : "PROBLEM: some are missing");
        }
        fprintf(cli_out, "%-12s %s\n", "Game code", code ? "ok" : "PROBLEM: not built for this version (savagewind setup)");
        fprintf(cli_out, "%-12s %s: %s\n", "DLC", dlc_ok ? "ok" : "PROBLEM", dlc.c_str());
    }
    fflush(cli_out);
    return ok ? 0 : 1;
}

// savagewind status: whether the game files are set up and the game code built, their version and the DLC.
// 0: ready to play.
extern "C" int launcher_status_cli(const char *root, bool json) {
    g_root = root;
    std::string why = check_game_files(g_root), id, version;
    if (nso_build_id(g_root + "/exefs/main", &id))
        if (const GameVersion *v = main_version(id)) version = v->name;
    RtDlcCheck dlc = rt_dlc_check(root);
    const char *dlc_state = rt_dlc_state_name(dlc.state);
    if (json) {
        fprintf(cli_out, "{\"ready\":%s,\"problem\":", why.empty() ? "true" : "false");
        cli_str(why);
        fprintf(cli_out, ",\"game_files\":");
        cli_str(root);
        fprintf(cli_out, ",\"version\":");
        cli_str(version);
        fprintf(cli_out, ",\"main_build_id\":");
        cli_str(id);
        fprintf(cli_out, ",\"dlc\":");
        cli_str(dlc_state);
        fprintf(cli_out, ",\"savage_wind\":");
        cli_str(rt_version());
        fprintf(cli_out, "}\n");
    } else {
        fprintf(cli_out, "Savage Wind %s\n", rt_version());
        fprintf(cli_out, "Game files:  %s\n", root);
        fprintf(cli_out, "Version:     %s\n", version.empty() ? "-" : version.c_str());
        fprintf(cli_out, "DLC:         %s\n", dlc_state);
        fprintf(cli_out, "Ready:       %s\n", why.empty() ? "yes" : why.c_str());
    }
    return why.empty() ? 0 : 1;
}

// savagewind delete --yes: what Delete game files does (the copy and the game code built from it; never the saves).
extern "C" int launcher_delete_cli(const char *root) {
    struct stat st;
    if (stat(root, &st) != 0) {
        fprintf(cli_out, "No game files to delete.\n");
        return 0;
    }
    double gb = rt_game_files_size(root) / 1e9;
    bool ok = rt_remove_game_files(root);
    fprintf(cli_out, ok ? "Deleted the game files (%.1f GB).\n" : "The game files couldn't be deleted (%.1f GB).\n", gb);
    return ok ? 0 : 1;
}

// ------------------------------------------------------------------ folder dialog (SDL calls back on any thread)
static std::string trim_folder_path(const char *s) {
    std::string p = s ? s : "";
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t' || p.front() == '\r' || p.front() == '\n')) p.erase(p.begin());
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t' || p.back() == '\r' || p.back() == '\n')) p.pop_back();
    if (p.size() >= 2 && ((p.front() == '"' && p.back() == '"') || (p.front() == '\'' && p.back() == '\'')))
        p = p.substr(1, p.size() - 2);
    return p;
}

static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files) {
        const char *err = SDL_GetError();
        if (err && err[0]) fprintf(stderr, "[launcher] folder dialog error: %s\n", err);
        set_job("Folder picker failed to open", true, "Type or paste the folder path below instead.");
        return;
    }
    if (!files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_src = files[0];
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ the user's own Nintendo fonts (not shipped)
// runtime/hle/pl.c loads fonts/ first, else a close substitute (Hiragino Sans). Import copies recognized files only.
static const char *FONT_STD[] = {"nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_std_003.ttf",
                                 "nintendo_udsgr_std_003.ttf", "nintendo_udsgr_std_003.bfttf"};
static const char *FONT_EXT[] = {"nintendo_ext_003.bfttf", "nintendo_ext_003.ttf"};
static std::vector<std::string> g_font_picks;
static std::string g_font_msg;

static bool have_font(const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (file_exists(std::string("fonts/") + names[i])) return true;
    return false;
}

static void SDLCALL picked_fonts(void *, const char *const *files, int) {
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) g_font_picks.push_back(files[i]);
    pthread_mutex_unlock(&g_lock);
}

static void import_fonts() {
    pthread_mutex_lock(&g_lock);
    std::vector<std::string> picks;
    picks.swap(g_font_picks);
    pthread_mutex_unlock(&g_lock);
    if (picks.empty()) return;
    mkdir("fonts", 0755);
    int ok = 0, skipped = 0;
    for (auto &src : picks) {
        // (the file dialog's paths have backslashes on Windows: the name is what follows either kind of separator)
        std::string base = src.substr(src.find_last_of("/\\") + 1), low = base;
        for (auto &c : low) c = (char)tolower(c);
        bool known = false;
        for (auto n : FONT_STD) known |= low == n;
        for (auto n : FONT_EXT) known |= low == n;
        if (!known) { skipped++; continue; }
        FILE *in = fopen(src.c_str(), "rb"), *out = fopen(("fonts/" + low).c_str(), "wb");
        if (in && out) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
            ok++;
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    char m[300];
    if (skipped) snprintf(m, sizeof m, tr("%d font file(s) imported, %d not recognized. Used from the next game start."), ok, skipped);
    else snprintf(m, sizeof m, tr("%d font file(s) imported. Used from the next game start."), ok);
    g_font_msg = m;
}

static void remove_fonts() {
    for (auto n : FONT_STD) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_EXT) unlink((std::string("fonts/") + n).c_str());
    g_font_msg = tr("Imported fonts removed: the substitute font is used.");
}

// ------------------------------------------------------------------ screen
static ImFont *g_font_title, *g_font_menu, *g_font_small;
enum Page { MAIN, SETTINGS, SETUP };  // SETTINGS: the in-game menu's tabs (runtime/ui/menu.mm)
static int g_refocus = MAIN;  // back on the main page: select the item of the page we came from (controllers)

// A big menu line: gold + a marker when focused / hovered (keyboard, gamepad or mouse), grey when disabled
static bool g_menu_small;  // sub pages: buttons in the small font
void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float));  // menu.mm
void menu_tabs_focus(bool first_tab);
extern int g_tab_request_ext;
extern "C" void menu_apply_window_requests(void *sdl_window);
static float g_menu_gap = 0.35f;  // space under each item, in line heights

// The first PLAY ever (settings.menu_tip_seen): a note on where the settings are in game (the game's own Options
// screen, runtime/ui/game_options.mm), OK starts the game. Returns true once OK is pressed.
static bool g_tip_test;  // (SAVAGEWIND_LAUNCHER_SHOT=<frame>:tip)
static bool menu_tip(float s) {
    bool go = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.04f, 0.05f, 0.94f));
    bool open = ImGui::BeginPopupModal("##menutip", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetWindowFontScale(1.25f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(tr("Savage Wind's video, audio and controls settings are in the game's own Options."));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr(pc_cursor_enabled()
            ? "Open them from the title screen, or pause the game and choose System > Options. Esc opens pause or goes back. F2 frees the mouse cursor."
            : "Open them from the title screen, or pause the game and choose System > Options. Esc frees the mouse cursor."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 180 * s) * 0.5f);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(tr("OK"), ImVec2(180 * s, 44 * s)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
            g_settings.menu_tip_seen = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            go = true;
        }
        ImGui::EndPopup();
    }
    return go;
}

// Start-screen text: a soft drop shadow, then the text with a top-to-bottom colour gradient (top == bottom: flat)
static void text_fx(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 top, ImU32 bottom, const char *t,
                    float shadow) {
    if (shadow > 0)
        for (float d : {shadow, shadow * 0.5f}) dl->AddText(font, size, ImVec2(p.x + d, p.y + d), IM_COL32(4, 8, 30, 110), t);
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, p, top, t);
    if (top == bottom) return;
    float y0 = p.y + size * 0.2f, y1 = p.y + size * 0.85f;
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        float f = (dl->VtxBuffer[i].pos.y - y0) / (y1 - y0);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
        dl->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f,
                                                                     a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f));
    }
}

// A four-point sparkle (PLAY's twinkle): k = 0..1 grows and fades it
static void sparkle(ImDrawList *dl, ImVec2 c, float r, float k, float spin) {
    if (k <= 0) return;
    ImVec2 pts[8];
    for (int i = 0; i < 8; i++) {
        float a = spin + i * 3.14159265f / 4, rr = (i & 1 ? r * 0.22f : r) * k;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    dl->AddCircleFilled(c, r * 0.55f * k, IM_COL32(255, 230, 150, (int)(60 * k)));  // soft glow
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(255, 248, 215, (int)(235 * k)));
}

// split .. split_end: that part of the label in its own colour
static bool menu_item(const char *label, bool enabled, bool *hovered = NULL, const char *split = NULL, ImU32 col2 = 0,
                      const char *split_end = NULL) {
    ImGui::PushFont(g_menu_small ? g_font_small : g_font_menu);
    ImGui::BeginDisabled(!enabled);
    ImVec2 at = ImGui::GetCursorScreenPos();
    // (the ID from the label's fixed part: a label that changes would get a new ID, and lose the selection)
    ImGui::PushID(label, split ? split : label + strlen(label));
    if (!split) label = tr(label);  // (shown in the menu language; split labels are built by the caller)
    ImVec2 sz = ImGui::CalcTextSize(label);
    // (EnableNav: otherwise an InvisibleButton can't be reached with the d-pad / arrow keys - ImGui 1.91)
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the hover band shows the selection, no box)
    bool clicked = ImGui::InvisibleButton("##b", ImVec2(sz.x + 40, sz.y + 6), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered) *hovered = hot;
    ImU32 col = !enabled ? IM_COL32(170, 175, 200, 140) : hot ? IM_COL32(255, 214, 64, 255) : IM_COL32(250, 250, 255, 255);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && enabled) {  // a shadow band behind the item (like the version corner's), feathered, with a warm glow
        float x0 = ImGui::GetWindowPos().x, x1 = at.x + sz.x * 0.75f, x2 = at.x + sz.x + sz.y * 3.5f;
        float f = sz.y * 0.22f, y0 = at.y - f * 0.4f, y1 = y0 + f, y3 = at.y + sz.y + f * 0.2f, y2 = y3 - f;
        ImU32 c = IM_COL32(6, 10, 34, 150), g = IM_COL32(255, 190, 80, 34), n = IM_COL32(6, 10, 34, 0), ng = IM_COL32(255, 190, 80, 0);
        for (int pass = 0; pass < 2; pass++) {  // dark band, then the glow over it
            ImU32 a = pass ? g : c, z = pass ? ng : n;
            dl->AddRectFilledMultiColor(ImVec2(x0, y1), ImVec2(x1, y2), a, a, a, a);
            dl->AddRectFilledMultiColor(ImVec2(x1, y1), ImVec2(x2, y2), a, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), z, z, a, a);  // (feathered top / bottom)
            dl->AddRectFilledMultiColor(ImVec2(x1, y0), ImVec2(x2, y1), z, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y2), ImVec2(x1, y3), a, a, z, z);
            dl->AddRectFilledMultiColor(ImVec2(x1, y2), ImVec2(x2, y3), a, z, z, z);
        }
    }
    ImU32 col_b = hot && enabled ? IM_COL32(255, 160, 50, 255) : col;
    if (split && enabled) {
        std::string head(label, split - label);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, head.c_str(), sz.y * 0.05f);
        std::string mid = split_end ? std::string(split, split_end - split) : std::string(split);
        ImVec2 p2(at.x + ImGui::CalcTextSize(label, split).x, at.y);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), p2, col2, col2, mid.c_str(), sz.y * 0.05f);
        if (split_end)
            text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + ImGui::CalcTextSize(label, split_end).x, at.y),
                    col, col, split_end, sz.y * 0.05f);
    } else
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, label, enabled ? sz.y * 0.05f : 0);
    if (g_menu_gap >= 0) ImGui::Dummy(ImVec2(0, sz.y * g_menu_gap));  // line spacing
    else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sz.y * g_menu_gap);  // (tighter than the font's line height)
    ImGui::EndDisabled();
    ImGui::PopFont();
    return clicked;
}

// Controllers, as in the in-game menu (runtime/ui/menu.mm): read straight from SDL, Nintendo layout - the right face
// button (A) confirms, the bottom one (B) goes back; d-pad / left stick move. (ImGui's own SDL gamepad handling is
// switched off: it used the Xbox layout and, per the user, didn't work on the start screen.)
static ImGuiKey launcher_pad_key(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // A: activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // B: back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

static void launcher_pad_open_all(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

// true when the event was a controller event (handled here)
static bool launcher_pad_event(const SDL_Event &e) {
    ImGuiIO &io = ImGui::GetIO();
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        SDL_OpenGamepad(e.gdevice.which);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey k = launcher_pad_key(e.gbutton.button);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e.gaxis.value / 32767.0f;
        // the left stick also works as the d-pad (ImGui moves between items with the d-pad only; the stick scrolls):
        // pressed past half way, released under 0.3
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            static bool held[4];  // left, right, up, down
            bool x = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            int neg = x ? 0 : 2, pos = neg + 1;
            const ImGuiKey keys[4] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                                      ImGuiKey_GamepadDpadDown};
            for (int i : {neg, pos}) {
                float m = i == neg ? -v : v;
                bool now = held[i] ? m > 0.3f : m > 0.5f;
                if (now != held[i]) { held[i] = now; io.AddKeyEvent(keys[i], now); }
            }
        }
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.3f, v > 0.3f ? v : 0);
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.3f, v > 0.3f ? v : 0);
        }
        return true;
    }
    default: return false;
    }
}

// Settings' first tab, on the start screen only: the extracted game files (size, delete) and the user's own Switch
// fonts (import / back to the substitute)
// Open a folder in Finder / Explorer (relative paths are in the data root, the working directory); also the Saves tab's
void reveal_folder(const std::string &dir) {
    std::string path = dir;
    bool abs = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!abs) path = std::string(rt_data_root()) + "/" + path;
    std::string url = "file://";
    if (path[0] != '/') url += "/";  // (Windows: file:///C:/...)
    for (char ch : path) {
        if (ch == '\\') ch = '/';
        if (ch == ' ') url += "%20";
        else if (ch == '%') url += "%25";
        else if (ch == '#') url += "%23";
        else url += ch;
    }
    SDL_OpenURL(url.c_str());
}

static SDL_Window *g_launcher_win;
static bool g_files_deleted;  // set here: the start screen goes back to setting the game files up
static void prelaunch_tab(float sc) {
    const ImVec4 gold(0.93f, 0.76f, 0.33f, 1);
    auto heading = [&](const char *t) {
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(gold, "%s", tr(t));
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
    };
    heading("Game files");
    FileSizeCache::Value size = g_file_size.get();
#ifdef _WIN32
    if (ImGui::Button(tr("Show in Explorer"))) reveal_folder(g_root);
#elif defined(__linux__)
    if (ImGui::Button(tr("Open folder"))) reveal_folder(g_root);
#else
    if (ImGui::Button(tr("Show in Finder"))) reveal_folder(g_root);
#endif
    ImGui::SameLine();
    static bool shot_del = getenv("SAVAGEWIND_LAUNCHER_SHOT") && strstr(getenv("SAVAGEWIND_LAUNCHER_SHOT"), ":askdelete");
    if (ImGui::Button(tr("Delete game files")) || shot_del) ImGui::OpenPopup("##delfiles"), shot_del = false;  // (test: :askdelete)
    if (ImGui::BeginPopupModal("##delfiles", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "%s", tr("Are you sure?"));
        ImGui::PopFont();
        if (size.state == FileSizeCache::State::Ready)
            ImGui::Text(tr("This deletes Savage Wind's copy of the game files (%.1f GB)."), size.bytes / 1e9);
        else {
            ImGui::TextUnformatted(tr("This deletes Savage Wind's copy of the game files."));
            ImGui::TextDisabled("%s", tr(size.state == FileSizeCache::State::Pending ? "Calculating..." : "Unavailable"));
        }
        ImGui::TextDisabled("%s", tr("Saves and settings are kept. Your own game files aren't touched."));
        if (rt_dev_mode()) {  // (a source checkout: the files are the project's own extracted/)
            bool abs = !g_root.empty() && (g_root[0] == '/' || (g_root.size() > 1 && g_root[1] == ':'));
            std::string where = abs ? g_root : std::string(rt_data_root()) + "/" + g_root;
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), tr("Dev checkout: this is %s"), where.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button(tr("Delete"), ImVec2(220 * sc, 0))) {
            g_file_size.invalidate();
            bool ok = rt_remove_game_files(g_root.c_str());  // (runtime/paths.c: only what the extraction made)
            set_job(ok ? "Game files deleted." : "Could not delete the game files (not in Savage Wind's data folder?).", !ok);
            g_files_deleted = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(tr("Cancel"), ImVec2(220 * sc, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (gamelib_compat()) {  // (runtime/gamelib.h: built for CPUs without AVX2 / FMA)
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Game code: compatible mode (this CPU has no AVX2 / FMA), slower"));
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Problem report");
    {
        static std::string made;
        if (ImGui::Button(tr("Make a problem report"))) {
            made = problem_report_make();  // (runtime/problem_report.c)
            if (!made.empty()) reveal_folder(rt_data_root());  // (the folder that holds the zip)
        }
        ImGui::PushTextWrapPos(0);
        if (made.empty())
            ImGui::TextDisabled("%s", tr("A zip file to attach to a bug report: the log of the last two runs, your settings and "
                                         "a short system summary. No game files and no saves."));
        else ImGui::Text(tr("Made %s in the data folder. Attach this file to your report."), made.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Game font");
    import_fonts();
    bool fs = have_font(FONT_STD, 4), fe = have_font(FONT_EXT, 2);
    ImGui::Text(tr("Text: %s    Button icons: %s"), tr(fs ? "Nintendo font (imported)" : "substitute (Hiragino Sans)"),
                tr(fe ? "imported" : "substitute"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Fonts aren't included. For the original look, dump them from your own Switch and import: "
                                 "nintendo_udsg-r_std_003.bfttf (text) and nintendo_ext_003.bfttf (button icons). "
                                 ".ttf versions with the same names work too."));
    ImGui::PopTextWrapPos();
    if (!g_font_msg.empty()) ImGui::Text("%s", g_font_msg.c_str());
    static const SDL_DialogFileFilter font_f[] = {{"Switch fonts", "bfttf;ttf"}};
    if (ImGui::Button(tr("Import fonts"))) SDL_ShowOpenFileDialog(picked_fonts, NULL, g_launcher_win, font_f, 1, NULL, true);
    if (fs || fe) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Use substitute font"))) remove_fonts();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Launch options");
    launch_flags_ui(sc);  // (runtime/ui/launch_flags_ui.mm)
}

// The start screen's fonts, with the menu language's glyphs that Latin fonts lack merged in (runtime/ui/i18n.mm); made
// again when the menu language changes.
static int g_fonts_lang = -1;
static void launcher_fonts(void) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fonts_lang = ui_lang_generation() * 10007 + ui_dynamic_generation();
    g_font_small = g_font_menu = g_font_title = NULL;
    // Fira Sans (OFL, shipped: third_party/fonts/FiraSans) - a clean humanist sans, like the game's own menu text:
    // SemiBold for the title and the menu, Medium for the rest. It has Cyrillic too (Russian menus).
    char semi[1024], med[1024];
    std::string own = rt_resource("FiraSans-SemiBold.ttf", "third_party/fonts/FiraSans/FiraSans-SemiBold.ttf", semi,
                                  sizeof semi) ? semi : "";
    std::string own_small = rt_resource("FiraSans-Medium.ttf", "third_party/fonts/FiraSans/FiraSans-Medium.ttf", med,
                                        sizeof med) ? med : own;
    const char *test = getenv("SAVAGEWIND_LAUNCHER_FONT");  // (test) another typeface, for both
    const char *fonts[] = {test ? test : "", own.c_str(), "fonts/nintendo_udsgr_std_003.ttf",
                           "/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf"};
    static const ImWchar latin[] = {0x20, 0xFF, 0x2039, 0x203A, 0x2192, 0x2192, 0x2713, 0x2713, 0x25B8, 0x25B8, 0};  // + ‹ › → ✓ ▸
    static const ImWchar cyrillic[] = {0x20, 0xFF, 0x400, 0x4FF, 0x2039, 0x203A, 0x2192, 0x2192, 0x2713, 0x2713, 0x25B8,
                                       0x25B8, 0};
    const ImWchar *ranges = !strcmp(ui_lang(), "ru") ? cyrillic : latin;
    auto add = [&](const char *p, float size, const ImWchar *r) {
        ImFont *f = io.Fonts->AddFontFromFileTTF(p, size, NULL, r);
        if (f) ui_merge_fallback_font(io.Fonts, size, NULL);
        return f;
    };
    for (const char *p : fonts) {
        if (g_font_menu || !*p || !file_exists(p)) continue;
        // (Fira's capitals are taller for its size than the system fonts': its sizes match theirs on screen)
        bool fira = p == own.c_str() && !test;
        g_font_small = add(fira ? own_small.c_str() : p, 30 * (fira ? 1.3f : 1), ranges);
        g_font_menu = add(p, 52 * (fira ? 1.3f : 1), ranges);
        g_font_title = add(p, 92 * (fira ? 1.6f : 1), latin);
    }
    if (!g_font_menu) g_font_small = g_font_menu = g_font_title = io.Fonts->AddFontDefault();
}

extern "C" void vk_imgui_fonts_changed(void);  // (runtime/nvn/vulkan/imgui_glue.cpp)
static void launcher_fonts_follow_language(void) {
    if (g_fonts_lang == ui_lang_generation() * 10007 + ui_dynamic_generation()) return;
    launcher_fonts();
#ifdef __APPLE__
    ImGui_ImplMetal_DestroyFontsTexture();  // (NewFrame makes it again)
#else
    vk_imgui_fonts_changed();
#endif
}

// The start screen's shadow banner, fading in from the left behind its menu (and the boot picture's title)
static void left_shade(ImDrawList *bg, ImVec2 ds) {
    const float stops[][2] = {{0.0f, 225}, {0.3f, 215}, {0.44f, 165}, {0.55f, 95}, {0.64f, 35}, {0.74f, 0}};
    for (int i = 0; i + 1 < (int)(sizeof stops / sizeof stops[0]); i++) {
        ImU32 a = IM_COL32(6, 10, 34, (int)stops[i][1]), b = IM_COL32(6, 10, 34, (int)stops[i + 1][1]);
        bg->AddRectFilledMultiColor(ImVec2(ds.x * stops[i][0], 0), ImVec2(ds.x * stops[i + 1][0], ds.y), a, b, b, a);
    }
}

#ifndef __APPLE__
// ---- the boot picture (vulkan/device.c boot_splash_start): the start screen's background, moving as it does there,
// with the name, from the game's start until its title screen is up. Without it the window is black for ~11 s on a
// plain start: ~5 s of Savage Wind loading (the game code, the RomFS image) with nothing to draw, then ~6 s of the
// game's own black frames while it loads the title's world (on the console its startup logo covers those). Its own
// ImGui context, drawn from whichever thread presents at the time, always with the GPU lock held.
static ImGuiContext *g_boot_ctx;
static ImFont *g_boot_font;
static uint64_t g_boot_t0, g_boot_last;

extern "C" void boot_splash_open(void) {
    if (g_boot_ctx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    g_boot_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_boot_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    // only the name's font (the start screen's title font, launcher_fonts): all of the start screen's, with every
    // language's fallbacks, took ~2 s to make, with nothing on screen yet
    char semi[1024];
    g_boot_font = NULL;
    if (rt_resource("FiraSans-SemiBold.ttf", "third_party/fonts/FiraSans/FiraSans-SemiBold.ttf", semi, sizeof semi))
        g_boot_font = io.Fonts->AddFontFromFileTTF(semi, 92 * 1.6f);
    if (!g_boot_font) g_boot_font = io.Fonts->AddFontDefault();
    vk_imgui_backend_init();
    splash_load();
    g_boot_t0 = SDL_GetTicksNS(), g_boot_last = 0;
    ImGui::SetCurrentContext(prev);
}

extern "C" bool boot_splash_active(void) { return g_boot_ctx != NULL; }

// One picture into the window image vk_frame_begin began (cmd: vk_frame_cmd), over what is there at `alpha` (1: all
// of it; less while it fades into the game's title screen)
extern "C" void boot_splash_draw(void *cmd, uint32_t w, uint32_t h, float alpha) {
    if (!g_boot_ctx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_boot_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    uint64_t now = SDL_GetTicksNS();
    io.DeltaTime = g_boot_last && now > g_boot_last ? (float)((now - g_boot_last) / 1e9) : 1.0f / 60;
    g_boot_last = now;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ImVec2 ds = io.DisplaySize;
    float s = ds.y / 720.0f;  // (the start screen's layout scale)
    io.FontGlobalScale = s * 0.5f;
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    splash_draw(bg, ds, (float)((now - g_boot_t0) / 1e9));
    left_shade(bg, ds);
    // the name where the start screen has it, gold to orange, shadowed
    ImGui::PushFont(g_boot_font);
    float fs = ImGui::GetFontSize();
    ImGui::PopFont();
    text_fx(bg, g_boot_font, fs, ImVec2(90 * s, 120 * s), IM_COL32(255, 232, 120, 255), IM_COL32(255, 138, 48, 255),
            "Savage Wind", 3 * s);
    if (alpha < 1) {  // (all of it at once: every vertex's alpha)
        int a = (int)(alpha * 256);
        for (ImDrawVert &v : bg->VtxBuffer) {
            unsigned va = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
            v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)((va * (unsigned)(a > 0 ? a : 0)) >> 8) << IM_COL32_A_SHIFT);
        }
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
    ImGui::SetCurrentContext(prev);
}

// Its textures and ImGui's Vulkan data (device idle: vk_imgui_backend_shutdown), and the context
extern "C" void boot_splash_close(void) {
    if (!g_boot_ctx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_boot_ctx);
    splash_unload();
    vk_imgui_backend_shutdown();
    ImGui::DestroyContext(g_boot_ctx);
    ImGui::SetCurrentContext(prev == g_boot_ctx ? NULL : prev);
    g_boot_ctx = NULL;
}
#endif

// mode (runtime/main.c): 0 setup - the game files page alone, the game starts once they're ready; 1 configuration.
// Returns when the game is to start; QUIT (and closing the setup page) exits.
static bool g_setup_only;
extern "C" void launcher_run(const char *root, int mode) {
    g_root = root;
    saves_tab_set_game_files(root);
    g_file_size.reset(g_root);
    g_setup_only = mode == 0;
    ui_window_init();
    SDL_Window *win = ui_window();
    g_launcher_win = win;
    SDL_SetWindowTitle(win, g_setup_only ? "Savage Wind" : "Savage Wind Configuration");
#ifdef __APPLE__
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
#endif

    IMGUI_CHECKVERSION();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // the in-game menu's, when back here after a reboot
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    // fonts belong to this context: back here via "Title screen" (in-place reboot) the previous run's are gone with
    // its context, so load them again (a stale pointer here aborted in PushFont - user, 0.1 RC)
    launcher_fonts();
    ImGuiStyle &st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.78f, 0.35f, 1);
    st.FrameRounding = st.WindowRounding = 10;
#ifdef __APPLE__
    ImGui_ImplSDL3_InitForMetal(win);
    ImGui_ImplMetal_Init(dev);
#else
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
#endif
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, NULL, 0);  // (controllers: launcher_pad_event)
    launcher_pad_open_all();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    splash_load();

    remove_old_image(g_root);
    std::string status = check_game_files(g_root);
    refresh_dlc();
    Page page = status.empty() && !g_setup_only ? MAIN : SETUP;
    set_job("");  // (an earlier visit's result)
    // A new version, or game code that doesn't match this one: no button to find, it sets itself up. It doesn't start
    // again by itself after failing: the page says why, and Set up game files retries. Not with a test hook that sets up
    // on its own (two builds at once in the same folder failed each other: a compiler's dependency file read half
    // written).
    bool test_setup =
        getenv("SAVAGEWIND_LAUNCHER_EXTRACT") || getenv("SAVAGEWIND_SETUP_FROM") || getenv("SAVAGEWIND_LAUNCHER_SETUP");
    bool auto_setup = post_update_shipped() && !getenv("SAVAGEWIND_LAUNCHER_SHOT") && !test_setup && check_copied_files(g_root).empty() &&
                      (post_update_pending(SAVAGEWIND_VERSION) || !status.empty()) && !post_update_failed_before(SAVAGEWIND_VERSION);
    if (!auto_setup) explain_status(status);
    bool play = false;
    bool tip_then_play = false;  // (setup mode: the game files just got ready; the first-start note, then the game)
    if (auto_setup) {
        fprintf(stderr, "[launcher] setting up this version by itself (%s)\n", status.empty() ? "new version" : status.c_str());
        page = SETUP;
        g_src.clear();
        g_busy = true;
        pthread_t t;
        pthread_create(&t, NULL, extract_thread, NULL);
        pthread_detach(t);
    }
    if (g_settings.check_updates && !getenv("SAVAGEWIND_NO_UPDATE_CHECK")) update_check_start();
    // SAVAGEWIND_LAUNCHER_SHOT=<frame>[:<page>] (test): save build/launcher.png at that frame and exit; page: settings[:<tab>] / askdelete / setup
    long shot_at = getenv("SAVAGEWIND_LAUNCHER_SHOT") ? atol(getenv("SAVAGEWIND_LAUNCHER_SHOT")) : 0, frame = 0;
    if (shot_at) {
#ifdef __APPLE__
        layer.framebufferOnly = NO;
#endif
        const char *pg = strchr(getenv("SAVAGEWIND_LAUNCHER_SHOT"), ':');
        if (pg && (!strncmp(pg + 1, "settings", 8) || !strcmp(pg + 1, "askdelete"))) page = SETTINGS, menu_tabs_focus(true);
        if (pg && !strcmp(pg + 1, "setup")) page = SETUP;
        if (pg && !strncmp(pg + 1, "fail", 4)) {  // (the failure messages; FAIL_NOT_YET: a version not supported yet)
            for (const GameVersion &v : VERSIONS)
                if (!v.supported && g_files_version.empty()) g_files_version = v.name;
            page = SETUP, set_failure(atoi(pg + 5));
        }
        if (pg && !strncmp(pg + 1, "settings", 8) && pg[9] == ':') g_tab_request_ext = atoi(pg + 10);  // settings:<tab>
        if (pg && !strcmp(pg + 1, "delete")) page = SETUP, g_confirm_delete = true;
        if (pg && !strcmp(pg + 1, "tip")) g_settings.menu_tip_seen = false, g_tip_test = true;
    }
    // SAVAGEWIND_SETUP_FROM=<folder> (test): the game files page, set up from that folder at once (as choosing it and Set up)
    if (const char *from = getenv("SAVAGEWIND_SETUP_FROM")) {
        page = SETUP;
        g_src = from;
        g_busy = true;
        pthread_t t;
        pthread_create(&t, NULL, extract_thread, NULL);
        pthread_detach(t);
    }

    static int runs;  // (test: =once - only the first time, so a "Title screen" reboot lands on the start screen)
    const char *ap = getenv("SAVAGEWIND_LAUNCHER_AUTOPLAY");
    if (ap && status.empty() && !(runs && !strcmp(ap, "once"))) play = true;  // (test: PLAY at once)
    runs++;
    if (const char *t = getenv("SAVAGEWIND_LAUNCHER_EXTRACT")) {  // (test) <folder>: set the game files up, log, exit
        g_src = t;
        g_busy = true;
        extract_thread(NULL);
        fprintf(stderr, "[launcher] setup: %s -> %s\n", g_job.c_str(), check_game_files(g_root).empty() ? "READY" : "NOT READY");
        exit(0);
    }
    if (const char *t = getenv("SAVAGEWIND_LAUNCHER_SETUP")) {  // (test) <folder>: Set up started on the start screen
        g_src = t;
        g_busy = true;
        page = SETUP;
        pthread_t th;
        pthread_create(&th, NULL, extract_thread, NULL);
        pthread_detach(th);
    }
    if (getenv("SAVAGEWIND_LAUNCHER_DELETE")) {  // (test) what DELETE GAME FILES does, logged; then the start screen
        double gb = rt_game_files_size(g_root.c_str()) / 1e9;
        bool ok = rt_remove_game_files(g_root.c_str());
        status = check_game_files(g_root);
        refresh_dlc();
        fprintf(stderr, "[launcher] delete %.1f GB: %s -> %s\n", gb, ok ? "ok" : "refused", status.c_str());
        if (!status.empty()) page = SETUP;
    }
    bool was_busy = false;
    while (!play) {
        menu_apply_window_requests(win);  // (the Video tab's Fullscreen)
        if (g_files_deleted) {  // (Settings > Prelaunch deleted them)
            g_files_deleted = false;
            status = check_game_files(g_root);
            refresh_dlc();
            if (!status.empty()) { bind_capture_cancel(); settings_save(); page = SETUP; }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (page == SETTINGS && bind_capture_event(&e)) continue;  // (waiting for a new binding: controls_ui.mm)
            if (!launcher_pad_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                pthread_mutex_lock(&g_lock);
                bool b = g_busy;
                pthread_mutex_unlock(&g_lock);
                if (!b) refresh_dlc();
            }
        }
        // extraction finished: re-check
        pthread_mutex_lock(&g_lock);
        bool busy = g_busy;
        RtDlcCheck dlc_st = g_dlc;
        pthread_mutex_unlock(&g_lock);
        g_file_size.set_suspended(busy);
        if (was_busy && !busy) {
            status = check_game_files(g_root);
            if (status.empty() && g_setup_only) {  // set up: on into the game (after the first-start note)
                if (g_settings.menu_tip_seen) play = true;
                else tip_then_play = true;
            } else if (status.empty() && !stay_on_setup_finished()) page = MAIN;
        }
        was_busy = busy;

#ifdef __APPLE__
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) { SDL_Delay(16); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.04, 0.05, 0.06, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            launcher_fonts_follow_language();
            ImGui_ImplMetal_NewFrame(rp);
#else
        {
            static const float clear_col[4] = {0.04f, 0.05f, 0.06f, 1};
            if (!vk_frame_begin(clear_col)) { SDL_Delay(16); continue; }
            launcher_fonts_follow_language();
            rec_drain();  // (ImGui uploads its fonts on the queue the recorder thread submits on)
            ImGui_ImplVulkan_NewFrame();
#endif
            ImGui_ImplSDL3_NewFrame();
            // the backend (manual mode, no gamepads of its own) clears HasGamepad every frame, and ImGui then drops the
            // controller presses launcher_pad_event fed in and turns gamepad navigation off: set it again while a
            // controller is connected
            if (SDL_HasGamepad()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            ImGui::NewFrame();

            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;  // layout scale (fonts are built for ~2x)
            ImGui::GetIO().FontGlobalScale = s * 0.5f;
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            splash_draw(bg, ds, (float)ImGui::GetTime());  // (the player's picture, or Savage Wind's own art)
            {  // a shadow banner fading in from the left behind the menu, and one from the right behind the version
                left_shade(bg, ds);
                float y0 = ds.y - 190 * s, y1 = ds.y - 135 * s, y2 = ds.y - 40 * s, y3 = ds.y, x0r = ds.x * 0.5f;
                ImU32 c0 = IM_COL32(6, 10, 34, 0), c1 = IM_COL32(6, 10, 34, 235);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y0), ImVec2(ds.x, y1), c0, c0, c1, c0);  // (feathered top)
                bg->AddRectFilledMultiColor(ImVec2(x0r, y1), ImVec2(ds.x, y2), c0, c1, c1, c0);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y2), ImVec2(ds.x, y3), c0, c1, c0, c0);  // (feathered bottom)
            }

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::SetScrollX(0);  // (the start screen never scrolls; a stray nav move mustn't shift it)
            ImGui::SetScrollY(0);
            float x0 = 90 * s;
            ImGui::SetCursorPos(ImVec2(x0, 120 * s));
            ImGui::PushFont(g_font_title);
            if (page == MAIN) {  // gold to orange, shadowed (not behind the Settings / Game Files windows)
                const char *t = "Savage Wind";
                ImVec2 p = ImGui::GetCursorScreenPos();
                text_fx(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), p, IM_COL32(255, 232, 120, 255),
                        IM_COL32(255, 138, 48, 255), t, 3 * s);
                ImVec2 ts = ImGui::CalcTextSize(t);
                ImGui::Dummy(ts);
                // and what this screen is, under it in the ice blue of the art
                std::string sub = tr_upper(tr("Configuration"));
                ImGui::PushFont(g_font_small);
                ImVec2 sp(p.x + 6 * s, p.y + ts.y * 0.98f);
                text_fx(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize() * 1.15f, sp,
                        IM_COL32(205, 232, 250, 255), IM_COL32(150, 200, 235, 255), sub.c_str(), 1.5f * s);
                ImGui::PopFont();
            }
            ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(x0, 285 * s));
            // typing in a text field / the on-screen keyboard (osk.h): B / Esc are for it, not "back" (from the frame
            // before too, so the Esc that ends the typing doesn't also leave the page)
            static bool typed_before;
            bool typing = typed_before || ui_text_busy();

            char update_version[64], update_msg[512];
            UpdateState update_st = update_check_state(update_version, sizeof update_version, update_msg, sizeof update_msg);
            static std::string update_error;  // (starting SW Update failed)
            if (page == MAIN) {
                {  // the four items close together, the block anchored where QUIT always was (the gap to the status
                   // line stays)
                    ImGui::PushFont(g_font_menu);
                    float h = ImGui::CalcTextSize("A").y, sp = ImGui::GetStyle().ItemSpacing.y;
                    ImGui::PopFont();
                    auto pitch = [&](float gap) { return h + 6 + gap * h + (gap >= 0 ? 2 : 1) * sp; };
                    g_menu_gap = -0.18f;
                    ImGui::SetCursorPosY(285 * s + 3 * pitch(0.35f) - 3 * pitch(g_menu_gap) - (update_st == UPDATE_AVAILABLE ? pitch(g_menu_gap) : 0));
                }
                ImGui::SetCursorPosX(x0);
                static bool focused_once;
                if (!focused_once) { ImGui::SetKeyboardFocusHere(); focused_once = true; }
                ImVec2 play_at = ImGui::GetCursorScreenPos();
                bool pressed = menu_item("PLAY", status.empty());
                if (status.empty()) {  // PLAY twinkles now and then while the game is ready
                    ImGui::PushFont(g_font_menu);
                    float w = ImGui::CalcTextSize(tr("PLAY")).x, h = ImGui::CalcTextSize(tr("PLAY")).y;
                    ImGui::PopFont();
                    float t = (float)ImGui::GetTime(), ph = fmodf(t, 3.6f), ph2 = fmodf(t + 0.45f, 3.6f);
                    float k1 = ph < 0.9f ? sinf(ph / 0.9f * 3.14159265f) : 0, k2 = ph2 < 0.7f ? sinf(ph2 / 0.7f * 3.14159265f) : 0;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    sparkle(dl, ImVec2(play_at.x + w + h * 0.05f, play_at.y + h * 0.22f), h * 0.2f, k1, t * 0.6f);
                    sparkle(dl, ImVec2(play_at.x + w - h * 0.32f, play_at.y + h * 0.08f), h * 0.11f, k2 * 0.8f, -t * 0.8f);
                }
                // first PLAY ever: how to reach our menu, then the game
                if (pressed) {
                    if (!g_settings.menu_tip_seen) ImGui::OpenPopup("##menutip");
                    else play = true;
                }
                if (update_st == UPDATE_AVAILABLE) {  // one click: SW Update takes over and this window goes away
                    ImGui::SetCursorPosX(x0);
                    if (menu_item("UPDATE", !busy) || (!busy && getenv("SAVAGEWIND_LAUNCHER_AUTOUPDATE"))) {  // (test: at once)
                        settings_save();
                        if (update_applies_here()) {
                            page = SETUP;
                            set_job("");
                            g_file_size.set_suspended(true);
                            pthread_mutex_lock(&g_lock);
                            g_busy = true;
                            pthread_mutex_unlock(&g_lock);
                            pthread_t ut;
                            pthread_create(&ut, NULL, update_thread, NULL);
                            pthread_detach(ut);
                        } else if (update_apply_start()) exit(0);
                        else update_error = tr("SW Update could not be started. An antivirus may have removed it.");
                    }
                }
                ImGui::SetCursorPosX(x0);
                if (g_refocus == SETTINGS) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;  // (back from Settings: it
                if (menu_item("SETTINGS", true)) {                                             //  stays selected)
                    static bool opened;
                    menu_tabs_focus(!opened);  // (first time this session: Prelaunch; later: the selected tab)
                    opened = true;
                    page = SETTINGS;
                }
                ImGui::SetCursorPosX(x0);  // (set up, set up again, delete: the game files page)
                if (g_refocus == SETUP) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;
                if (menu_item(status.empty() ? "GAME FILES" : "SET UP GAME FILES", !busy)) refresh_dlc(), page = SETUP;
                ImGui::SetCursorPosX(x0);
                if (menu_item("QUIT", true)) exit(0);
                g_menu_gap = 0.35f;
            } else if (page == SETTINGS) {
                // the in-game menu's tabs (runtime/ui/menu.mm), in a window like the in-game one; tabs that need a
                // loaded game say so
                float sc = ds.y / 1080.0f, w = fminf(ds.x * 0.9f, 1500 * sc), h = ds.y - 2 * 18 * sc;  // (all the height: a slim margin)
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y), IM_COL32(0, 0, 0, 110));
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.08f, 0.96f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##settings", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Settings"));
                ImGui::PopFont();
                bool back;
                {  // close: a drawn X in the corner (B / Esc too)
                    float bx = 44 * sc;
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                    back = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
                    ImGui::PopStyleColor();
                    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                    ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
                    float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * sc);
                    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
                    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
                }
                bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);  // (before the pages run, like the menu)
                menu_tabs_draw(sc, g_font_small, g_font_menu, false, prelaunch_tab);
                int cap_dev;
                bind_capture_active(&cap_dev, NULL, NULL);
                if (!popup && !typing && cap_dev < 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                ImGui::EndChild();
                if (back && !saves_tab_busy()) {  // (an import or a backup is running, or waiting for the player's yes)
                    bind_capture_cancel();
                    settings_save();
                    page = MAIN, g_refocus = SETTINGS;
                }
            } else {
                // SETUP: the game files, in a window like Settings (option rows: runtime/ui/option_rows.h) - choose the
                // folder with the game files, set up; once set up, delete or set up again. Each row's explanation on
                // the right. A card on the left: the background shows beside it. The first start's is the welcome.
                float sc = ds.y / 1080.0f, w = fminf(ds.x * 0.66f, 1320 * sc), h = fminf(ds.y - 2 * 18 * sc, 820 * sc);
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y), IM_COL32(0, 0, 0, 50));
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(ImVec2(wp.x + fmaxf(18 * sc, fminf(ds.x * 0.05f, x0)), wp.y + (ds.y - h) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.08f, 0.10f, 0.93f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##setup", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                bool welcome = g_setup_only && status == "No game files";
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr(welcome ? "Welcome to Savage Wind" : "Game Files"));
                ImGui::PopFont();
                bool back = false;
                if (!busy) {  // close: the drawn X in the corner, as Settings (B / Esc too; setup mode: the X quits)
                    float bx = 44 * sc;
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                    back = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
                    ImGui::PopStyleColor();
                    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                    ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
                    float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * sc);
                    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
                    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
                }
                ImGui::Spacing();
                if (welcome && !busy) {  // what this is about, the first time
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s",
                                       trv("Savage Wind needs your own copy of the game, version %s. Choose the folder "
                                           "with its extracted files and set them up: the game starts when it's done.")
                                           .c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Dummy(ImVec2(0, 10 * sc));
                }

                pthread_mutex_lock(&g_lock);
                std::string src = g_src, job = g_job, job_hint = g_job_hint;
                bool failed = g_job_failed;
                pthread_mutex_unlock(&g_lock);
                bool ready = status.empty() && !busy;  // (set up: the window offers to delete them, or set them up again)
                bool shipped_release = post_update_shipped();
                // chosen: "Selected" with a green check (long paths ran over the label); the path is in the
                // explanation, and choosing again opens the picker again
                std::string src_v = tr(src.empty() ? "Choose..." : "Selected");
                auto check_left_of_value = [&](const std::string &v) {
                    ImVec2 rb = ImGui::GetItemRectMax(), ra = ImGui::GetItemRectMin();
                    float fs = ImGui::GetFontSize(), vw = ImGui::CalcTextSize(v.c_str()).x;
                    float x = rb.x - 16 * sc - vw - fs * 0.95f, cy = (ra.y + rb.y) * 0.5f, u = fs * 0.62f;
                    ImU32 g = IM_COL32(120, 220, 120, 255);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddLine(ImVec2(x, cy), ImVec2(x + u * 0.35f, cy + u * 0.35f), g, fs * 0.11f);
                    dl->AddLine(ImVec2(x + u * 0.35f, cy + u * 0.35f), ImVec2(x + u, cy - u * 0.4f), g, fs * 0.11f);
                };
                std::string src_d = trv("The folder with your own extracted game files: exefs (main, main.npdm, sdk...) and "
                                        "romfs, version %s with the update merged in. The game isn't included with Savage Wind.");
                if (!src.empty()) src_d += std::string("\n\n") + tr("Chosen:") + " " + src;
                // the step that's running / its result, under the rows and in the Set up row's explanation
                const std::string &job_line = job;
                std::string ex_d = tr("Copies the game files Savage Wind needs and recompiles the game from them. This only "
                                      "needs to be done once and may take a while. Your own files aren't changed.");
                if (!job.empty())
                    ex_d += std::string("\n\n") + trv(job) + (job_hint.empty() ? "" : std::string(".\n") + trv(job_hint));
                std::string again_d = tr("Sets the game files up again from the folder chosen above, in place of Savage Wind's "
                                         "copy. With no folder chosen, builds the game code again from Savage Wind's copy. "
                                         "Saves and settings are kept.");

                // the controller's cursor starts on the first row (not the X), a few frames while the window lays out
                static int seen_frame = -10, focus_first;  // (not drawn last frame: the window has just opened)
                if (seen_frame != ImGui::GetFrameCount() - 1) focus_first = 4;
                seen_frame = ImGui::GetFrameCount();
                auto first_row = [&] {
                    if (focus_first > 0 && focus_first--) ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                };
                opt_begin("##files", sc);
                if (ready && !g_setup_only) {
                    // DELETE: frees Savage Wind's copy (runtime/paths.c removes only what the setup made, only inside
                    // the data root). Saves, settings and the user's own files stay.
                    FileSizeCache::Value size = g_file_size.get();
                    char gb[128];
                    if (size.state == FileSizeCache::State::Ready) snprintf(gb, sizeof gb, "%.1f GB", size.bytes / 1e9);
                    else snprintf(gb, sizeof gb, "%s", tr(size.state == FileSizeCache::State::Pending ? "Calculating..." : "Unavailable"));
                    opt_section("Game files");
                    if (!g_confirm_delete) {
                        if (opt_action("Delete game files", gb, "Frees the space Savage Wind's copy of the game files takes. "
                                       "Saves and settings are kept, and your own game files aren't touched: you can "
                                       "set the game files up again from them.", true))
                            g_confirm_delete = true;
                        first_row();
                    } else {
                        if (opt_action("Yes, delete them", gb, "Deletes the extracted game files now.", true)) {
                            g_file_size.invalidate();
                            bool ok = rt_remove_game_files(g_root.c_str());
                            status = check_game_files(g_root);
                            refresh_dlc();
                            dlc_st = g_dlc;
                            if (ok) set_job("Game files deleted.");
                            else set_job("Couldn't delete the game files", true, "They aren't in Savage Wind's data folder, "
                                         "so they were left alone.");
                            g_confirm_delete = false;
                        }
                        if (opt_action("Cancel", "", "Keeps the game files.", true)) g_confirm_delete = false;
                    }
                    if (!g_confirm_delete) {  // the DLC set up (added from the folder chosen below), checking and removing it
                        std::string dlc_v = dlc_value_text(dlc_st);
                        std::string dlc_d = tr("Checks whether The Master Trials and The Champions' Ballad are complete and "
                                               "recognized by the game.");
                        if (dlc_st.state == RT_DLC_READY) {
                            char ok[256];
                            snprintf(ok, sizeof ok, tr("Both DLC are set up (aoc1 + aoc2, version %s). The title screen shows DLC Ver.%s."),
                                     dlc_st.version, dlc_st.version);
                            dlc_d += std::string("\n\n") + ok;
                        } else if (dlc_st.state == RT_DLC_NONE) {
                            dlc_d += std::string("\n\n") + tr("No downloadable content is set up.");
                        } else {
                            std::string head, hint;
                            dlc_problem_text(dlc_st, &head, &hint);
                            dlc_d += "\n\n" + head + ". " + hint;
                        }
                        opt_section("Downloadable content");
                        if (opt_action("Check downloadable content", dlc_v.c_str(), dlc_d.c_str(), true)) {
                            g_file_size.set_suspended(true);
                            pthread_mutex_lock(&g_lock);
                            g_busy = true;
                            pthread_mutex_unlock(&g_lock);
                            g_stay_on_setup = true;
                            pthread_t t;
                            pthread_create(&t, NULL, dlc_check_thread, NULL);
                            pthread_detach(t);
                        }
                        if (dlc_st.state == RT_DLC_READY) check_left_of_value(dlc_v);
                        std::string dl = dlc_folders_list(dlc_st.present_mask);
                        if (dl.empty()) opt_disable_next();
                        if (opt_action("Remove downloadable content", dl.empty() ? tr("Not set up") : dl.c_str(),
                                       tr("The Master Trials and The Champions' Ballad, from your own copy of them. To add "
                                          "them, choose the folder with their extracted RomFS (or the game's folder, if "
                                          "they're in it) below and set up. Removing them keeps your saves."), true)) {
                            g_file_size.invalidate();
                            remove_dlc(g_root);
                            refresh_dlc();
                            set_job("Downloadable content removed.");
                        }
                    }
#ifndef __APPLE__
                    if (!g_confirm_delete && shader_precompile_available()) {  // all the shaders, now (or again)
                        char date[32] = "";
                        int nsh = 0, pst = shader_precompile_status(&nsh, date, sizeof date);
                        std::string pd = tr("Translates and compiles every shader of the game now, so that none stutters the "
                                            "first time it is drawn, also in places never visited. Takes a few minutes and "
                                            "about 1.5 GB of disk.");
                        if (pst) {
                            char sum[200];
                            snprintf(sum, sizeof sum, tr("%d shaders, %s"), nsh, date);
                            pd += std::string("\n\n") + tr("Last time:") + " " + sum;
                        }
                        if (pst == 2) pd += std::string("\n") + tr("Savage Wind's shader translator or the graphics driver has changed "
                                                                       "since: run it again.");
                        opt_section("Shaders");
                        if (opt_action("Precompile shaders", tr(pst == 1 ? "Done" : pst == 2 ? "Run again" : "Not done"), pd.c_str(),
                                       true)) {
                            g_file_size.set_suspended(true);
                            pthread_mutex_lock(&g_lock);
                            g_busy = true;
                            pthread_mutex_unlock(&g_lock);
                            g_pc_only = true;
                            pthread_t t;
                            pthread_create(&t, NULL, precompile_thread, NULL);
                            pthread_detach(t);
                        }
                    }
#endif
                }
                if (!ready || !g_confirm_delete) {  // choose the folder, set up (again: from it, or the game code alone)
                    opt_section(!ready ? "Your copy of the game" : "Set up again");
                    if (busy) opt_disable_next();
                    if (opt_action("Game files folder", src_v.c_str(), src_d.c_str(), true))
                        SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
                    if (!src.empty()) check_left_of_value(src_v);
                    if (busy) opt_disable_next();
                    {
                        static char path_buf[1024];
                        static std::string last_synced_src;
                        if (src != last_synced_src) {
                            snprintf(path_buf, sizeof path_buf, "%s", src.c_str());
                            last_synced_src = src;
                        }
                        if (opt_text("Folder path", path_buf, sizeof path_buf, "Type or paste path...",
                                     "Enter the path to your extracted game files folder directly if the system folder picker does not open.")) {
                            std::string p = trim_folder_path(path_buf);
                            pthread_mutex_lock(&g_lock);
                            g_src = p;
                            pthread_mutex_unlock(&g_lock);
                            last_synced_src = p;
                            snprintf(path_buf, sizeof path_buf, "%s", p.c_str());
                        }
                    }
                    if (!ready) {
                        first_row();
                        opt_section("Set up");
                    }
#ifndef __APPLE__
                    if (shader_precompile_available()) {
                        if (busy) opt_disable_next();
                        {   // the background (ui/splash.mm), from the next start
                            static const char *const pics[START_PICTURE_COUNT] = {"Savage Wind", "A screenshot", "Latest save"};
                            if (opt_choice("Background", &g_settings.start_picture, pics, START_PICTURE_COUNT,
                                           START_PICTURE_SCREENSHOT, "The picture behind this screen and while the game loads. A splash.jpg or splash.png of yours in the data folder always comes first."))
                                settings_save();
                            if (opt_toggle("Loading picture", &g_settings.boot_picture, true, "Shows that picture while the game loads. Turned off, the window stays black until the game's title screen: loading takes just as long."))
                                settings_save();
                        }
                        if (opt_toggle("Precompile all shaders", &g_settings.precompile_shaders, true,
                                       "After the setup, translates and compiles every shader of the game, so that none "
                                       "stutters the first time it is drawn, also in places never visited. A few more "
                                       "minutes and about 1.5 GB of disk. It can be done later too, from the game "
                                       "files page of Savage Wind Configuration."))
                            settings_save();
                    }
                    {   // Savage Wind's own colour look (runtime/color_grade.h): on at the first setup unless the player
                        // chose a LUT before; Options > Colour changes it later. It comes with the Vivid colour preset,
                        // which suits the LUT best (the user's pick); turning the look off puts Natural back from Vivid.
                        const int vivid = 3;  // (color_grade.c g_color_presets)
                        if (!ready && !g_settings.color_lut_set) {
                            snprintf(g_settings.color_lut, sizeof g_settings.color_lut, "%s", COLOR_LUT_RECOMMENDED);
                            g_settings.color_lut_strength = COLOR_LUT_RECOMMENDED_STRENGTH;
                            g_settings.color_lut_set = true;
                            if (color_preset_current() == 0) color_preset_apply(vivid);
                        }
                        bool look = !strcmp(g_settings.color_lut, COLOR_LUT_RECOMMENDED);
                        if (busy) opt_disable_next();
                        if (opt_toggle("Savage Wind colour look", &look, true,
                                       "Recommended. The game's picture with Savage Wind's own colour look. Change it or turn it off any time in the game's Options > "
                                       "Colour (LUT).")) {
                            snprintf(g_settings.color_lut, sizeof g_settings.color_lut, "%s", look ? COLOR_LUT_RECOMMENDED : "");
                            g_settings.color_lut_strength = look ? COLOR_LUT_RECOMMENDED_STRENGTH : 100;
                            if (look && color_preset_current() == 0) color_preset_apply(vivid);
                            else if (!look && color_preset_current() == vivid) color_preset_apply(0);
                            settings_save();
                        }
                    }
#endif
                    if (shipped_release) {  // (runtime/post_update.h)
                        if (opt_toggle("Check for updates", &g_settings.check_updates, true,
                                       "When Savage Wind Configuration opens, asks GitHub if there is a new version and "
                                       "offers it. Nothing is installed until you press Update."))
                            settings_save();
                        if (opt_toggle("Open this screen first", &g_settings.start_in_config, true,
                                       "Starting Savage Wind opens this screen, where updates are offered. Turned off, "
                                       "savagewind.exe goes straight into the game."))
                            settings_save();
                    }
                    if (steam_supported()) {
                        bool sb = g_steam_busy;
                        if (sb || busy) opt_disable_next();
                        if (opt_action("Add to Steam", sb ? tr("Working...") : "", tr("Adds two entries to your Steam library: Savage Wind, which goes straight into the game, and Savage Wind Configuration. Steam closes for a moment and opens again. Do it again if you move this folder."), true)) {
                            g_steam_busy = true;
                            set_job(tr("Add to Steam") + std::string("..."));
                            pthread_t st;
                            pthread_create(&st, NULL, steam_thread, NULL);
                            pthread_detach(st);
                        }
                    }
                    if (busy || (src.empty() && !check_copied_files(g_root).empty())) opt_disable_next();
                    if (opt_action("Set up game files", busy ? tr("Working...") : "", ready ? again_d.c_str() : ex_d.c_str(),
                                   true)) {
                        settings_save();  // (the colour look as chosen above)
                        g_file_size.set_suspended(true);
                        pthread_mutex_lock(&g_lock);
                        g_busy = true;
                        pthread_mutex_unlock(&g_lock);
                        pthread_t t;
                        pthread_create(&t, NULL, extract_thread, NULL);
                        pthread_detach(t);
                    }
                }
                if (failed && !busy && post_update_can_rollback()) {  // (the previous version's files are still there)
                    if (opt_action("Go back to the previous version", "", tr("Puts back the version you had before this update. "
                                   "Your saves and game files are kept."), true)) {
                        settings_save();
                        if (update_rollback_start()) exit(0);
                    }
                }
                if (!job_line.empty()) {  // the current step / result
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(failed ? ImVec4(1, 0.5f, 0.4f, 1) : busy ? ImVec4(0.93f, 0.76f, 0.33f, 1)
                                              : ImVec4(0.45f, 0.95f, 0.55f, 1), "%s", trv(job_line).c_str());
                    if (failed && !job_hint.empty())  // (what to do next, in the rows' own text colour)
                        ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s", trv(job_hint).c_str());
                    ImGui::PopTextWrapPos();
                }
                if (g_setup_only && !busy) {  // where the rest is, later
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
#ifdef _WIN32
                    ImGui::TextDisabled("%s", tr("To change settings or the game files later, open Savage Wind Configuration."));
#elif defined(__APPLE__)
                    ImGui::TextDisabled("%s", tr("To change settings or the game files later, hold Option while you open Savage Wind."));
#else
                    ImGui::TextDisabled("%s", tr("To change settings or the game files later, start Savage Wind with --config."));
#endif
                    ImGui::PopTextWrapPos();
                }
                opt_end();
                if (g_confirm_delete && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    g_confirm_delete = false;  // (B / Esc: back out of the question first)
                else if (!busy && !typing && !g_setup_only && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                                               ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;  // (setup mode has nothing to go back to: only its X closes it)
                ImGui::PopFont();
                ImGui::EndChild();
                if (back && !busy) {
                    settings_save();
                    if (g_setup_only) exit(0);
                    g_confirm_delete = false;
                    refresh_dlc();
                    page = MAIN, g_refocus = SETUP;
                }
                static bool test_busy = getenv("SAVAGEWIND_LAUNCHER_SHOT") && strstr(getenv("SAVAGEWIND_LAUNCHER_SHOT"), ":extracting");
                if (busy || test_busy) {  // extracting: everything dimmed, a progress bar in front (input waits)
                    ImDrawList *fg = ImGui::GetForegroundDrawList();
                    ImVec2 o = ImGui::GetMainViewport()->Pos;
                    fg->AddRectFilled(o, ImVec2(o.x + ds.x, o.y + ds.y), IM_COL32(0, 0, 0, 170));
                    float f = test_busy ? 0.42f : extract_progress();
                    // (while the game code is built: what's happening, on whose files - runtime/gamebuild.c)
                    bool recompiling = g_build_frac >= 0, dlc_check = g_dlc_checking;
                    const char *title = tr(dlc_check ? "Downloadable content"
                                           : recompiling ? "Recompiling the game" : "Setting up the game files");
                    const char *const notes[] = {tr("This only needs to be done once and may take a while."),
                                                 tr("Esc or B stops it: what is done is kept.")};
                    int nnotes = dlc_check ? 0 : recompiling ? 1 : 0;
                    if (update_apply_progress() >= 0) {
                        f = update_apply_progress();
                        title = tr("Updating Savage Wind");
                        nnotes = 0;
                    }
#ifndef __APPLE__
                    bool shaders = g_precompiling;
                    if (shaders) {
                        f = shader_precompile_progress(&g_pc);
                        title = tr("Precompiling shaders");
                        nnotes = 2;
                        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                            atomic_store(&g_pc.cancel, true);
                    }
#endif
                    float tfs = g_font_menu->FontSize * 0.75f, sfs = g_font_small->FontSize * 0.85f;
                    ImVec2 ts = g_font_menu->CalcTextSizeA(tfs, FLT_MAX, 0, title);
                    float line_h = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, "A").y;
                    float pw = fminf(ds.x * 0.6f, 900 * sc), bar_y = 36 * sc + ts.y + 30 * sc;
                    for (int i = 0; i < nnotes; i++)  // (wide enough for its text, within the window)
                        pw = fmaxf(pw, g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, notes[i]).x + 96 * sc);
                    pw = fminf(fmaxf(pw, ts.x + 96 * sc), ds.x - 32 * sc);
                    float ph = bar_y + 24 * sc + 26 * sc + line_h + (nnotes ? 14 * sc + nnotes * line_h : 0) + 40 * sc;
                    ImVec2 a(o.x + (ds.x - pw) * 0.5f, o.y + (ds.y - ph) * 0.5f), b(a.x + pw, a.y + ph);
                    fg->AddRectFilled(a, b, IM_COL32(15, 23, 20, 250), 14 * sc);
                    fg->AddRect(a, b, IM_COL32(236, 194, 84, 140), 14 * sc, 0, 1.5f * sc);
                    fg->AddText(g_font_menu, tfs, ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + 36 * sc), IM_COL32(236, 194, 84, 255), title);
                    ImVec2 ba(a.x + 48 * sc, a.y + bar_y), bb(b.x - 48 * sc, a.y + bar_y + 24 * sc);
                    fg->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, 25), 12 * sc);
                    if (f > 0) fg->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * f, bb.y), IM_COL32(236, 194, 84, 255), 12 * sc);
                    char line[200];
#ifndef __APPLE__
                    std::string step = shaders ? precompile_line() : tr(test_busy ? "Setting up the game files..." : job.c_str());
#else
                    std::string step = tr(test_busy ? "Setting up the game files..." : job.c_str());
#endif
                    if (dlc_check) snprintf(line, sizeof line, "%s", step.c_str());
                    else snprintf(line, sizeof line, "%s   %d%%", step.c_str(), (int)(f * 100));
                    ImVec2 ls = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, line);
                    fg->AddText(g_font_small, sfs, ImVec2((a.x + b.x - ls.x) * 0.5f, bb.y + 26 * sc), IM_COL32(220, 216, 205, 255), line);
                    for (int i = 0; i < nnotes; i++) {
                        ImVec2 ns = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, notes[i]);
                        fg->AddText(g_font_small, sfs, ImVec2((a.x + b.x - ns.x) * 0.5f, bb.y + 26 * sc + (i + 1) * line_h + 14 * sc),
                                    IM_COL32(150, 148, 140, 255), notes[i]);
                    }
                }
            }

            // the first start's note on where the settings are: after PLAY, or in setup mode once the game files are
            // ready; OK starts the game
            if (g_tip_test) ImGui::OpenPopup("##menutip"), g_tip_test = false;
            if (tip_then_play) ImGui::OpenPopup("##menutip"), tip_then_play = false;
            if (menu_tip(s)) play = true;

            // status (lower left, main page) and version (lower right) - not under the Settings window
            if (page != SETTINGS) {
            ImGui::PushFont(g_font_small);
            if (page == MAIN) {
            ImGui::SetCursorPos(ImVec2(x0, ds.y - (status.empty() && dlc_st.state != RT_DLC_NONE ? 140 : 118) * s));
            static std::string main_id;  // (read again after the game files change)
            if (!status.empty()) main_id.clear();
            auto draw_ready_mark = [&] {  // a drawn check mark (the Switch font has no ✓)
                ImVec2 p = ImGui::GetCursorScreenPos();
                float u = ImGui::GetTextLineHeight();
                ImU32 g = IM_COL32(140, 230, 140, 255);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.1f, p.y + u * 0.55f), ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), g, u * 0.12f);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), ImVec2(p.x + u * 0.85f, p.y + u * 0.2f), g, u * 0.12f);
                ImGui::SetCursorPosX(x0 + u * 1.3f);
            };
            if (status.empty()) {
                draw_ready_mark();
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), "%s", tr("Game files ready."));
                ImGui::SetCursorPosX(x0);
                if (main_id.empty() && !nso_build_id(g_root + "/exefs/main", &main_id)) main_id = "?";
                ImGui::TextColored(ImVec4(0.66f, 0.76f, 0.98f, 1), tr("main build id %s"), main_id.c_str());
#ifndef __APPLE__
                {   // the precompiled shaders were made by another translator or driver: the first play stutters until
                    // they are made again (Game Files > Precompile shaders says so too, but only there). Asked once.
                    static int stale = -1;
                    if (stale < 0) {
                        stale = shader_precompile_available() && shader_precompile_status(NULL, NULL, 0) == 2;
                        if (stale) fprintf(stderr, "[launcher] the precompiled shaders are out of date (a new driver or translator)\n");
                    }
                    if (stale) {
                        ImGui::SetCursorPosX(x0);
                        ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "%s", tr("Shaders out of date: see Game Files."));
                    }
                }
#endif
                if (dlc_st.state == RT_DLC_READY) {
                    ImGui::SetCursorPosX(x0);
                    draw_ready_mark();
                    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), tr("DLC %s ready."), dlc_st.version);
                } else if (dlc_st.state != RT_DLC_NONE) {
                    ImGui::SetCursorPosX(x0);
                    ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "!  %s", tr("DLC not ready: see Game Files."));
                }
            } else {
                ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "!  %s", trv(busy ? "Setting up the game files..." : status).c_str());
                ImGui::SetCursorPosX(x0);
                ImGui::TextDisabled("%s", tr("Set up the game files to play."));
            }
            if (update_st == UPDATE_AVAILABLE) {
                ImGui::SetCursorPosX(x0);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), tr("Version %s is available."), update_version);
            } else if (update_st == UPDATE_ERROR || !update_error.empty()) {
                ImGui::SetCursorPosX(x0);
                ImGui::TextDisabled("%s %s", tr("Couldn't check for updates:"), update_error.empty() ? update_msg : update_error.c_str());
            }
            if (const char *f = getenv("SAVAGEWIND_LAUNCHER_FONT")) {  // (test) which typeface this window shows
                const char *b = strrchr(f, '/');
                ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetWindowPos().x + 20 * s, ImGui::GetWindowPos().y + 16 * s),
                                                    IM_COL32(255, 255, 255, 255), b ? b + 1 : f);
            }
            }
            {  // version (lower right), under a shadow banner from the right
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImFont *font = ImGui::GetFont();
                float fs = ImGui::GetFontSize(), right = ImGui::GetWindowPos().x + ds.x - 60 * s;
                char ver[64];
                snprintf(ver, sizeof ver, tr("Version %s"), SAVAGEWIND_VERSION);
                float vs = fs * 1.4f, y = ImGui::GetWindowPos().y + ds.y - 134 * s;
                ImVec2 vsz = font->CalcTextSizeA(vs, FLT_MAX, 0, ver);
                ImVec2 p(right - vsz.x, y);
                for (float d : {2.5f, 1.5f, 0.8f}) dl->AddText(font, vs, ImVec2(p.x + d * s, p.y + d * s), IM_COL32(4, 8, 30, 150), ver);
                dl->AddText(font, vs, p, IM_COL32(255, 255, 255, 255), ver);
            }
            ImGui::PopFont();
            }
            ImGui::End();
            ImGui::PopStyleColor();
            osk_draw(g_font_menu, s * 0.7f);  // (a text field activated with the controller)
            typed_before = ui_text_busy();

            ImGui::Render();
#ifdef __APPLE__
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc endEncoding];
            id<MTLTexture> shot = nil;
            if (shot_at && ++frame == shot_at) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                shot = [dev newTextureWithDescriptor:td];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture toTexture:shot];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (shot) {
                [cb waitUntilCompleted];
                save_png(shot, getenv("SAVAGEWIND_SHOT_PATH") ? getenv("SAVAGEWIND_SHOT_PATH") : "build/launcher.png");
                exit(0);
            }
#else
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            bool shot = shot_at && ++frame == shot_at;
            vk_frame_end(shot ? getenv("SAVAGEWIND_SHOT_PATH") ? getenv("SAVAGEWIND_SHOT_PATH") : "build/launcher.png" : NULL);
            if (shot) exit(0);
#endif
        }
        // At most 60 frames a second, 20 without focus (#131): the present mode is the game's (Video > VSync), and with
        // Fast (mailbox) or Off nothing else held this screen back, so it drew at the display's rate or faster (half a
        // GPU on a 240 Hz display) for a picture that moves slowly, also while the setup builds the game code.
        if (!shot_at) {
            static uint64_t next;
            uint64_t now = plat_now_ns(), period = 1000000000ull / (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS ? 60 : 20);
            if (next > now + period) next = now;  // (focus came back, or the clock jumped)
            if (next > now) plat_sleep_until_ns(next);
            next = (next > now ? next : now) + period;
        }
    }
    // play: cancel the size traversal before handing the window to the game
    g_file_size.stop();
    settings_save();
    SDL_SetWindowTitle(win, "Savage Wind");
    splash_unload();
    saves_tab_unload();
#ifdef __APPLE__
    ImGui_ImplMetal_Shutdown();
#else
    vk_imgui_backend_shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
}
