#pragma once

struct ImFontAtlas;
struct ImFont;
struct GameFont;
typedef unsigned short ImWchar16;

enum { GAME_FONT_FILL, GAME_FONT_EDGE };

GameFont *game_font_load(const char *name);
float game_font_line(const GameFont *source_font);
bool game_font_edged(const GameFont *source_font);
void game_font_add(GameFont *source_font, ImFontAtlas *atlas, ImFont *font, const ImWchar16 *ranges, int layer);

void game_font_finish(ImFontAtlas *atlas);
