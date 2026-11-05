// Input harnesses leave the free camera disabled and exercise gameplay input.
#pragma once
#include <stdbool.h>
#include <stdint.h>
void photo_pad(uint32_t buttons) { (void)buttons; }
bool freecam_active(void) { return false; }
void freecam_toggle(void) {}
void freecam_key(int scancode, bool down) { (void)scancode; (void)down; }
void freecam_mouse(float dx, float dy) { (void)dx; (void)dy; }
void freecam_pad(float lx, float ly, float rx, float ry, float up, float down, bool fast, bool slow) {
    (void)lx; (void)ly; (void)rx; (void)ry;
    (void)up; (void)down; (void)fast; (void)slow;
}
