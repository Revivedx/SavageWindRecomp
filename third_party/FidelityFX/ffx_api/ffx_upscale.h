// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once
#include "ffx_api.h"
#include "ffx_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FFX_UPSCALER_VERSION_MAJOR 4
#define FFX_UPSCALER_VERSION_MINOR 1
#define FFX_UPSCALER_VERSION_PATCH 2

#define FFX_UPSCALER_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_UPSCALER_VERSION                           FFX_UPSCALER_MAKE_VERSION(FFX_UPSCALER_VERSION_MAJOR, FFX_UPSCALER_VERSION_MINOR, FFX_UPSCALER_VERSION_PATCH)

enum FfxApiUpscaleQualityMode
{
    FFX_UPSCALE_QUALITY_MODE_NATIVEAA          = 0,  ///< Perform upscaling with a per-dimension upscaling ratio of 1.0x.
    FFX_UPSCALE_QUALITY_MODE_QUALITY           = 1,  ///< Perform upscaling with a per-dimension upscaling ratio of 1.5x.
    FFX_UPSCALE_QUALITY_MODE_BALANCED          = 2,  ///< Perform upscaling with a per-dimension upscaling ratio of 1.7x.
    FFX_UPSCALE_QUALITY_MODE_PERFORMANCE       = 3,  ///< Perform upscaling with a per-dimension upscaling ratio of 2.0x.
    FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE = 4   ///< Perform upscaling with a per-dimension upscaling ratio of 3.0x.
};

enum FfxApiCreateContextUpscaleFlags
{
    FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE                 = (1 << 0),  ///< A bit indicating if the input color data provided is using a high-dynamic range.
    FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS  = (1 << 1),  ///< A bit indicating if the motion vectors are rendered at display resolution.
    FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2),  ///< A bit indicating that the motion vectors have the jittering pattern applied to them.
    FFX_UPSCALE_ENABLE_DEPTH_INVERTED                     = (1 << 3),  ///< A bit indicating that the input depth buffer data provided is inverted [1..0].
    FFX_UPSCALE_ENABLE_DEPTH_INFINITE                     = (1 << 4),  ///< A bit indicating that the input depth buffer data provided is using an infinite far plane.
    FFX_UPSCALE_ENABLE_AUTO_EXPOSURE                      = (1 << 5),  ///< A bit indicating if automatic exposure should be applied to input color data.
    FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION                 = (1 << 6),  ///< A bit indicating that the application uses dynamic resolution scaling.
    FFX_UPSCALE_ENABLE_DEBUG_CHECKING                     = (1 << 7),  ///< A bit indicating that the runtime should check some API values and report issues.
    FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE              = (1 << 8),  ///< A bit indicating that the color resource contains perceptual (gamma corrected) colors
    FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION                = (1 << 9),  ///< A bit indicating if debug visualization is allowed. (memory consumption could increase)
};

enum FfxApiDispatchFsrUpscaleFlags
{
    FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW       = (1 << 0),
    FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB = (1 << 1),
    FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ   = (1 << 2),
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00)
struct ffxCreateContextDescUpscale
{
    ffxCreateContextDescHeader header;          ///< Header descriptor, use type FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE.
    uint32_t                   flags;           ///< Zero or a combination of values from FfxApiCreateContextUpscaleFlags.
    struct FfxApiDimensions2D  maxRenderSize;   ///< The maximum size that rendering will be performed at.
    struct FfxApiDimensions2D  maxUpscaleSize;  ///< The size of the presentation resolution targeted by the upscaling process.
    ffxApiMessage              fpMessage;       ///< A pointer to a function that can receive messages from the runtime. May be null.
};

#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01)
struct ffxDispatchDescUpscale
{
    ffxDispatchDescHeader      header;                      ///< Header descriptor, use type FFX_API_DISPATCH_DESC_TYPE_UPSCALE.
    void*                      commandList;                 ///< Command list to record upscaling rendering commands into.
    struct FfxApiResource      color;                       ///< Color buffer for the current frame (at render resolution).
    struct FfxApiResource      depth;                       ///< 32bit depth values for the current frame (at render resolution).
    struct FfxApiResource      motionVectors;               ///< 2-dimensional motion vectors (at render resolution).
    struct FfxApiResource      exposure;                    ///< Optional resource containing a 1x1 exposure value.
    struct FfxApiResource      reactive;                    ///< Optional resource containing alpha value of reactive objects in the scene.
    struct FfxApiResource      transparencyAndComposition;  ///< Optional resource containing alpha value of special objects in the scene.
    struct FfxApiResource      output;                      ///< Output color buffer for the current frame (at presentation resolution).
    struct FfxApiFloatCoords2D jitterOffset;                ///< The subpixel jitter offset applied to the camera.
    struct FfxApiFloatCoords2D motionVectorScale;           ///< The scale factor to convert motion vectors to pixel space.
    struct FfxApiDimensions2D  renderSize;                  ///< The resolution that was used for rendering the input resources.
    struct FfxApiDimensions2D  upscaleSize;                 ///< The resolution that the upscaler will upscale to.
    bool                       enableSharpening;            ///< Enable an additional sharpening pass.
    float                      sharpness;                   ///< The sharpness value between 0 and 1.
    float                      frameTimeDelta;              ///< The time elapsed since the last frame (expressed in milliseconds).
    float                      preExposure;                 ///< The pre exposure value (must be > 0.0f)
    bool                       reset;                       ///< Indicates the camera has moved discontinuously.
    float                      cameraNear;                  ///< The distance to the near plane of the camera.
    float                      cameraFar;                   ///< The distance to the far plane of the camera.
    float                      cameraFovAngleVertical;      ///< The camera angle field of view in the vertical direction (expressed in radians).
    float                      viewSpaceToMetersFactor;     ///< The scale factor to convert view space units to meters
    uint32_t                   flags;                       ///< Zero or a combination of values from FfxApiDispatchFsrUpscaleFlags.
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x02)
struct ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode
{
    ffxQueryDescHeader header;
    uint32_t           qualityMode;
    float*             pOutUpscaleRatio;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x03)
struct ffxQueryDescUpscaleGetRenderResolutionFromQualityMode
{
    ffxQueryDescHeader header;
    uint32_t           displayWidth;
    uint32_t           displayHeight;
    uint32_t           qualityMode;
    uint32_t*          pOutRenderWidth;
    uint32_t*          pOutRenderHeight;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x04)
struct ffxQueryDescUpscaleGetJitterPhaseCount
{
    ffxQueryDescHeader header;
    uint32_t           renderWidth;
    uint32_t           displayWidth;
    int32_t*           pOutPhaseCount;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x05)
struct ffxQueryDescUpscaleGetJitterOffset
{
    ffxQueryDescHeader header;
    int32_t            index;
    int32_t            phaseCount;
    float*             pOutX;
    float*             pOutY;
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b)
struct ffxCreateContextDescUpscaleVersion
{
    ffxCreateContextDescHeader header;   ///< Header descriptor, use type FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION.
    uint32_t                   version;  ///< Set to FFX_UPSCALER_VERSION.
};

#ifdef __cplusplus
}
#endif

