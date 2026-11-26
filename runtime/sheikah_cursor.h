// The mouse pointer of Savage Wind's windows: a small Sheikah-style arrow in place of the system's (sheikah_cursor.c)
#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif
SDL_Surface *sheikah_cursor_surface(float scale, int *hot);  // its picture (ARGB8888) and hot spot (tests)
SDL_Cursor *sheikah_cursor_create(float scale);  // a new one at this display scale (1: about 20 pixels tall)
SDL_Cursor *sheikah_cursor(SDL_Window *w);       // the one for this window's display scale, made once
void sheikah_cursor_use(SDL_Window *w);          // make it the pointer (no call to SDL when it already is)
#ifdef __cplusplus
}
#endif
