// Render without a GPU and exercise cancel, snapshots, result delivery and reboot cleanup.
#include <cassert>
#include "../runtime/ui/amiibo_picker.mm"

extern "C" { Settings g_settings = {}; void input_reset(void) {} }
const char *tr(const char *text) { return text; }
void osk_cancel(void) {}
bool ui_text_busy(void) { return ImGui::GetIO().WantTextInput; }
bool ui_input_text(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags, const char *hint, const char *) {
    return ImGui::InputTextWithHint(label, hint, buf, size, flags);
}
static void frame(void) {
    ImGui::NewFrame();
    amiibo_picker_draw(1);
    ImGui::Render();
}
int main(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
    amiibo_picker_open();
    tab = 1; strcpy(query, "no such figure");
    for (int i = 0; i < 4; i++) frame();
    draft.favorites[AMIIBO_ARCHER] = true;
    close_picker(AMIIBO_ARCHER);
    assert(amiibo_picker_take(&r) && r.selected == AMIIBO_ARCHER && r.favorites[AMIIBO_ARCHER]);
    assert(g_settings.amiibo_figure == AMIIBO_GUARDIAN && !g_settings.amiibo_favorites[AMIIBO_ARCHER]);
    amiibo_picker_open();
    amiibo_picker_reset();
    assert(!amiibo_picker_active() && !amiibo_picker_take(&r));
    ImGui::DestroyContext();
    puts("PASS: amiibo browser rendering, cancel, settings snapshots, result delivery and reset");
}
