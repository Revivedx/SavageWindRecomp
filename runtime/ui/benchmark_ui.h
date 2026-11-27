// In-game benchmark menu page and live progress HUD (runtime/ui/benchmark_ui.mm).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void bench_ui_tab(bool *want_close);
void bench_ui_overlay(float display_w, float display_h, float scale);

#ifdef __cplusplus
}
#endif
