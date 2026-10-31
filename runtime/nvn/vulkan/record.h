// The recorder thread (record.c): every Vulkan call that records into a submission's command buffers, writes a
// descriptor set or submits is made by a thread of its own. The thread that translates the game's command lists (the
// queue thread, sw-queue) keeps every decision and calls the rec_* functions below instead of vkCmd* etc.: each copies
// its arguments into a packet in a ring, in the order it was called, and the recorder makes the real call in that
// order. With the recorder off (SAVAGEWIND_VK_RECORDER=0, or no thread could be made) the same functions make the call
// at once, on the calling thread: the calls and their order are the same either way.
//
// From LAS Recomp's recorder (peiikai/LASRecomp bb52932, c2bdc17, 76f4f21). Unlike it, descriptor sets are allocated
// and their pools reset by the translating thread (only their writes and binds are packets), and pipelines are bound
// by handle (draws whose pipeline isn't built yet are skipped by the translating thread, as before).
#pragma once
#include "vk.h"

// ---- start / mode (exec.c vk_exec_init, before any command)
void rec_init(void);
bool rec_on(void);  // commands go through the recorder thread

// ---- command buffers, submissions
void rec_BeginCommandBuffer(VkCommandBuffer cb);  // ONE_TIME_SUBMIT
void rec_EndCommandBuffer(VkCommandBuffer cb);
// Submits si (up to 2 command buffers, 1 wait, 2 signals; nothing chained) on g_vk.queue. seq: the timeline value it
// signals, which rec_submitted() reaches once the driver has it.
void rec_QueueSubmit2(const VkSubmitInfo2 *si, uint64_t seq);
// n writes of one descriptor each (buffer or image info), in order with the commands
void rec_UpdateDescriptorSets(uint32_t n, const VkWriteDescriptorSet *w);

// ---- waits (any thread)
uint64_t rec_submitted(void);           // the latest submission handed to the driver (the timeline value it signals)
void rec_wait_submitted(uint64_t seq);  // until the driver has submission seq (seq must have been passed to rec_QueueSubmit2)
// Everything queued so far is recorded and submitted (the translating thread): before vkDeviceWaitIdle, before
// recording into a command buffer directly (the menu's ImGui, frame generation), before using the queue outside it.
void rec_drain(void);

// ---- profiling (SAVAGEWIND_VK_PROF): counts since the last take
typedef struct {
    uint64_t packets, bytes, enqueue_ns;  // packets queued, their bytes, the translating thread's time queuing them
    uint64_t busy_ns, wakeups, submit_ns;  // the recorder's time making calls, times it woke, its time in vkQueueSubmit2
    uint64_t drains, drain_ns, seq_waits, seq_wait_ns, full_waits, full_ns;  // waits on it: drains, submissions, ring full
} RecStats;
void rec_stats_take(RecStats *o);

// ---- commands: as vkCmd*, the same arguments (arrays and structures copied; nothing chained through pNext)
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri);
void rec_CmdEndRendering(VkCommandBuffer cb);
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di);
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p);
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                               const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn);
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs);
void rec_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type);
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off, uint32_t size,
                          const void *data);
void rec_CmdDraw(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst);
void rec_CmdDrawIndexed(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, int32_t base, uint32_t first_inst);
void rec_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z);
void rec_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v);
void rec_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r);
void rec_CmdSetLineWidth(VkCommandBuffer cb, float w);
void rec_CmdSetDepthBias(VkCommandBuffer cb, float c, float k, float s);
void rec_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]);
void rec_CmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 v);
void rec_CmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 v);
void rec_CmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 v);
void rec_CmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 v);
void rec_CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 v);
void rec_CmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp v);
void rec_CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags v);
void rec_CmdSetFrontFace(VkCommandBuffer cb, VkFrontFace v);
void rec_CmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology v);
void rec_CmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v);
void rec_CmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v);
void rec_CmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v);
void rec_CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail,
                         VkCompareOp cmp);
void rec_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t n);
void rec_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query);
void rec_CmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query, VkQueryControlFlags flags);
void rec_CmdEndQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query);
void rec_CmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r);
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *c, uint32_t n,
                            const VkImageSubresourceRange *rg);
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n,
                                   const VkImageSubresourceRange *rg);
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r);
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r);
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r,
                      VkFilter f);
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                         const VkImageResolve *r);
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r);
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r);
