/* AIR integer intrinsics (census X11), executed: each fixture computes four integer-intrinsic results per pixel from the pixel x coordinate (inputs: X==0 ? 0 :
 * 1<<(X-1), plus a derived multi-bit value), writes them as float4 to an RGBA32F target, and every pixel is compared with a CPU reference of the Metal
 * semantics (clz(0) = ctz(0) = bit width, popcount, reverse_bits, ...). Runs the translated SPIR-V on a Vulkan 1.3 device (lavapipe). Exit 77 (skip) when there is
 * no device or it lacks shaderInt16 / shaderInt64 / shaderInt8.
 * Fixtures tests/fixtures/rt_*_fragment.air.bc come from the sibling .ll files (llvm-as; HAND-WRITTEN IR, NOT Apple output); names per the MIT project
 * vertex-language/air (intrinsic.go @3d277d16858a), none of them occurs in the device dumps of runs 231501 / 230109.
 * SPDX-License-Identifier: MIT */
#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP(...) do { printf("SKIP: "); printf(__VA_ARGS__); printf("\n"); return 77; } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

/* vertex shader (glslangValidator -V of the GLSL below): full-screen triangle from gl_VertexIndex; Location 0 = (fragment x + 0.5, fragment y + 0.5, 0, 1) for a
 * 65 x 65 target (the fixtures read it as the AIR position argument), Location 1 = 0.
#version 450
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec2 ndc = p * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    o0 = vec4((ndc.x * 0.5 + 0.5) * 65.0, (ndc.y * 0.5 + 0.5) * 65.0, 0.0, 1.0);   / * = (fragment x + 0.5, fragment y + 0.5) at pixel centres of a 65 x 65 target * /
    o1 = vec4(0.0);
}
 */
static const uint32_t VERT_SPV[] = {
0x07230203,0x00010000,0x0008000b,0x0000003f,0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
0x0009000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x00000024,0x0000002d,
0x0000003d,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,
0x00030005,0x00000009,0x00000070,0x00060005,0x0000000c,0x565f6c67,0x65747265,0x646e4978,
0x00007865,0x00030005,0x00000017,0x0063646e,0x00060005,0x00000022,0x505f6c67,0x65567265,
0x78657472,0x00000000,0x00060006,0x00000022,0x00000000,0x505f6c67,0x7469736f,0x006e6f69,
0x00070006,0x00000022,0x00000001,0x505f6c67,0x746e696f,0x657a6953,0x00000000,0x00070006,
0x00000022,0x00000002,0x435f6c67,0x4470696c,0x61747369,0x0065636e,0x00070006,0x00000022,
0x00000003,0x435f6c67,0x446c6c75,0x61747369,0x0065636e,0x00030005,0x00000024,0x00000000,
0x00030005,0x0000002d,0x0000306f,0x00030005,0x0000003d,0x0000316f,0x00040047,0x0000000c,
0x0000000b,0x0000002a,0x00030047,0x00000022,0x00000002,0x00050048,0x00000022,0x00000000,
0x0000000b,0x00000000,0x00050048,0x00000022,0x00000001,0x0000000b,0x00000001,0x00050048,
0x00000022,0x00000002,0x0000000b,0x00000003,0x00050048,0x00000022,0x00000003,0x0000000b,
0x00000004,0x00040047,0x0000002d,0x0000001e,0x00000000,0x00040047,0x0000003d,0x0000001e,
0x00000001,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,
0x00000020,0x00040017,0x00000007,0x00000006,0x00000002,0x00040020,0x00000008,0x00000007,
0x00000007,0x00040015,0x0000000a,0x00000020,0x00000001,0x00040020,0x0000000b,0x00000001,
0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000001,0x0004002b,0x0000000a,0x0000000e,
0x00000001,0x0004002b,0x0000000a,0x00000010,0x00000002,0x0004002b,0x00000006,0x00000019,
0x40000000,0x0004002b,0x00000006,0x0000001b,0x3f800000,0x00040017,0x0000001e,0x00000006,
0x00000004,0x00040015,0x0000001f,0x00000020,0x00000000,0x0004002b,0x0000001f,0x00000020,
0x00000001,0x0004001c,0x00000021,0x00000006,0x00000020,0x0006001e,0x00000022,0x0000001e,
0x00000006,0x00000021,0x00000021,0x00040020,0x00000023,0x00000003,0x00000022,0x0004003b,
0x00000023,0x00000024,0x00000003,0x0004002b,0x0000000a,0x00000025,0x00000000,0x0004002b,
0x00000006,0x00000027,0x00000000,0x00040020,0x0000002b,0x00000003,0x0000001e,0x0004003b,
0x0000002b,0x0000002d,0x00000003,0x0004002b,0x0000001f,0x0000002e,0x00000000,0x00040020,
0x0000002f,0x00000007,0x00000006,0x0004002b,0x00000006,0x00000032,0x3f000000,0x0004002b,
0x00000006,0x00000035,0x42820000,0x0004003b,0x0000002b,0x0000003d,0x00000003,0x0007002c,
0x0000001e,0x0000003e,0x00000027,0x00000027,0x00000027,0x00000027,0x00050036,0x00000002,
0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,
0x00000007,0x0004003b,0x00000008,0x00000017,0x00000007,0x0004003d,0x0000000a,0x0000000d,
0x0000000c,0x000500c4,0x0000000a,0x0000000f,0x0000000d,0x0000000e,0x000500c7,0x0000000a,
0x00000011,0x0000000f,0x00000010,0x0004006f,0x00000006,0x00000012,0x00000011,0x0004003d,
0x0000000a,0x00000013,0x0000000c,0x000500c7,0x0000000a,0x00000014,0x00000013,0x00000010,
0x0004006f,0x00000006,0x00000015,0x00000014,0x00050050,0x00000007,0x00000016,0x00000012,
0x00000015,0x0003003e,0x00000009,0x00000016,0x0004003d,0x00000007,0x00000018,0x00000009,
0x0005008e,0x00000007,0x0000001a,0x00000018,0x00000019,0x00050050,0x00000007,0x0000001c,
0x0000001b,0x0000001b,0x00050083,0x00000007,0x0000001d,0x0000001a,0x0000001c,0x0003003e,
0x00000017,0x0000001d,0x0004003d,0x00000007,0x00000026,0x00000017,0x00050051,0x00000006,
0x00000028,0x00000026,0x00000000,0x00050051,0x00000006,0x00000029,0x00000026,0x00000001,
0x00070050,0x0000001e,0x0000002a,0x00000028,0x00000029,0x00000027,0x0000001b,0x00050041,
0x0000002b,0x0000002c,0x00000024,0x00000025,0x0003003e,0x0000002c,0x0000002a,0x00050041,
0x0000002f,0x00000030,0x00000017,0x0000002e,0x0004003d,0x00000006,0x00000031,0x00000030,
0x00050085,0x00000006,0x00000033,0x00000031,0x00000032,0x00050081,0x00000006,0x00000034,
0x00000033,0x00000032,0x00050085,0x00000006,0x00000036,0x00000034,0x00000035,0x00050041,
0x0000002f,0x00000037,0x00000017,0x00000020,0x0004003d,0x00000006,0x00000038,0x00000037,
0x00050085,0x00000006,0x00000039,0x00000038,0x00000032,0x00050081,0x00000006,0x0000003a,
0x00000039,0x00000032,0x00050085,0x00000006,0x0000003b,0x0000003a,0x00000035,0x00070050,
0x0000001e,0x0000003c,0x00000036,0x0000003b,0x00000027,0x0000001b,0x0003003e,0x0000002d,
0x0000003c,0x0003003e,0x0000003d,0x0000003e,0x000100fd,0x00010038
};

static uint8_t *slurp(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
        fclose(f); *n = (size_t)sz; return b;
    }
    return NULL;
}

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xffffffffu;
}


/* ---- CPU references (Metal integer-function semantics) ---- */
static uint64_t bitrev(uint64_t v, unsigned w) { uint64_t r = 0; for (unsigned i = 0; i < w; i++) if (v >> i & 1u) r |= (uint64_t)1 << (w - 1 - i); return r; }
static unsigned clzw(uint64_t v, unsigned w) { unsigned n = 0; for (int i = (int)w - 1; i >= 0 && !(v >> i & 1u); i--) n++; return n; }   /* clz(0) = w */
static unsigned ctzw(uint64_t v, unsigned w) { unsigned n = 0; for (unsigned i = 0; i < w && !(v >> i & 1u); i++) n++; return n; }          /* ctz(0) = w */
static unsigned popc(uint64_t v) { unsigned n = 0; for (; v; v >>= 1) n += (unsigned)(v & 1u); return n; }
static uint32_t inA(int x) { return x == 0 ? 0u : 1u << ((x - 1) & 31); }
static uint32_t inB(int x) { uint32_t a = inA(x); return a | (a >> 2) | (a >> 5); }
static uint64_t inL(int x) { return x == 0 ? 0u : (uint64_t)1 << (x - 1); }
static uint64_t inLB(int x) { uint64_t l = inL(x); return l | (l >> 2) | (l >> 7); }

static void ref_bits32(int x, int y, float o[4]) { uint32_t a = inA(x); o[0] = (float)clzw(a, 32); o[1] = (float)ctzw(a, 32); o[2] = (float)popc(inB(x)); o[3] = (float)bitrev(a, 32); }
static void ref_narrow(int x, int y, float o[4]) { uint32_t a = inA(x); o[0] = (float)clzw(a & 0xffffu, 16); o[1] = (float)ctzw(a & 0xffffu, 16); o[2] = (float)clzw(a & 0xffu, 8); o[3] = (float)ctzw(a & 0xffu, 8); }
static void ref_narrow2(int x, int y, float o[4]) { uint32_t b = inB(x); o[0] = (float)popc(b & 0xffffu); o[1] = (float)bitrev(b & 0xffffu, 16); o[2] = (float)popc(b & 0xffu); o[3] = (float)bitrev(b & 0xffu, 8); }
static void ref_bits64(int x, int y, float o[4]) { uint64_t l = inL(x); o[0] = (float)clzw(l, 64); o[1] = (float)ctzw(l, 64); o[2] = (float)popc(inLB(x)); o[3] = (float)(double)bitrev(l, 64); }
static void ref_bitsvec(int x, int y, float o[4]) { uint32_t a = inA(x), b = inB(x); o[0] = (float)clzw(b, 32); o[1] = (float)ctzw(a, 32); o[2] = (float)popc(b); o[3] = (float)bitrev(b, 32); }


/* arithmetic fixtures: a = g(x), b = g(y) with g(X, w) = (X << (w-6)) | (X odd ? (1 << (w-6)) - 1 : 0): the 65 values per axis include 0, MIN (X=32), MAX (X=31), -1 (X=63) */
static uint64_t gen_in(int X, unsigned w) { uint64_t v = ((uint64_t)X << (w - 6)) | ((X & 1) ? (((uint64_t)1 << (w - 6)) - 1u) : 0u); return w == 64 ? v : v & (((uint64_t)1 << w) - 1u); }
static int64_t sx(uint64_t v, unsigned w) { return w == 64 ? (int64_t)v : (int64_t)(v << (64 - w)) >> (64 - w); }
static uint64_t wmask(uint64_t v, unsigned w) { return w == 64 ? v : v & (((uint64_t)1 << w) - 1u); }
static uint64_t ref_mulhi(uint64_t a, uint64_t b, unsigned w, int sgn) {
    if (w == 64) return sgn ? (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64) : (uint64_t)(((unsigned __int128)a * b) >> 64);
    return wmask(sgn ? (uint64_t)((sx(a, w) * sx(b, w)) >> w) : (a * b) >> w, w);
}
static uint64_t ref_addsat(uint64_t a, uint64_t b, unsigned w, int sgn) {
    if (!sgn) { uint64_t s = wmask(a + b, w); return s < a ? wmask(~(uint64_t)0, w) : s; }
    __int128 r = (__int128)sx(a, w) + sx(b, w), hi = ((__int128)1 << (w - 1)) - 1, lo = -((__int128)1 << (w - 1));
    return wmask((uint64_t)(r > hi ? hi : r < lo ? lo : r), w);
}
static uint64_t ref_subsat(uint64_t a, uint64_t b, unsigned w, int sgn) {
    if (!sgn) return a < b ? 0u : a - b;
    __int128 r = (__int128)sx(a, w) - sx(b, w), hi = ((__int128)1 << (w - 1)) - 1, lo = -((__int128)1 << (w - 1));
    return wmask((uint64_t)(r > hi ? hi : r < lo ? lo : r), w);
}
static uint64_t (*g_op)(uint64_t, uint64_t, unsigned, int);
static int g_sgn;
static float fold64(uint64_t r) { return (float)((r ^ r >> 16 ^ r >> 32 ^ r >> 48) & 0xffffu); }
static void arith_ref(int x, int y, float o[4]) {
    uint64_t r32 = g_op(gen_in(x, 32), gen_in(y, 32), 32, g_sgn);
    o[0] = (float)g_op(gen_in(x, 8), gen_in(y, 8), 8, g_sgn); o[1] = (float)g_op(gen_in(x, 16), gen_in(y, 16), 16, g_sgn);
    o[2] = (float)(r32 >> 16); o[3] = (float)(r32 & 0xffffu);
}
#define ARITH_REF(NAME, OP, SGN) static void NAME(int x, int y, float o[4]) { g_op = OP; g_sgn = SGN; arith_ref(x, y, o); }
ARITH_REF(ref_mulhi_u, ref_mulhi, 0) ARITH_REF(ref_mulhi_s, ref_mulhi, 1)
ARITH_REF(ref_addsat_u, ref_addsat, 0) ARITH_REF(ref_addsat_s, ref_addsat, 1)
ARITH_REF(ref_subsat_u, ref_subsat, 0) ARITH_REF(ref_subsat_s, ref_subsat, 1)
static void ref_abs(int x, int y, float o[4]) {   /* abs.u = identity; abs.s = |x| as an unsigned bit pattern (MIN stays MIN) */
    int64_t s8 = sx(gen_in(y, 8), 8), s32 = sx(gen_in(x, 32), 32);
    uint32_t r32 = (uint32_t)(s32 < 0 ? -s32 : s32);
    o[0] = (float)gen_in(x, 16); o[1] = (float)(uint8_t)(s8 < 0 ? -s8 : s8); o[2] = (float)(r32 >> 16); o[3] = (float)(r32 & 0xffffu);
}
static void ref_minmax(int x, int y, float o[4]) {   /* GLSL.std.450 SMin/UMax/UClamp/SClamp via air.{min,max,clamp}.{s,u}.<ty> */
    uint64_t a8 = gen_in(x, 8), b8 = gen_in(y, 8), a16 = gen_in(x, 16), b16 = gen_in(y, 16), a32 = gen_in(x, 32), b32 = gen_in(y, 32);
    int64_t smin16 = sx(a16, 16) < sx(b16, 16) ? sx(a16, 16) : sx(b16, 16), smax16 = sx(a16, 16) > sx(b16, 16) ? sx(a16, 16) : sx(b16, 16);
    uint64_t lo = a32 < b32 ? a32 : b32, hi = a32 > b32 ? a32 : b32, x32 = a32 ^ b32, c32 = x32 < lo ? lo : x32 > hi ? hi : x32;
    int64_t x16 = sx(a16 ^ b16, 16), c16 = x16 < smin16 ? smin16 : x16 > smax16 ? smax16 : x16;
    o[0] = (float)wmask((uint64_t)smin16, 16); o[1] = (float)(a8 > b8 ? a8 : b8); o[2] = (float)((c32 >> 16) ^ (c32 & 0xffffu)); o[3] = (float)wmask((uint64_t)c16, 16);
}
static void ref_sat64a(int x, int y, float o[4]) {
    uint64_t a = gen_in(x, 64), b = gen_in(y, 64);
    o[0] = fold64(ref_mulhi(a, b, 64, 0)); o[1] = fold64(ref_mulhi(a, b, 64, 1)); o[2] = fold64(ref_addsat(a, b, 64, 0)); o[3] = fold64(ref_addsat(a, b, 64, 1));
}
static void ref_sat64b(int x, int y, float o[4]) {
    uint64_t a = gen_in(x, 64), b = gen_in(y, 64);
    o[0] = fold64(ref_subsat(a, b, 64, 0)); o[1] = fold64(ref_subsat(a, b, 64, 1));
    o[2] = (float)ref_addsat(gen_in(y, 16), gen_in(x, 16), 16, 1);                       /* v2i16 add_sat.s: lane 1 = add_sat(b, a) */
    o[3] = (float)(ref_mulhi(gen_in(x, 32), gen_in(y, 32), 32, 0) >> 16);                /* v2i32 mul_hi.u: lane 0 = mul_hi(a, b), high 16 bits */
}

/* bit-field fixtures: offset = x % (w+1), bits = min(y % (w+1), w - offset) (always defined), operands mixed from x and y */
static uint64_t fld_mask(unsigned n, unsigned w) { return n >= w ? wmask(~(uint64_t)0, w) : (((uint64_t)1 << n) - 1u); }
static uint64_t ref_extract(uint64_t v, unsigned off, unsigned n, unsigned w, int sgn) {
    if (n == 0) return 0;
    uint64_t f = (off >= 64 ? 0 : v >> off) & fld_mask(n, w);
    if (sgn && (f >> (n - 1) & 1u)) f |= ~fld_mask(n, 64);
    return wmask(f, w);
}
static uint64_t ref_insert(uint64_t base, uint64_t ins, unsigned off, unsigned n, unsigned w) {
    if (n == 0) return base;
    uint64_t m = off >= 64 ? 0 : fld_mask(n, w) << off;
    return wmask((base & ~m) | ((ins << off) & m), w);
}
typedef struct { unsigned off, n; } fld_t;
static fld_t fld_for(int x, int y, unsigned w) { unsigned o = (unsigned)x % (w + 1u), n0 = (unsigned)y % (w + 1u), av = w - o; return (fld_t){ o, n0 < av ? n0 : av }; }
static float fold16of32(uint64_t r) { return (float)((r >> 16) ^ (r & 0xffffu)); }
static void bf_inputs(int x, int y, uint32_t *s32, uint32_t *t32) { uint32_t mx = (uint32_t)x * 0x9E3779B1u + (uint32_t)y * 0x85EBCA6Bu; *s32 = mx; *t32 = mx * 0x27D4EB2Fu; }
static void ref_bitfield(int x, int y, float o[4]) {
    uint32_t s, t; bf_inputs(x, y, &s, &t);
    fld_t f8 = fld_for(x, y, 8), f16 = fld_for(x, y, 16), f32 = fld_for(x, y, 32);
    o[0] = (float)ref_extract(s & 0xffu, f8.off, f8.n, 8, 1); o[1] = (float)ref_extract(s & 0xffffu, f16.off, f16.n, 16, 0);
    o[2] = fold16of32(ref_extract(s, f32.off, f32.n, 32, 1)); o[3] = fold16of32(ref_insert(s, t, f32.off, f32.n, 32));
}
static void ref_bitfield2(int x, int y, float o[4]) {
    uint32_t s, t; bf_inputs(x, y, &s, &t);
    fld_t f8 = fld_for(x, y, 8), f16 = fld_for(x, y, 16), f32 = fld_for(x, y, 32);
    o[0] = (float)ref_insert(s & 0xffu, t & 0xffu, f8.off, f8.n, 8); o[1] = (float)ref_insert(s & 0xffffu, t & 0xffffu, f16.off, f16.n, 16);
    o[2] = fold16of32(ref_extract(s, f32.off, f32.n, 32, 0)); o[3] = fold16of32(ref_insert(t, s, f32.off, f32.n, 32));   /* v2i32 lane 1: base = t, insert = s */
}
static void ref_bitfield64(int x, int y, float o[4]) {
    uint32_t s32, t32; bf_inputs(x, y, &s32, &t32);
    uint64_t s = ((uint64_t)s32 * 0x9E3779B97F4A7C15ull) ^ s32, t = ((uint64_t)t32 * 0xBF58476D1CE4E5B9ull) ^ t32;
    fld_t f = fld_for(x, y, 64);
    o[0] = fold64(ref_extract(s, f.off, f.n, 64, 1)); o[1] = fold64(ref_extract(s, f.off, f.n, 64, 0));
    o[2] = fold64(ref_insert(s, t, f.off, f.n, 64)); o[3] = fold64(ref_insert(t, s, f.off, f.n, 64));
}

typedef struct { const char *fixture; int w, h; void (*ref)(int x, int y, float o[4]); } case_t;
static const case_t cases[] = {
    { "rt_bits32_fragment.air.bc",     65, 65, ref_bits32 },
    { "rt_bitsnarrow_fragment.air.bc", 65, 65, ref_narrow },
    { "rt_bitsnarrow2_fragment.air.bc", 65, 65, ref_narrow2 },
    { "rt_bits64_fragment.air.bc",     65, 65, ref_bits64 },
    { "rt_bitsvec_fragment.air.bc",    65, 65, ref_bitsvec },
    { "rt_mulhi_u_fragment.air.bc",    65, 65, ref_mulhi_u },
    { "rt_mulhi_s_fragment.air.bc",    65, 65, ref_mulhi_s },
    { "rt_addsat_u_fragment.air.bc",   65, 65, ref_addsat_u },
    { "rt_addsat_s_fragment.air.bc",   65, 65, ref_addsat_s },
    { "rt_subsat_u_fragment.air.bc",   65, 65, ref_subsat_u },
    { "rt_subsat_s_fragment.air.bc",   65, 65, ref_subsat_s },
    { "rt_sat64a_fragment.air.bc",     65, 65, ref_sat64a },
    { "rt_sat64b_fragment.air.bc",     65, 65, ref_sat64b },
    { "rt_abs_fragment.air.bc",        65, 65, ref_abs },
    { "rt_minmax_fragment.air.bc",     65, 65, ref_minmax },
    { "rt_bitfield_fragment.air.bc",   65, 65, ref_bitfield },
    { "rt_bitfield2_fragment.air.bc",  65, 65, ref_bitfield2 },
    { "rt_bitfield64_fragment.air.bc", 65, 65, ref_bitfield64 },
};

static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q;

static int run_case(const case_t *cs) {
    size_t bn = 0; uint8_t *bc = slurp(cs->fixture, &bn);
    CHECK(bc, "fixture %s not found", cs->fixture);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(bc, bn, &m) == LAGFX_OK && m, "%s: module_open", cs->fixture);
    uint8_t *spv = NULL; size_t sz = 0;
    CHECK(lagfx_air2spv_translate_module(m, &spv, &sz) == LAGFX_OK && spv, "%s: translate", cs->fixture);
    VkDevice dev = g_dev; VkPhysicalDevice pd = g_pd; VkQueue q = g_q;
    const int W = cs->w, H = cs->h;
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .extent = { (uint32_t)W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; VK(vkCreateImage(dev, &imi, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; VK(vkAllocateMemory(dev, &mai, NULL, &imem)); VK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; VK(vkCreateImageView(dev, &vci, NULL, &view));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * H * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf; VK(vkCreateBuffer(dev, &bci, NULL, &buf)); vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(mai.memoryTypeIndex != 0xffffffffu, "no host visible memory");
    VkDeviceMemory bmem; VK(vkAllocateMemory(dev, &mai, NULL, &bmem)); VK(vkBindBufferMemory(dev, buf, bmem, 0));
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VERT_SPV, .pCode = VERT_SPV };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = (const uint32_t *)(const void *)spv };
    VkShaderModule vs, fs; VK(vkCreateShaderModule(dev, &smv, NULL, &vs)); VK(vkCreateShaderModule(dev, &smf, NULL, &fs));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkFormat cf = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl };
    VkPipeline pipe; VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool pool; VK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0.5f, 0.5f, 0.5f, 0.5f } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));
    float *px; VK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float want[4]; cs->ref(x, y, want);
            const float *got = px + ((size_t)y * W + x) * 4;
            if (memcmp(got, want, sizeof want) != 0) {
                if (!bad) printf("%s: first bad pixel (%d,%d): got (%g %g %g %g) want (%g %g %g %g)\n", cs->fixture, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                bad++;
            }
        }
    CHECK(!bad, "%s: %d of %d pixels differ from the CPU reference", cs->fixture, bad, W * H);
    printf("PASS: %s: all %d pixels match the reference\n", cs->fixture, W * H);
    vkUnmapMemory(dev, bmem);
    free(spv); lagfx_air_module_free(m); free(bc);
    return 0;
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "int-intrinsics", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) SKIP("no Vulkan ICD");
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) SKIP("no Vulkan device");
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(inst, &nd, pds);
    VkPhysicalDevice pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_3) SKIP("device is not Vulkan 1.3 (dynamic rendering)");
    VkPhysicalDeviceVulkan12Features s12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 s2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &s12 };
    vkGetPhysicalDeviceFeatures2(pd, &s2);
    if (!s2.features.shaderInt16 || !s2.features.shaderInt64 || !s12.shaderInt8) SKIP("device lacks shaderInt16 / shaderInt64 / shaderInt8");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .shaderInt8 = VK_TRUE };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12, .features = { .shaderInt16 = VK_TRUE, .shaderInt64 = VK_TRUE } };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq };
    VkDevice dev; VK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    g_pd = pd; g_dev = dev; g_q = q;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++)
        if (run_case(&cases[k])) return 1;
    printf("PASS air2spv-int-intrinsics-lavapipe (%zu fixtures) on %s\n", sizeof cases / sizeof cases[0], pp.deviceName);
    return 0;
}
