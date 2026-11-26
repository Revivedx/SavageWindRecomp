// The NVN copy commands of the command executor (exec_list in exec.c).
#pragma once

#include <stdint.h>

#include "vk.h"

typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst);
void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst);
void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags);
