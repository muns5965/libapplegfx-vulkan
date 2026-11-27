#!/usr/bin/env python3
# Regenerates tests/texcompat-spv.h: HAND-WRITTEN SPIR-V fragment shaders (spirv-as text below, validated with spirv-val --target-env vulkan1.0) for
# tests/air2spv-unnorm-use-lavapipe.c and tests/texture-compat-lavapipe.c. They are test fixtures, not translator output.
#
# Usage: tests/fixtures/gen-texcompat-spv.py  (needs spirv-as and spirv-val; the header is committed)
#
# Bindings (set 0):  0 = image tex0 (2D)   1 = sampler smpA   2 = sampler smpB   3 = image tex1 (2D)   4 = image tex3 (3D)   5 = image texd (2D depth)
# Push constants (fragment):  vec4 a = (scale.x, scale.y, offset.x, offset.y)   vec4 b = (z0, dz/dx, dz/dy, lod)
#   coordinate = (frag.x * a.x + a.z, frag.y * a.y + a.w [, b.x + b.y * frag.x + b.z * frag.y])
import subprocess, sys, os, tempfile

HEAD = """OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %fc %out
OpExecutionMode %main OriginUpperLeft
OpDecorate %out Location 0
OpDecorate %fc BuiltIn FragCoord
OpDecorate %PC Block
OpMemberDecorate %PC 0 Offset 0
OpMemberDecorate %PC 1 Offset 16
OpDecorate %tex0 DescriptorSet 0
OpDecorate %tex0 Binding 0
OpDecorate %smpA DescriptorSet 0
OpDecorate %smpA Binding 1
OpDecorate %smpB DescriptorSet 0
OpDecorate %smpB Binding 2
OpDecorate %tex1 DescriptorSet 0
OpDecorate %tex1 Binding 3
OpDecorate %tex3 DescriptorSet 0
OpDecorate %tex3 Binding 4
OpDecorate %texd DescriptorSet 0
OpDecorate %texd Binding 5
%void = OpTypeVoid
%fnv = OpTypeFunction %void
%f32 = OpTypeFloat 32
%i32 = OpTypeInt 32 1
%v2f = OpTypeVector %f32 2
%v3f = OpTypeVector %f32 3
%v4f = OpTypeVector %f32 4
%v2i = OpTypeVector %i32 2
%v3i = OpTypeVector %i32 3
%img2d = OpTypeImage %f32 2D 0 0 0 1 Unknown
%img3d = OpTypeImage %f32 3D 0 0 0 1 Unknown
%img2dd = OpTypeImage %f32 2D 1 0 0 1 Unknown
%simg2d = OpTypeSampledImage %img2d
%simg3d = OpTypeSampledImage %img3d
%simg2dd = OpTypeSampledImage %img2dd
%sampler = OpTypeSampler
%p_img2d = OpTypePointer UniformConstant %img2d
%p_img3d = OpTypePointer UniformConstant %img3d
%p_img2dd = OpTypePointer UniformConstant %img2dd
%p_smp = OpTypePointer UniformConstant %sampler
%p_in4 = OpTypePointer Input %v4f
%p_out4 = OpTypePointer Output %v4f
%PC = OpTypeStruct %v4f %v4f
%p_pc = OpTypePointer PushConstant %PC
%p_pc4 = OpTypePointer PushConstant %v4f
%pc = OpVariable %p_pc PushConstant
%fc = OpVariable %p_in4 Input
%out = OpVariable %p_out4 Output
%tex0 = OpVariable %p_img2d UniformConstant
%smpA = OpVariable %p_smp UniformConstant
%smpB = OpVariable %p_smp UniformConstant
%tex1 = OpVariable %p_img2d UniformConstant
%tex3 = OpVariable %p_img3d UniformConstant
%texd = OpVariable %p_img2dd UniformConstant
%c_0 = OpConstant %i32 0
%c_1 = OpConstant %i32 1
%c_m1 = OpConstant %i32 -1
%f_0 = OpConstant %f32 0
%f_05 = OpConstant %f32 0.5
%f_2 = OpConstant %f32 2
%f_3 = OpConstant %f32 3
%f_q = OpConstant %f32 0.25
%v2z = OpConstantComposite %v2f %f_0 %f_0
%v2q = OpConstantComposite %v2f %f_q %f_q
%off2 = OpConstantComposite %v2i %c_1 %c_m1
%off3z = OpConstantComposite %v3i %c_0 %c_0 %c_1
%off3 = OpConstantComposite %v3i %c_1 %c_m1 %c_1
"""

# prologue of main: %fxy, %coord (2D), %coord3 (3D), %lod
PRO = """%main = OpFunction %void None %fnv
%entry = OpLabel
%fcv = OpLoad %v4f %fc
%fxy = OpVectorShuffle %v2f %fcv %fcv 0 1
%pa_p = OpAccessChain %p_pc4 %pc %c_0
%pa = OpLoad %v4f %pa_p
%pb_p = OpAccessChain %p_pc4 %pc %c_1
%pb = OpLoad %v4f %pb_p
%sc = OpVectorShuffle %v2f %pa %pa 0 1
%of = OpVectorShuffle %v2f %pa %pa 2 3
%cs = OpFMul %v2f %fxy %sc
%coord = OpFAdd %v2f %cs %of
%fx = OpCompositeExtract %f32 %fcv 0
%fy = OpCompositeExtract %f32 %fcv 1
%z0 = OpCompositeExtract %f32 %pb 0
%zx = OpCompositeExtract %f32 %pb 1
%zy = OpCompositeExtract %f32 %pb 2
%lod = OpCompositeExtract %f32 %pb 3
%zt1 = OpFMul %f32 %zx %fx
%zt2 = OpFMul %f32 %zy %fy
%zt3 = OpFAdd %f32 %z0 %zt1
%cz = OpFAdd %f32 %zt3 %zt2
%cx = OpCompositeExtract %f32 %coord 0
%cy = OpCompositeExtract %f32 %coord 1
%coord3 = OpCompositeConstruct %v3f %cx %cy %cz
"""
EPI = """OpReturn
OpFunctionEnd
"""

def sample2d(img, smp, op, tail, res='%r'):
    return f"""{res}_i = OpLoad %img2d {img}
{res}_s = OpLoad %sampler {smp}
{res}_si = OpSampledImage %simg2d {res}_i {res}_s
{res} = {op} %v4f {res}_si %coord {tail}
"""

def sample3d(smp, op, tail, res='%r', coord='%coord3'):
    return f"""{res}_i = OpLoad %img3d %tex3
{res}_s = OpLoad %sampler {smp}
{res}_si = OpSampledImage %simg3d {res}_i {res}_s
{res} = {op} %v4f {res}_si {coord} {tail}
"""

IMPL = 'OpImageSampleImplicitLod'
EXPL = 'OpImageSampleExplicitLod'
V = {}
def store(r='%r'): return f"OpStore %out {r}\n"
# ---- unnormalized-use variants (A = the pixel sampler) ----
V['u_impl']     = sample2d('%tex0', '%smpA', IMPL, '') + store()
V['u_bias']     = sample2d('%tex0', '%smpA', IMPL, 'Bias %f_2') + store()
V['u_off']      = sample2d('%tex0', '%smpA', IMPL, 'ConstOffset %off2') + store()
V['u_biasoff']  = sample2d('%tex0', '%smpA', IMPL, 'Bias|ConstOffset %f_2 %off2') + store()
V['u_expl']     = sample2d('%tex0', '%smpA', EXPL, 'Lod %f_3') + store()
V['u_expl_off'] = sample2d('%tex0', '%smpA', EXPL, 'Lod|ConstOffset %f_0 %off2') + store()
V['u_gradoff']  = sample2d('%tex0', '%smpA', EXPL, 'Grad|ConstOffset %v2z %v2z %off2') + store()
V['u_mixed']    = (sample2d('%tex0', '%smpA', IMPL, '', '%ra') +
    "%coordq = OpFMul %v2f %fxy %v2q\n%rb_i = OpLoad %img2d %tex1\n%rb_s = OpLoad %sampler %smpB\n%rb_si = OpSampledImage %simg2d %rb_i %rb_s\n"
    "%rb = OpImageSampleImplicitLod %v4f %rb_si %coordq\n"
    "%ax = OpCompositeExtract %f32 %ra 0\n%ay = OpCompositeExtract %f32 %ra 1\n%bx = OpCompositeExtract %f32 %rb 0\n%by = OpCompositeExtract %f32 %rb 1\n"
    "%r = OpCompositeConstruct %v4f %ax %bx %ay %by\n") + store()
V['u_unused']   = ("%rb_i = OpLoad %img2d %tex1\n%rb_s = OpLoad %sampler %smpB\n%rb_si = OpSampledImage %simg2d %rb_i %rb_s\n%r = OpImageSampleImplicitLod %v4f %rb_si %coord\n") + store()
V['u_dref']     = ("%rd_i = OpLoad %img2dd %texd\n%rd_s = OpLoad %sampler %smpA\n%rd_si = OpSampledImage %simg2dd %rd_i %rd_s\n%d = OpImageSampleDrefImplicitLod %f32 %rd_si %coord %f_05\n"
                   "%r = OpCompositeConstruct %v4f %d %d %d %d\n") + store()
V['u_img3d']    = sample3d('%smpA', IMPL, '') + store()
V['u_3dB_2dA']  = (sample2d('%tex0', '%smpA', IMPL, '', '%ra') + sample3d('%smpB', IMPL, '', '%rb') +
    "%ax = OpCompositeExtract %f32 %ra 0\n%ay = OpCompositeExtract %f32 %ra 1\n%bx = OpCompositeExtract %f32 %rb 0\n%by = OpCompositeExtract %f32 %rb 1\n"
    "%r = OpCompositeConstruct %v4f %ax %bx %ay %by\n") + store()
# ---- coordinate / filter / LOD sweep shaders (explicit LOD, so they are legal with any sampler); sampler = smpB ----
V['x2d']        = sample2d('%tex0', '%smpB', EXPL, 'Lod %lod') + store()
V['x2d_off']    = sample2d('%tex0', '%smpB', EXPL, 'Lod|ConstOffset %lod %off2') + store()
V['x3d']        = sample3d('%smpB', EXPL, 'Lod %lod') + store()
V['x3d_offz']   = sample3d('%smpB', EXPL, 'Lod|ConstOffset %lod %off3z') + store()
V['x3d_off']    = sample3d('%smpB', EXPL, 'Lod|ConstOffset %lod %off3') + store()
V['x3d_impl']   = sample3d('%smpB', IMPL, '') + store()
# the unresolved case: the sampler reaches the sample through a function parameter
UNRES_FN = """%fnt = OpTypeFunction %v4f %p_img2d %p_smp %v2f
%helper = OpFunction %v4f None %fnt
%pi = OpFunctionParameter %p_img2d
%ps = OpFunctionParameter %p_smp
%pco = OpFunctionParameter %v2f
%hl = OpLabel
%hi = OpLoad %img2d %pi
%hs = OpLoad %sampler %ps
%hsi = OpSampledImage %simg2d %hi %hs
%hr = OpImageSampleImplicitLod %v4f %hsi %pco
OpReturnValue %hr
OpFunctionEnd
"""
V['u_unres']    = "%r = OpFunctionCall %v4f %helper %tex0 %smpA %coord\n" + store()

def module(name, body):
    fns = UNRES_FN if name == 'u_unres' else ''
    # types for the helper must precede functions: they are emitted by splitting fns into its type line + function
    if fns:
        t, f = fns.split('\n', 1)
        return HEAD + t + '\n' + f + PRO + body + EPI
    return HEAD + PRO + body + EPI

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, '..', 'texcompat-spv.h')
    lines = ['/* GENERATED by tests/fixtures/gen-texcompat-spv.py (spirv-as + spirv-val vulkan1.0). Hand-written test shaders, not translator output. Bindings / push constants: see the generator. */\n']
    with tempfile.TemporaryDirectory() as td:
        for name in sorted(V):
            src = os.path.join(td, name + '.spvasm'); bin_ = os.path.join(td, name + '.spv')
            open(src, 'w').write(module(name, V[name]))
            subprocess.check_call(['spirv-as', '--target-env', 'vulkan1.0', src, '-o', bin_])
            subprocess.check_call(['spirv-val', '--target-env', 'vulkan1.0', bin_])
            data = open(bin_, 'rb').read()
            words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
            lines.append(f'static const uint32_t SPV_{name}[] = {{\n' + ',\n'.join(','.join(f'0x{w:08x}' for w in words[i:i + 8]) for i in range(0, len(words), 8)) + '\n};\n')
    open(out, 'w').write(''.join(lines))
    print('wrote', out)

main()
