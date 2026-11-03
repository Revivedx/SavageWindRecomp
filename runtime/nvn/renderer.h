// Interface between the NVN executor (renderer.m) and the Metal plumbing (metal.m).
#pragma once
#ifdef __OBJC__
#import <Metal/Metal.h>
void renderer_init(id<MTLDevice> dev);
id<MTLCommandQueue> renderer_queue(void);
id<MTLTexture> renderer_texture(NvnTexture *t, id<MTLBlitCommandEncoder> blit, bool for_render);
MTLPixelFormat renderer_pixel_format(uint32_t nvn_format);
bool renderer_format_alpha_one(uint32_t nvn_format);
extern id<MTLBuffer> g_guest_buffer;
extern uint64_t g_guest_buffer_base;
#endif
#include <SDL3/SDL_events.h>

// The renderer's state (open submission, texture registry, caches) is shared by every thread that submits,
// presents, waits or finalizes textures; BOTW 1.8.2 does those from several threads. Recursive.
void gpu_lock(void);
void gpu_unlock(void);

bool renderer_is_gpu_owned(NvnTexture *t);
void renderer_flush(void);  // finish all recorded GPU work
bool renderer_origin_lower_left(void);
const char *renderer_stats(void);
bool renderer_format_is_depth(uint32_t f);
bool renderer_format_has_stencil(uint32_t f);
void renderer_mark_gpu_owned(NvnTexture *t);
bool renderer_texture_scalable(NvnTexture *t);  // allocated at the output scale
void renderer_make_scalable(NvnTexture *t);     // from now on (a target used with scaled ones)
void input_handle_event(const SDL_Event *e);
void input_focus_update(SDL_Window *w);
bool input_user_blocked(const SDL_Event *e);
