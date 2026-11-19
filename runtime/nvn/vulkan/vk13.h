// The commands of Vulkan 1.3's core that the renderer calls, through pointers (device.c vk13_load): the device's own on
// a 1.3 driver, and on a 1.2 driver those of the extensions they were promoted from (VK_KHR_dynamic_rendering,
// VK_KHR_synchronization2, VK_EXT_extended_dynamic_state and _state2), which have the same signatures. Include after
// vk.h in a file that calls any of them: the names below then go through the table.
#pragma once
#include "vk.h"

typedef struct {
    PFN_vkQueueSubmit2 QueueSubmit2;
    PFN_vkCmdBeginRendering CmdBeginRendering;
    PFN_vkCmdEndRendering CmdEndRendering;
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
    PFN_vkCmdWriteTimestamp2 CmdWriteTimestamp2;
    PFN_vkCmdSetCullMode CmdSetCullMode;
    PFN_vkCmdSetFrontFace CmdSetFrontFace;
    PFN_vkCmdSetPrimitiveTopology CmdSetPrimitiveTopology;
    PFN_vkCmdSetDepthTestEnable CmdSetDepthTestEnable;
    PFN_vkCmdSetDepthWriteEnable CmdSetDepthWriteEnable;
    PFN_vkCmdSetDepthCompareOp CmdSetDepthCompareOp;
    PFN_vkCmdSetStencilTestEnable CmdSetStencilTestEnable;
    PFN_vkCmdSetStencilOp CmdSetStencilOp;
    PFN_vkCmdSetDepthBiasEnable CmdSetDepthBiasEnable;
    PFN_vkCmdSetPrimitiveRestartEnable CmdSetPrimitiveRestartEnable;
} Vk13;
extern Vk13 g_vk13;

#define vkQueueSubmit2 g_vk13.QueueSubmit2
#define vkCmdBeginRendering g_vk13.CmdBeginRendering
#define vkCmdEndRendering g_vk13.CmdEndRendering
#define vkCmdPipelineBarrier2 g_vk13.CmdPipelineBarrier2
#define vkCmdWriteTimestamp2 g_vk13.CmdWriteTimestamp2
#define vkCmdSetCullMode g_vk13.CmdSetCullMode
#define vkCmdSetFrontFace g_vk13.CmdSetFrontFace
#define vkCmdSetPrimitiveTopology g_vk13.CmdSetPrimitiveTopology
#define vkCmdSetDepthTestEnable g_vk13.CmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable g_vk13.CmdSetDepthWriteEnable
#define vkCmdSetDepthCompareOp g_vk13.CmdSetDepthCompareOp
#define vkCmdSetStencilTestEnable g_vk13.CmdSetStencilTestEnable
#define vkCmdSetStencilOp g_vk13.CmdSetStencilOp
#define vkCmdSetDepthBiasEnable g_vk13.CmdSetDepthBiasEnable
#define vkCmdSetPrimitiveRestartEnable g_vk13.CmdSetPrimitiveRestartEnable
