// Options > Accessibility > Colour vision: a correction of the whole presented picture (the 3D world, the game's HUD
// and menus) for players with a colour vision deficiency, so colours they confuse (red and green; blue and yellow)
// come apart. Applied at the present (runtime/nvn/vulkan/exec.c vk_present_draw for SDR, device.c's HDR10 present),
// after the game has drawn its HUD; the Savage Wind menu and Album photos keep their colours. Off: nothing runs.
//
// The model: Machado, Oliveira and Fernandes (2009), "A Physiologically-based Model for Simulation of Color Vision
// Deficiency", severity 1.0 matrices S in linear RGB (a dichromat's view). The correction ("daltonize") moves the
// difference the player cannot see, c - S c, into channels they can: c' = c + k E (c - S c), with E sending the lost
// red-green difference to green and blue (protan, deutan) or the lost blue-yellow one to red and green (tritan), and k
// the setting's strength. That is one 3x3 matrix, M = I + k E (I - S); white and greys stay as they are.
//
// (testing) SAVAGEWIND_CVD_SIMULATE=protan|deutan|tritan[:<percent>]: the picture as a dichromat sees it (S, mixed
// with the identity at that percent), after the correction if one is on (S M: "corrected, as they see it").
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The matrix for the current settings, row-major (m[0..2]: output red from input r, g, b): applied to linear RGB.
// false when it is the identity (option off, no simulation): the present draws the picture unchanged.
bool colour_vision_matrix(float m[9]);

// The matrix for one type (1 protanopia, 2 deuteranopia, 3 tritanopia) at a strength (0..1), and the simulation of
// that type at a severity (0..1, mixed with the identity), row-major. (tests, colour_vision_matrix)
void colour_vision_correction(int type, float strength, float m[9]);
void colour_vision_simulation(int type, float severity, float m[9]);

#ifdef __cplusplus
}
#endif
