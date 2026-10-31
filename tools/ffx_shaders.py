#!/usr/bin/env python3
"""FidelityFX frame interpolation, optical flow and FSR 3 upscaler shaders -> one C header (runtime/nvn/vulkan/framegen.cpp).

The SDK builds its shader "permutation" headers with FidelityFX_SC.exe (a Windows-only wrapper around glslang that
also writes each shader's resource bindings). This does the same with glslangValidator and a small SPIR-V reader,
for the permutations Savage Wind uses only: wave32, 32-bit floats, low-resolution motion vectors without jitter, and
both depth directions (frame interpolation); SDR luma (optical flow); SDR input, low-resolution motion vectors
without jitter, depth not inverted, the reference (not LUT) Lanczos (FSR 3 upscaler, runtime/nvn/vulkan/fsr3.cpp; its
accumulate pass twice, without and with sharpening, FFX_FSR3UPSCALER_PASS_ACCUMULATE / _SHARPEN). The backend
(ffx_vk.cpp) takes each blob with its bindings by name, as FidelityFX_SC writes them.

    python3 tools/ffx_shaders.py <FidelityFX sdk dir> <output header> [glslangValidator] [--effects fi,of | fu]

--effects picks the header: fi,of (the default: frame interpolation and optical flow, spirv/ffx_shaders_vk.h) or fu
(the FSR 3 upscaler, spirv/ffx_fsr3upscaler_vk.h), so that regenerating one doesn't rewrite the other's blobs.
"""
import os
import struct
import subprocess
import sys
import tempfile

FI_PASSES = [  # FfxFrameInterpolationPass order
    'reconstruct_and_dilate_pass', 'setup_pass', 'reconstruct_previous_depth_pass', 'game_motion_vector_field_pass',
    'optical_flow_vector_field_pass', 'disocclusion_mask_pass', 'pass', 'compute_inpainting_pyramid_pass',
    'inpainting_pass', 'compute_game_vector_field_inpainting_pyramid_pass', 'debug_view_pass']
OF_PASSES = [  # FfxOpticalflowPass order
    'prepare_luma_pass', 'compute_luminance_pyramid_pass', 'generate_scd_histogram_pass', 'compute_scd_divergence_pass',
    'compute_optical_flow_advanced_pass_v5', 'filter_optical_flow_pass_v5', 'scale_optical_flow_advanced_pass_v5']

FI_DEFS = ['-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
           '-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
           '-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
           '-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
           '-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2',
           '-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1',
           '-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0']
OF_DEFS = ['-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0']
FU_PASSES = [  # FfxFsr3UpscalerPass order: (shader, sharpening); None: no shader (the deprecated TCR pass)
    ('prepare_inputs_pass', 0), ('luma_pyramid_pass', 0), ('shading_change_pyramid_pass', 0), ('shading_change_pass', 0),
    ('prepare_reactivity_pass', 0), ('luma_instability_pass', 0), ('accumulate_pass', 0), ('accumulate_pass', 1),
    ('rcas_pass', 0), ('debug_view_pass', 0), ('autogen_reactive_pass', 0), None]
FU_DEFS = ['-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
           '-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
           '-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
           '-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
           '-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2',
           '-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=2',
           '-DFFX_FSR3UPSCALER_OPTION_REPROJECT_CATMULL_ROM_5=1',
           '-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0',
           '-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1',
           '-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0',
           '-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0']

# SPIR-V opcodes / enums used below
OP_NAME, OP_ENTRY, OP_CONSTANT, OP_DECORATE, OP_MEMBER_DECORATE = 5, 15, 43, 71, 72
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_VARIABLE = \
    25, 26, 28, 29, 30, 32, 59
DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_NON_WRITABLE, DEC_BINDING, DEC_SET = 2, 3, 24, 33, 34
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER = 0, 2, 12
KINDS = ['cb', 'srv_tex', 'uav_tex', 'srv_buf', 'uav_buf', 'sampler']


def spirv_string(words):
    b = struct.pack('<%dI' % len(words), *words)
    return b[:b.index(0)].decode()


def reflect(spv):
    """{kind: [(name, binding, count, set)]} for the resources the entry point uses"""
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    names, deco, mdeco, types, consts, ptrs, vars_, iface = {}, {}, {}, {}, {}, {}, [], set()
    i = 5
    while i < len(w):
        op, n = w[i] & 0xffff, w[i] >> 16
        a = w[i + 1:i + n]
        if op == OP_NAME:
            names[a[0]] = spirv_string(a[1:])
        elif op == OP_ENTRY:
            s = spirv_string(a[2:])
            iface.update(a[2 + (len(s) + 4) // 4:])
        elif op == OP_DECORATE:
            deco.setdefault(a[0], {})[a[1]] = a[2] if len(a) > 2 else True
        elif op == OP_MEMBER_DECORATE:
            mdeco.setdefault(a[0], {}).setdefault(a[1], set()).add(a[2])
        elif op == OP_CONSTANT:
            consts[a[1]] = a[2]
        elif op in (OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT):
            types[a[0]] = (op, a[1:])
        elif op == OP_TYPE_POINTER:
            ptrs[a[0]] = a[2]
        elif op == OP_VARIABLE:
            vars_.append((a[1], a[0], a[2]))
        i += n
    out = {k: [] for k in KINDS}
    for v, ptype, sc in vars_:
        d = deco.get(v, {})
        if v not in iface or DEC_BINDING not in d:
            continue
        t, count = ptrs[ptype], 1
        while types.get(t, (0,))[0] == OP_TYPE_ARRAY:
            count *= consts[types[t][1][1]]
            t = types[t][1][0]
        op, args = types[t]
        name = names.get(v) or names.get(t)
        if op == OP_TYPE_STRUCT:
            td = deco.get(t, {})
            members = mdeco.get(t, {})
            ro = len(members) == len(args) and all(DEC_NON_WRITABLE in m for m in members.values())
            if sc == SC_UNIFORM and DEC_BLOCK in td:
                kind = 'cb'
            else:
                kind = 'srv_buf' if ro else 'uav_buf'
        elif op == OP_TYPE_IMAGE:
            kind = 'uav_tex' if args[5] == 2 else 'srv_tex'
        elif op == OP_TYPE_SAMPLER:
            kind = 'sampler'
        else:
            raise SystemExit('unknown resource type for %s' % name)
        out[kind].append((name, d[DEC_BINDING], count, d.get(DEC_SET, 0)))
    for k in KINDS:
        out[k].sort(key=lambda r: r[1])
    return out


def compile_glsl(glslang, src, incs, defs, half=0):
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, 'out.spv')
        cmd = [glslang, '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-Os', '-DFFX_GLSL=1', '-DFFX_GPU=1',
               '-DFFX_HALF=%d' % half] + defs + ['-I' + i for i in incs] + ['-o', o, src]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('%s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
        with open(o, 'rb') as f:
            return f.read()


def main():
    args = sys.argv[1:]
    effects = {'fi', 'of'}
    if '--effects' in args:
        k = args.index('--effects')
        effects = set(args[k + 1].split(','))
        del args[k:k + 2]
    sdk, out = args[0], args[1]
    glslang = args[2] if len(args) > 2 else 'glslangValidator'
    gpu = os.path.join(sdk, 'include', 'FidelityFX', 'gpu')
    shaders = os.path.join(sdk, 'src', 'backends', 'vk', 'shaders')
    blobs = []  # (c name, spv, reflection)
    for inv in (0, 1) if 'fi' in effects else ():
        for p in FI_PASSES:
            src = os.path.join(shaders, 'frameinterpolation', 'ffx_frameinterpolation_%s.glsl' % p)
            spv = compile_glsl(glslang, src, [gpu, os.path.join(gpu, 'frameinterpolation')],
                               FI_DEFS + ['-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=%d' % inv])
            blobs.append(('fi_%s_%d' % (p, inv), spv, reflect(spv)))
    for p in OF_PASSES if 'of' in effects else ():
        src = os.path.join(shaders, 'opticalflow', 'ffx_opticalflow_%s.glsl' % p)
        spv = compile_glsl(glslang, src, [gpu, os.path.join(gpu, 'opticalflow')], OF_DEFS)
        blobs.append(('of_%s' % p, spv, reflect(spv)))
    for half in (0, 1) if 'fu' in effects else ():  # (16-bit floats: where the device has them, fsr3.cpp)
        for e in FU_PASSES:
            if e is None:
                continue
            p, sharpen = e
            src = os.path.join(shaders, 'fsr3upscaler', 'ffx_fsr3upscaler_%s.glsl' % p)
            spv = compile_glsl(glslang, src, [gpu, os.path.join(gpu, 'fsr3upscaler')],
                               FU_DEFS + ['-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=%d' % sharpen], half)
            blobs.append(('fu%d_%s_%d' % (half, p, sharpen), spv, reflect(spv)))
    L = ['// Generated by tools/ffx_shaders.py from the AMD FidelityFX SDK (MIT, see third_party/FidelityFX/LICENSE.txt).',
         '// Do not edit.', '#pragma once', '#include <stdint.h>', '']
    for cname, spv, refl in blobs:
        words = struct.unpack('<%dI' % (len(spv) // 4), spv)
        L.append('static const uint32_t %s_spv[%d] = {' % (cname, len(words)))
        for j in range(0, len(words), 12):
            L.append('    ' + ','.join('0x%x' % x for x in words[j:j + 12]) + ',')
        L.append('};')
        for k in KINDS:
            r = refl[k]
            if not r:
                continue
            L.append('static const char *%s_%s_names[] = {%s};' % (cname, k, ', '.join('"%s"' % x[0] for x in r)))
            for f, idx in (('bind', 1), ('count', 2), ('space', 3)):
                L.append('static const uint32_t %s_%s_%s[] = {%s};' % (cname, k, f, ', '.join(str(x[idx]) for x in r)))
        L.append('')

    def blob(cname, refl):  # FfxShaderBlob's fields in order
        f = ['(const uint8_t *)%s_spv' % cname, 'sizeof %s_spv' % cname]
        f += [str(len(refl[k])) for k in KINDS[:5]] + [str(len(refl['sampler'])), '0']
        for k in ['cb', 'srv_tex', 'uav_tex', 'srv_buf', 'uav_buf', 'sampler']:
            if refl[k]:
                f += ['%s_%s_names' % (cname, k)] + ['%s_%s_%s' % (cname, k, x) for x in ('bind', 'count', 'space')]
            else:
                f += ['0'] * 4
        f += ['0'] * 4  # (ray tracing acceleration structures)
        return '    {' + ', '.join(f) + '},'

    if 'fi' in effects:
        L.append('// [inverted depth][FfxFrameInterpolationPass]')
        L.append('static const FfxShaderBlob ffx_fi_blobs[2][%d] = {' % len(FI_PASSES))
        for inv in (0, 1):
            L.append('  {')
            L += [blob(c, r) for c, _, r in blobs if c.startswith('fi_') and c.endswith('_%d' % inv)]
            L.append('  },')
        L.append('};')
    if 'of' in effects:
        L.append('// [FfxOpticalflowPass]')
        L.append('static const FfxShaderBlob ffx_of_blobs[%d] = {' % len(OF_PASSES))
        L += [blob(c, r) for c, _, r in blobs if c.startswith('of_')]
        L.append('};')
    if 'fu' in effects:
        L.append('// [16-bit floats][FfxFsr3UpscalerPass] (an empty blob: no shader)')
        L.append('static const FfxShaderBlob ffx_fu_blobs[2][%d] = {' % len(FU_PASSES))
        refl = {c: r for c, _, r in blobs}
        for half in (0, 1):
            L.append(' {')
            L += ['    {},' if e is None else blob('fu%d_%s_%d' % ((half,) + e), refl['fu%d_%s_%d' % ((half,) + e)])
                  for e in FU_PASSES]
            L.append(' },')
        L.append('};')
    with open(out, 'w', newline='\n') as f:
        f.write('\n'.join(L) + '\n')
    total = sum(len(s) for _, s, _ in blobs)
    print('%s: %d shaders, %d KB of SPIR-V' % (out, len(blobs), total // 1024))


if __name__ == '__main__':
    main()
