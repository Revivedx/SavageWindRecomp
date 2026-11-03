// Render without a GPU and exercise cancel, snapshots, result delivery and reboot cleanup.
#include <cassert>
#include "../runtime/ui/amiibo_picker.mm"

extern "C" { Settings g_settings = {}; void input_reset(void) {} }
static bool translated;
const char *tr(const char *text) {
    if (translated && !strcmp(text, "Choose amiibo")) return "Choisir un amiibo";
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
    io.AddKeyEvent(ImGuiKey_GamepadR1, true); frame();
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    for (int i = 0; i < 3; i++) frame();
    assert(tab == 1 && preview == AMIIBO_MARIO);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, true); frame();
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, false); frame();
    assert(!draft.favorites[AMIIBO_MARIO] && amiibo_picker_active());
    io.AddKeyEvent(ImGuiKey_GamepadL1, true); frame();
    io.AddKeyEvent(ImGuiKey_GamepadL1, false);
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
    amiibo_picker_reset();
    assert(!amiibo_picker_active() && !amiibo_picker_take(&r));
    ImGui::DestroyContext();
    puts("PASS: amiibo browser, controller filters, on-screen search, favorites, result delivery and reset");
}
