// Draws of the Vulkan command executor (exec.c): index expansion, the program/pipeline lookup, link and wait, and
// the state a draw records. Split from exec.c; the shared state and helpers are in exec/internal.h.
#pragma once
#include <stdbool.h>

#include "exec/internal.h"

void draw(CmdRec *r, bool indexed);
