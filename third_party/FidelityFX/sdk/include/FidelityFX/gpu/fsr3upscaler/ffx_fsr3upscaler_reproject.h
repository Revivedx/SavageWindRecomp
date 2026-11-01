// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
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

#ifndef FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE
#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0 // Reference
#endif

FfxFloat32x4 WrapHistory(FfxInt32x2 iPxSample)
{
    return LoadHistory(iPxSample);
}

DeclareCustomFetchBicubicSamples(FetchHistorySamples, WrapHistory)
DeclareCustomTextureSample(HistorySample, FFX_FSR3UPSCALER_GET_LANCZOS_SAMPLER1D(FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE), FetchHistorySamples)

#if FFX_HALF
FFX_MIN16_F4 WrapHistory16(FfxInt32x2 iPxSample)
{
    return FFX_MIN16_F4(LoadHistory(iPxSample));
}

DeclareCustomFetchBicubicSamplesMin16(FetchHistorySamples16, WrapHistory16)
DeclareCustomTextureSampleMin16(HistorySample16, FFX_FSR3UPSCALER_GET_LANCZOS_SAMPLER1D(FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE), FetchHistorySamples16)
#endif

FfxFloat32x2 GetMotionVector(FfxInt32x2 iPxHrPos, FfxFloat32x2 fHrUv)
{
#if FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS
    const FfxFloat32x2 fDilatedMotionVector = LoadDilatedMotionVector(FFX_MIN16_I2(fHrUv * RenderSize()));
#else
    const FfxFloat32x2 fDilatedMotionVector = LoadInputMotionVector(iPxHrPos);
#endif

    return fDilatedMotionVector;
}

void ComputeReprojectedUVs(FFX_PARAMETER_INOUT AccumulationPassCommonParams params)
{
    params.fReprojectedHrUv = params.fHrUv + params.fMotionVector;

    params.bIsExistingSample = IsUvInside(params.fReprojectedHrUv);
}

#if FFX_FSR3UPSCALER_OPTION_REPROJECT_CATMULL_ROM_5
// Savage Wind: the history reprojected with a Catmull-Rom filter from 5 bilinear samples (its 4x4 footprint's corners
// left out) instead of Lanczos on 16 loads: the accumulation's largest cost, ~0.3 ms less at 3840x2160 on an RTX 3060.
// The history texture is the upscale size (the upscaler's output, fixed).
FfxFloat32x4 HistorySampleCatmullRom5(FfxFloat32x2 fUv, FfxFloat32x2 fSize)
{
    const FfxFloat32x2 fPx = fUv * fSize;
    const FfxFloat32x2 fPx1 = floor(fPx - 0.5f) + 0.5f;
    const FfxFloat32x2 f = fPx - fPx1;
    const FfxFloat32x2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const FfxFloat32x2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const FfxFloat32x2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const FfxFloat32x2 w3 = f * f * (-0.5f + 0.5f * f);
    const FfxFloat32x2 w12 = w1 + w2;
    const FfxFloat32x2 fUv0 = (fPx1 - 1.0f) / fSize;
    const FfxFloat32x2 fUv3 = (fPx1 + 2.0f) / fSize;
    const FfxFloat32x2 fUv12 = (fPx1 + w2 / w12) / fSize;
    const FfxFloat32 k0 = w12.x * w0.y, k1 = w0.x * w12.y, k2 = w12.x * w12.y, k3 = w3.x * w12.y, k4 = w12.x * w3.y;
    FfxFloat32x4 c = SampleHistory(FfxFloat32x2(fUv12.x, fUv0.y)) * k0
                   + SampleHistory(FfxFloat32x2(fUv0.x, fUv12.y)) * k1
                   + SampleHistory(fUv12) * k2
                   + SampleHistory(FfxFloat32x2(fUv3.x, fUv12.y)) * k3
                   + SampleHistory(FfxFloat32x2(fUv12.x, fUv3.y)) * k4;
    return ffxMax(c / (k0 + k1 + k2 + k3 + k4), FfxFloat32x4(0.0f, 0.0f, 0.0f, 0.0f));
}
#endif

void ReprojectHistoryColor(const AccumulationPassCommonParams params, FFX_PARAMETER_INOUT AccumulationPassData data)

{
#if FFX_FSR3UPSCALER_OPTION_REPROJECT_CATMULL_ROM_5
    const FfxFloat32x4 fReprojectedHistory = HistorySampleCatmullRom5(params.fReprojectedHrUv, FfxFloat32x2(UpscaleSize()));
#elif FFX_HALF && FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF
    const FfxFloat32x4 fReprojectedHistory = FfxFloat32x4(HistorySample16(params.fReprojectedHrUv, UpscaleSize()));
#else
    const FfxFloat32x4 fReprojectedHistory = HistorySample(params.fReprojectedHrUv, PreviousFrameUpscaleSize());
#endif

    data.fHistoryColor = fReprojectedHistory.rgb;
    data.fHistoryColor *= DeltaPreExposure();
    data.fHistoryColor *= Exposure();

    data.fHistoryColor = RGBToYCoCg(data.fHistoryColor);

    data.fLock = fReprojectedHistory.w;
}
