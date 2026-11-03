// Render without a GPU and exercise cancel, snapshots, result delivery and reboot cleanup.
#include <cassert>
#include "../runtime/ui/amiibo_picker.mm"

extern "C" { Settings g_settings = {}; void input_reset(void) {} }
static int scan_completions;
static uint64_t completed_generation;
static AmiiboPickerResult completed_scan;
static bool accept_scan = true;
static WolfLinkStatus dump_status = WOLF_DUMP_MISSING;
extern "C" WolfLinkStatus wolf_link_dump_load(WolfLinkDump *result) { memset(result, 0, sizeof *result); return dump_status; }
extern "C" const char *wolf_link_dump_status_text(WolfLinkStatus) { return "Missing amiibo/wolf_link.bin."; }
extern "C" bool nfp_scan_complete(uint64_t generation, const AmiiboPickerResult *result) {
    scan_completions++;
    completed_generation = generation;
    completed_scan = *result;
    if (!accept_scan) dump_status = WOLF_DUMP_MISSING;
    return accept_scan;
}
static bool translated;
const char *tr(const char *text) {
    if (translated && !strcmp(text, "Choose amiibo")) return "Choisir un amiibo";
    if (translated && !strcmp(text, "Search amiibo")) return "Rechercher dans la collection de figurines amiibo";
    return text;
}
#include "../runtime/ui/osk.mm"
static void frame(void) {
    ImGui::NewFrame();
    amiibo_picker_draw(1);
    osk_draw(nullptr, 1);
    ImGui::Render();
}
static void press(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true); frame();
    ImGui::GetIO().AddKeyEvent(key, false);
    for (int i = 0; i < 3; i++) frame();
}
static bool focus_up_to(const char *label) {
    ImGuiWindow *window = ImGui::FindWindowByName("###amiibo-picker");
    ImGuiID id = window->GetID(label);
    if (!strcmp(label, "##amiibo-series")) {
        if (!focus_up_to("##amiibo-search")) return false;
        press(ImGuiKey_GamepadDpadRight);
        return GImGui->NavId == id;
    }
    if (GImGui->NavId == window->GetID("##amiibo-series")) press(ImGuiKey_GamepadDpadLeft);
    for (int i = 0; i < 12 && GImGui->NavId != id; i++) press(ImGuiKey_GamepadDpadUp);
    return GImGui->NavId == id;
}
int main(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    g_settings.amiibo_figure = AMIIBO_GUARDIAN;
    g_settings.amiibo_favorites[AMIIBO_MARIO] = true;
    g_settings.amiibo_recent[0] = AMIIBO_ARCHER;
    g_settings.amiibo_recent_count = 1;
    amiibo_picker_open();
    for (int i = 0; i < 4; i++) frame();
    assert(amiibo_picker_active());
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame();
    assert(!amiibo_picker_active());
    AmiiboPickerResult r;
    assert(amiibo_picker_take(&r) && r.selected == -1 && r.favorites[AMIIBO_MARIO]);
    assert(!amiibo_picker_take(&r));
    io.AddKeyEvent(ImGuiKey_Escape, false);
    amiibo_picker_open();
    for (int i = 0; i < 4; i++) frame();
    io.AddKeyEvent(ImGuiKey_Enter, true); frame();
    io.AddKeyEvent(ImGuiKey_Enter, false); frame();
    assert(!amiibo_picker_active());
    assert(amiibo_picker_take(&r) && r.selected == AMIIBO_GUARDIAN);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    amiibo_picker_open();
    for (int i = 0; i < 4; i++) frame();
    io.AddKeyEvent(ImGuiKey_GamepadR2, true); frame();
    io.AddKeyEvent(ImGuiKey_GamepadR2, false);
    for (int i = 0; i < 3; i++) frame();
    assert(tab == 1 && preview == AMIIBO_MARIO);
    io.AddKeyEvent(ImGuiKey_GamepadBack, true); frame();
    io.AddKeyEvent(ImGuiKey_GamepadBack, false); frame();
    assert(!draft.favorites[AMIIBO_MARIO] && amiibo_picker_active());
    io.AddKeyEvent(ImGuiKey_GamepadL2, true); frame();
    io.AddKeyEvent(ImGuiKey_GamepadL2, false);
    for (int i = 0; i < 3; i++) frame();
    assert(tab == 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true); frame();
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false); frame();
    assert(!amiibo_picker_active());
    assert(amiibo_picker_take(&r) && r.selected == AMIIBO_MARIO && !r.favorites[AMIIBO_MARIO]);
    amiibo_picker_open();
    for (int i = 0; i < 4; i++) frame();
    assert(focus_up_to("##amiibo-series"));
    press(ImGuiKey_GamepadFaceDown);
    assert(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
    press(ImGuiKey_GamepadDpadDown);
    press(ImGuiKey_GamepadFaceDown);
    assert(series == AMIIBO_SERIES_BOTW && amiibo_picker_active());
    assert(focus_up_to("##amiibo-search"));
    press(ImGuiKey_GamepadFaceDown);
    assert(osk_is_open());
    io.AddInputCharactersUTF8("Link"); frame();
    press(ImGuiKey_GamepadStart);
    assert(!osk_is_open() && !strcmp(query, "Link") && amiibo_picker_active());
    press(ImGuiKey_F14);
    assert(!query[0] && amiibo_picker_active());
    int previous_series = series;
    press(ImGuiKey_GamepadR1);
    assert(series == (previous_series + 1) % AMIIBO_SERIES_COUNT);
    press(ImGuiKey_F13);
    assert(osk_is_open());
    press(ImGuiKey_GamepadStart);
    assert(!osk_is_open() && amiibo_picker_active());
    amiibo_picker_reset();
    amiibo_picker_open();
    tab = 1; strcpy(query, "no such figure");
    for (int i = 0; i < 4; i++) frame();
    draft.favorites[AMIIBO_ARCHER] = true;
    close_picker(AMIIBO_ARCHER);
    assert(amiibo_picker_take(&r) && r.selected == AMIIBO_ARCHER && r.favorites[AMIIBO_ARCHER]);
    assert(g_settings.amiibo_figure == AMIIBO_GUARDIAN && !g_settings.amiibo_favorites[AMIIBO_ARCHER]);
    amiibo_picker_open();
    for (int index = 0; index < 4; index++) frame();
    ImGuiWindow *original = ImGui::FindWindowByName("###amiibo-picker");
    assert(original);
    translated = true;
    frame();
    assert(ImGui::FindWindowByName("Choisir un amiibo###amiibo-picker") == original);
    assert(amiibo_picker_active() && g_settings.amiibo_figure == AMIIBO_GUARDIAN);
    AmiiboPickerControls labels;
    labels.confirm = "Circle";
    labels.cancel = "Cross";
    labels.search = "Square";
    labels.clear = "Triangle";
    labels.favorite = "Create";
    labels.series = "L1 / R1";
    labels.tabs = "L2 / R2";
    amiibo_picker_set_controls(labels);
    for (ImVec2 display : {ImVec2(640, 360), ImVec2(800, 450), ImVec2(2560, 1080), ImVec2(720, 960)}) {
        io.DisplaySize = display;
        for (int index = 0; index < 4; index++) frame();
        assert(original->Pos.x == 0 && original->Pos.y == 0);
        assert(original->Size.x == display.x && original->Size.y == display.y);
        assert(original->ScrollMax.y == 0 && original->FontWindowScale == 1);
        assert(GImGui->StyleVarStack.Size == 0 && GImGui->ColorStack.Size == 0);
        for (ImGuiWindow *window : GImGui->Windows) {
            if (!window->Active || window->ParentWindow != original) continue;
            assert(window->Pos.x >= 0 && window->Pos.y >= 0);
            assert(window->Pos.x + window->Size.x <= display.x);
            assert(window->Pos.y + window->Size.y <= display.y);
        }
        assert(amiibo_picker_active() && g_settings.amiibo_figure == AMIIBO_GUARDIAN);
    }
    amiibo_picker_reset();
    assert(!amiibo_picker_active() && !amiibo_picker_take(&r));
    translated = false;
    amiibo_picker_set_controls(AmiiboPickerControls{});
    io.DisplaySize = ImVec2(1280, 720);
    amiibo_picker_open();
    for (int index = 0; index < 4; index++) frame();
    io.AddMousePosEvent(162, 273); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false);
    for (int index = 0; index < 4; index++) frame();
    assert(draft.favorites[AMIIBO_ARCHER] && preview == AMIIBO_ARCHER && amiibo_picker_active());
    io.AddMousePosEvent(320, 273); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    assert(amiibo_picker_active() && preview == AMIIBO_ARCHER);
    press(ImGuiKey_GamepadDpadDown);
    assert(preview == AMIIBO_ZELDA && amiibo_picker_active());
    bool was_favorite = draft.favorites[AMIIBO_ZELDA];
    press(ImGuiKey_F);
    assert(draft.favorites[AMIIBO_ZELDA] != was_favorite);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    frame();
    press(ImGuiKey_F);
    io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
    assert(ui_text_busy());
    io.AddInputCharactersUTF8("no matching amiibo"); frame();
    press(ImGuiKey_Enter);
    assert(amiibo_picker_active());
    ImGuiWindow *picker = ImGui::FindWindowByName("###amiibo-picker");
    ImGuiID confirm = picker->GetID("###amiibo-confirm");
    ImGui::SetFocusID(confirm, picker);
    press(ImGuiKey_Enter);
    assert(amiibo_picker_active() && query[0]);
    press(ImGuiKey_Delete);
    assert(!query[0] && amiibo_picker_active());
    strcpy(query, "no matching amiibo");
    series = AMIIBO_SERIES_SMASH;
    tab = 1;
    for (int index = 0; index < 4; index++) frame();
    io.AddMousePosEvent(200, 406); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false);
    for (int index = 0; index < 4; index++) frame();
    assert(!query[0] && series == 0 && tab == 0 && amiibo_picker_active());
    amiibo_picker_reset();
    amiibo_picker_open();
    for (int index = 0; index < 25; index++) frame();
    io.AddMousePosEvent(320, 273); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    assert(amiibo_picker_active() && preview == AMIIBO_ARCHER);
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    assert(!amiibo_picker_active() && amiibo_picker_take(&r) && r.selected == AMIIBO_ARCHER);
    g_settings.amiibo_figure = AMIIBO_COUNT - 1;
    amiibo_picker_open();
    for (int index = 0; index < 4; index++) frame();
    assert(preview == AMIIBO_COUNT - 1);
    for (int index = 0; index < 20; index++) press(ImGuiKey_GamepadDpadUp);
    assert(preview == AMIIBO_COUNT - 21 && amiibo_picker_active());
    amiibo_picker_reset();
    g_settings.amiibo_figure = AMIIBO_GUARDIAN;
    amiibo_picker_open_scan(42);
    for (int index = 0; index < 4; index++) frame();
    press(ImGuiKey_Enter);
    assert(!amiibo_picker_active() && !amiibo_picker_take(&r));
    assert(scan_completions == 1 && completed_generation == 42 && completed_scan.selected == AMIIBO_GUARDIAN);
    amiibo_picker_open_scan(43);
    for (int index = 0; index < 4; index++) frame();
    press(ImGuiKey_Escape);
    assert(scan_completions == 2 && completed_generation == 43 && completed_scan.selected == -1);
    amiibo_picker_open_scan(44);
    amiibo_picker_cancel_scan();
    frame();
    assert(!amiibo_picker_active() && scan_completions == 2);
    amiibo_picker_open();
    amiibo_picker_cancel_scan();
    assert(amiibo_picker_active());
    amiibo_picker_reset();
    int wolf = -1;
    for (int figure = 0; figure < AMIIBO_COUNT; figure++)
        if (!strcmp(virtual_amiibos[figure].id, "01030000024f0902")) wolf = figure;
    assert(wolf >= 0);
    g_settings.amiibo_figure = wolf;
    g_settings.amiibo_own_wolf = true;
    amiibo_picker_open_scan(45);
    for (int index = 0; index < 4; index++) frame();
    int before = scan_completions;
    press(ImGuiKey_Enter);
    assert(amiibo_picker_active() && scan_completions == before);
    press(ImGuiKey_Escape);
    assert(!amiibo_picker_active() && completed_scan.selected == -1);
    dump_status = WOLF_DUMP_OK;
    accept_scan = false;
    amiibo_picker_open_scan(46);
    for (int index = 0; index < 4; index++) frame();
    press(ImGuiKey_Enter);
    assert(amiibo_picker_active() && wolf_status == WOLF_DUMP_MISSING);
    accept_scan = true;
    press(ImGuiKey_Escape);
    assert(!amiibo_picker_active());
    g_settings.amiibo_own_wolf = false;
    ImGui::DestroyContext();
    puts("PASS: amiibo browser, controller filters, on-screen search, favorites, result delivery and reset");
}
