// Settings pages in two panes (runtime/ui/option_rows.mm): option rows on the left - an uppercase label, the value on
// the right (brighter when it isn't the default) - and the selected (controller) or hovered (mouse) row's description on
// the right, instead of tooltips. A / click changes a row (toggle / next value), left / right step it.
//
//   opt_begin("##video", scale);
//   opt_section("Display");
//   if (opt_toggle("Room reverb", &g_settings.reverb, true, "The room reverb effect.")) settings_save();
//   opt_end();
#pragma once

void opt_begin(const char *id, float scale);
void opt_section(const char *title);
bool opt_toggle(const char *label, bool *v, bool def, const char *desc);
// wrap: past the last value back to the first (and the other way); false: stops at the ends
bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap = true);
// a long choice: A / click opens the values as a list (*picked: one was picked there), left / right step through them
bool opt_list(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool *picked);
// a choice where some values can't be picked yet (avail[i] false: shown greyed when stepped onto, never stored): *v
// keeps the last available value, *shown is the one on screen (-1: *v). True when *v changed.
bool opt_choice_avail(const char *label, int *v, int *shown, const char *const *names, const bool *avail, int n, int def,
                      const char *desc);
// steps of `step` between lo and hi; fmt formats the value (e.g. "%gx")
bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc);
// whole numbers lo..hi in steps of `step` (holding left / right speeds up); fmt formats it (e.g. "%d rupees"), lo_name
// (if given) is shown for lo instead (e.g. "Any")
bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name = NULL);
// an action: true when activated
bool opt_button(const char *label, const char *desc);
// one of a set of choices (a ring on the right, filled when selected); true when chosen
bool opt_radio(const char *label, bool selected, const char *desc);
// an action / item row with a value on the right (upper: the label in capitals, as option rows; false: as written,
// e.g. a folder); true when activated
bool opt_action(const char *label, const char *value, const char *desc, bool upper = true);
// text: the value shown on the right (hint when empty); A / click turns the row into a text field (the on-screen
// keyboard with a controller). True when Enter / the keyboard confirmed a change.
#include <stddef.h>
bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc);
// that text row is being edited (its buffer is the user's: don't overwrite it)
bool opt_text_editing(const char *label);
// the next row is shown greyed out and can't be changed (e.g. not usable in this mode)
void opt_disable_next(void);
// rows after this sit one step in (+1) or back out (-1): options that belong to the row above
void opt_indent(int step);
void opt_end(void);
// A picture shown above the description while the row just drawn is hovered / selected (the Saves tab's save
// pictures). Call it right after the row, every frame it is drawn; tex is the backend's ImTextureID.
void opt_desc_image(unsigned long long tex, int w, int h);
// the next page's first row takes the controller's cursor (Down on the tab bar: ImGui's own pick across the nested
// panes was unreliable - some tabs went to a row further down, some nowhere). Cleared by the next opt_begin / call.
void opt_focus_first(bool on);
