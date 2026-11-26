/*
 * libapplegfx-vulkan — DIAGNOSTIC SPIR-V rewrites of the pipeline repro (pure, no device)
 * tests/spv-variants-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Modules assembled with spirv-as (--target-env vulkan1.3) from hand-written SPIR-V assembly (not Apple output). Every rewritten module is also run through spirv-val when
 * that tool is installed (printed when it is not).
 */
#include "vulkan/spv_variants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

/* vertex shader: a 64-bit index in OpVectorExtractDynamic + OpVectorInsertDynamic, an OpUndef %v4float inside the function; Int64 / Int16 / Float16 declared (only Int64 is used) */
static const uint32_t k_t1[] = { 0x07230203,0x00010600,0x00070000,0x00000011,0x00000000,0x00020011,0x00000001,0x00020011,0x0000000b,0x00020011,0x00000016,0x00020011,0x00000009,0x0003000e,0x00000000,0x00000001,0x0006000f,0x00000000,0x00000001,0x6e69616d,0x00000000,0x00000002,0x00040047,0x00000002,0x0000000b,0x00000000,0x00020013,0x00000003,0x00030021,0x00000004,0x00000003,0x00030016,0x00000005,0x00000020,0x00040017,0x00000006,0x00000005,0x00000004,0x00040020,0x00000007,0x00000003,0x00000006,0x0004003b,0x00000007,0x00000002,0x00000003,0x0004002b,0x00000005,0x00000008,0x00000000,0x0004002b,0x00000005,0x00000009,0x3f800000,0x0007002c,0x00000006,0x0000000a,0x00000008,0x00000009,0x00000008,0x00000009,0x00040015,0x0000000b,0x00000040,0x00000000,0x0005002b,0x0000000b,0x0000000c,0x00000001,0x00000000,0x00050036,0x00000003,0x00000001,0x00000000,0x00000004,0x000200f8,0x0000000d,0x0005004d,0x00000005,0x0000000e,0x0000000a,0x0000000c,0x00030001,0x00000006,0x0000000f,0x0006004e,0x00000006,0x00000010,0x0000000f,0x0000000e,0x0000000c,0x0003003e,0x00000002,0x00000010,0x000100fd,0x00010038 };
/* fragment shader: Int64 Int8 Int16 Float16 StorageBuffer16BitAccess declared, none used */
static const uint32_t k_t2[] = { 0x07230203,0x00010600,0x00070000,0x00000005,0x00000000,0x00020011,0x00000001,0x00020011,0x0000000b,0x00020011,0x00000027,0x00020011,0x00000016,0x00020011,0x00000009,0x00020011,0x00001151,0x0003000e,0x00000000,0x00000001,0x0005000f,0x00000004,0x00000001,0x6e69616d,0x00000000,0x00030010,0x00000001,0x00000007,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00050036,0x00000002,0x00000001,0x00000000,0x00000003,0x000200f8,0x00000004,0x000100fd,0x00010038 };

/* compute shader (spirv-as): OpUndef of a pointer (Function, unreferenced), of a struct (module scope, consumed by OpCompositeInsert), of a vec4 (consumed), of a uint (unreferenced), and a struct OpUndef inside the function */
static const uint32_t k_t3[] = { 0x07230203,0x00010600,0x00070000,0x00000014,0x00000000,0x00020011,0x00000001,0x0003000e,0x00000000,0x00000001,0x0005000f,0x00000005,0x00000001,0x6e69616d,0x00000000,0x00060010,0x00000001,0x00000011,0x00000001,0x00000001,0x00000001,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000004,0x00000020,0x00040015,0x00000005,0x00000020,0x00000000,0x00040017,0x00000006,0x00000004,0x00000004,0x0004001e,0x00000007,0x00000004,0x00000005,0x00040020,0x00000008,0x00000007,0x00000005,0x0004002b,0x00000004,0x00000009,0x3f800000,0x00030001,0x00000008,0x0000000a,0x00030001,0x00000007,0x0000000b,0x00030001,0x00000006,0x0000000c,0x00030001,0x00000005,0x0000000d,0x00050036,0x00000002,0x00000001,0x00000000,0x00000003,0x000200f8,0x0000000e,0x00060052,0x00000007,0x0000000f,0x00000009,0x0000000b,0x00000000,0x00050051,0x00000005,0x00000010,0x0000000f,0x00000001,0x0009004f,0x00000006,0x00000011,0x0000000c,0x0000000c,0x00000000,0x00000001,0x00000002,0x00000003,0x00030001,0x00000007,0x00000012,0x00050051,0x00000004,0x00000013,0x00000012,0x00000000,0x000100fd,0x00010038 };

/* fragment shader, SPIR-V 1.4 (spirv-as --target-env spv1.4): entry interface lists an SSBO and two input attachments (bindings 40 / 41, InputAttachmentIndex 0 / 1) besides the output,
 * a helper function that returns an OpUndef %v4float, and an OpSelect of a v4float with a SCALAR bool condition (1.4 only) */
static const uint32_t k_t4[] = { 0x07230203,0x00010400,0x00070000,0x00000024,0x00000000,0x00020011,0x00000001,0x00020011,0x00000028,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000004,0x00000001,0x6e69616d,0x00000000,0x00000002,0x00000003,0x00000004,0x00000005,0x00030010,0x00000001,0x00000007,0x00040047,0x00000002,0x0000001e,0x00000000,0x00030047,0x00000006,0x00000002,0x00050048,0x00000006,0x00000000,0x00000023,0x00000000,0x00040047,0x00000003,0x00000022,0x00000000,0x00040047,0x00000003,0x00000021,0x00000003,0x00040047,0x00000004,0x00000022,0x00000000,0x00040047,0x00000004,0x00000021,0x00000028,0x00040047,0x00000004,0x0000002b,0x00000000,0x00040047,0x00000005,0x00000022,0x00000000,0x00040047,0x00000005,0x00000021,0x00000029,0x00040047,0x00000005,0x0000002b,0x00000001,0x00020013,0x00000007,0x00030021,0x00000008,0x00000007,0x00030016,0x00000009,0x00000020,0x00020014,0x0000000a,0x00040017,0x0000000b,0x00000009,0x00000004,0x00040020,0x0000000c,0x00000003,0x0000000b,0x0004003b,0x0000000c,0x00000002,0x00000003,0x0003001e,0x00000006,0x00000009,0x00040020,0x0000000d,0x0000000c,0x00000006,0x0004003b,0x0000000d,0x00000003,0x0000000c,0x00040015,0x0000000e,0x00000020,0x00000001,0x00040017,0x0000000f,0x0000000e,0x00000002,0x00090019,0x00000010,0x00000009,0x00000006,0x00000000,0x00000000,0x00000000,0x00000002,0x00000000,0x00040020,0x00000011,0x00000000,0x00000010,0x0004003b,0x00000011,0x00000004,0x00000000,0x0004003b,0x00000011,0x00000005,0x00000000,0x0004002b,0x0000000e,0x00000012,0x00000000,0x0005002c,0x0000000f,0x00000013,0x00000012,0x00000012,0x0004002b,0x00000009,0x00000014,0x3f800000,0x0004002b,0x00000009,0x00000015,0x00000000,0x0007002c,0x0000000b,0x00000016,0x00000014,0x00000015,0x00000015,0x00000014,0x00030001,0x0000000b,0x00000017,0x00030021,0x00000018,0x0000000b,0x00050036,0x0000000b,0x00000019,0x00000000,0x00000018,0x000200f8,0x0000001a,0x000200fe,0x00000017,0x00010038,0x00050036,0x00000007,0x00000001,0x00000000,0x00000008,0x000200f8,0x0000001b,0x0004003d,0x00000010,0x0000001c,0x00000004,0x0004003d,0x00000010,0x0000001d,0x00000005,0x00050062,0x0000000b,0x0000001e,0x0000001c,0x00000013,0x00050062,0x0000000b,0x0000001f,0x0000001d,0x00000013,0x000500b8,0x0000000a,0x00000020,0x00000014,0x00000015,0x000600a9,0x0000000b,0x00000021,0x00000020,0x0000001e,0x0000001f,0x00040039,0x0000000b,0x00000022,0x00000019,0x00050081,0x0000000b,0x00000023,0x00000021,0x00000022,0x0003003e,0x00000002,0x00000023,0x000100fd,0x00010038 };

static unsigned count_op(const uint32_t *w, size_t n, unsigned op) { unsigned c = 0; for (size_t i = 5; i < n; i += w[i] >> 16) c += (w[i] & 0xffffu) == op; return c; }
static unsigned count_cap(const uint32_t *w, size_t n, unsigned cap) { unsigned c = 0; for (size_t i = 5; i < n; i += w[i] >> 16) c += (w[i] & 0xffffu) == 17u && w[i + 1] == cap; return c; }

static int validate(const char *what, const uint32_t *w, size_t n) {
    static int have = -1;
    if (have < 0) have = system("command -v spirv-val >/dev/null 2>&1") == 0;
    if (!have) { printf("note: spirv-val not installed, %s not validated\n", what); return 0; }
    char path[] = "/tmp/spvvar-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    FILE *f = fdopen(fd, "wb");
    fwrite(w, 4, n, f); fclose(f);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", path);
    int rc = system(cmd);
    unlink(path);
    CHECK(rc == 0, "spirv-val rejects the %s variant", what);
    return 0;
}

int main(void) {
    char info[200];
    uint32_t *o = NULL; size_t on = 0;
    CHECK(validate("t1 (input)", k_t1, sizeof k_t1 / 4) == 0 && validate("stub vs", lagfx_stub_vs_spv, 67) == 0 && validate("stub fs", lagfx_stub_fs_spv, 32) == 0, "inputs valid");

    /* (e) */
    CHECK(count_op(k_t1, sizeof k_t1 / 4, 113) == 0, "no OpUConvert to start with");
    int r = lagfx_spv_variant_index32(k_t1, sizeof k_t1 / 4, &o, &on, info, sizeof info);
    CHECK(r == 2 && o, "index32: 2 changes expected, got %d (%s)", r, info);
    CHECK(count_op(o, on, 113) == 2 && o[3] == k_t1[3] + 3, "index32: two OpUConvert, bound + 3 (new 32-bit type + 2 ids), bound %u", o[3]);
    CHECK(validate("index32", o, on) == 0, "index32 valid");
    uint32_t *o2 = NULL; size_t on2 = 0;
    CHECK(lagfx_spv_variant_index32(o, on, &o2, &on2, info, sizeof info) == 0 && !o2, "index32 on its own output: nothing left to do");
    free(o); o = NULL;
    CHECK(lagfx_spv_variant_index32(lagfx_stub_vs_spv, 67, &o, &on, info, sizeof info) == 0 && !o, "index32 does not apply to the stub");

    /* (f) */
    r = lagfx_spv_variant_strip_caps(k_t1, sizeof k_t1 / 4, &o, &on, info, sizeof info);
    CHECK(r == 2 && o && count_cap(o, on, 22) == 0 && count_cap(o, on, 9) == 0 && count_cap(o, on, 11) == 1, "strip_caps t1: Int16 + Float16 dropped, Int64 (in use) kept: %d %s", r, info);
    CHECK(strstr(info, "Int64"), "the report names what is still in use: %s", info);
    CHECK(validate("strip_caps t1", o, on) == 0, "strip_caps t1 valid");
    free(o); o = NULL;
    r = lagfx_spv_variant_strip_caps(k_t2, sizeof k_t2 / 4, &o, &on, info, sizeof info);
    CHECK(r == 5 && o && on == sizeof k_t2 / 4 - 10, "strip_caps t2: five capability declarations dropped, got %d", r);
    CHECK(validate("strip_caps t2", o, on) == 0, "strip_caps t2 valid");
    free(o); o = NULL;

    /* (g) */
    CHECK(count_op(k_t1, sizeof k_t1 / 4, 1) == 1 && count_op(k_t1, sizeof k_t1 / 4, 46) == 0, "t1 has one OpUndef");
    r = lagfx_spv_variant_undef_composites(k_t1, sizeof k_t1 / 4, &o, &on, info, sizeof info);
    CHECK(r == 1 && o && count_op(o, on, 1) == 0 && count_op(o, on, 46) == 1 && on == sizeof k_t1 / 4, "undef_composites: %d (%s)", r, info);
    CHECK(validate("undef_composites", o, on) == 0, "undef_composites valid (the null constant is hoisted out of the function)");
    free(o); o = NULL;
    CHECK(lagfx_spv_variant_undef_composites(k_t2, sizeof k_t2 / 4, &o, &on, info, sizeof info) == 0 && !o, "undef_composites does not apply to t2");


    /* (h) only struct / pointer typed OpUndef -> OpConstantNull: the vec4 and uint OpUndef stay, the in-function struct undef is hoisted */
    CHECK(count_op(k_t3, sizeof k_t3 / 4, 1) == 5 && validate("t3 (input)", k_t3, sizeof k_t3 / 4) == 0, "t3 has five OpUndef and is valid");
    r = lagfx_spv_variant_undef_struct_ptr(k_t3, sizeof k_t3 / 4, &o, &on, info, sizeof info);
    CHECK(r == 3 && o && count_op(o, on, 1) == 2 && count_op(o, on, 46) == 3 && on == sizeof k_t3 / 4, "undef_struct_ptr: 3 changes (pointer + 2 struct), 2 OpUndef left: %d (%s)", r, info);
    CHECK(strstr(info, "1 of them hoisted"), "the in-function struct undef was hoisted: %s", info);
    CHECK(validate("undef_struct_ptr", o, on) == 0, "undef_struct_ptr valid");
    free(o); o = NULL;
    CHECK(lagfx_spv_variant_undef_struct_ptr(k_t2, sizeof k_t2 / 4, &o, &on, info, sizeof info) == 0 && !o, "undef_struct_ptr does not apply to t2");
    CHECK(lagfx_spv_variant_undef_struct_ptr(k_t1, sizeof k_t1 / 4, &o, &on, info, sizeof info) == 0 && !o, "undef_struct_ptr leaves the vec4 OpUndef of t1 alone");

    /* (j) struct only: the pointer undef stays, 2 struct undefs become null */
    r = lagfx_spv_variant_undef_struct(k_t3, sizeof k_t3 / 4, &o, &on, info, sizeof info);
    CHECK(r == 2 && o && count_op(o, on, 1) == 3 && count_op(o, on, 46) == 2, "undef_struct: 2 changes, 3 OpUndef left (pointer, vec4, uint): %d (%s)", r, info);
    CHECK(validate("undef_struct", o, on) == 0, "undef_struct valid");
    free(o); o = NULL;

    /* (i) unreferenced OpUndef dropped: the pointer one and the uint one; everything that is consumed stays */
    r = lagfx_spv_variant_drop_dead_undef(k_t3, sizeof k_t3 / 4, &o, &on, info, sizeof info);
    CHECK(r == 2 && o && count_op(o, on, 1) == 3 && on == sizeof k_t3 / 4 - 6, "drop_dead_undef: 2 dropped, 3 left: %d (%s)", r, info);
    CHECK(validate("drop_dead_undef", o, on) == 0, "drop_dead_undef valid");
    free(o); o = NULL;
    CHECK(lagfx_spv_variant_drop_dead_undef(k_t2, sizeof k_t2 / 4, &o, &on, info, sizeof info) == 0 && !o, "drop_dead_undef does not apply to t2");

    /* (k) only OpReturnValue(OpUndef): the returned undef becomes a fresh null, the OpUndef stays */
    CHECK(validate("t4 (input)", k_t4, sizeof k_t4 / 4) == 0 && count_op(k_t4, sizeof k_t4 / 4, 1) == 1, "t4 valid, one OpUndef");
    r = lagfx_spv_variant_undef_returns(k_t4, sizeof k_t4 / 4, &o, &on, info, sizeof info);
    CHECK(r == 1 && o && count_op(o, on, 1) == 1 && count_op(o, on, 46) == 1 && o[3] == k_t4[3] + 1, "undef_returns: 1 change, OpUndef kept, one OpConstantNull, bound +1: %d (%s)", r, info);
    CHECK(validate("undef_returns", o, on) == 0, "undef_returns valid");
    free(o); o = NULL;
    CHECK(lagfx_spv_variant_undef_returns(k_t2, sizeof k_t2 / 4, &o, &on, info, sizeof info) == 0 && !o, "undef_returns does not apply to t2");

    /* (M2) 1.4 -> 1.3: version word, interface trimmed to the Output, the scalar-condition vector select rewritten (OpCompositeConstruct of a bool vector + a new bool vector type) */
    r = lagfx_spv_variant_downgrade13(k_t4, sizeof k_t4 / 4, &o, &on, info, sizeof info);
    CHECK(r == 5 && o && o[1] == 0x00010300u && count_op(o, on, 80) == 1 && count_op(o, on, 169) == 1 && o[3] == k_t4[3] + 2, "downgrade13: version 1.3, 3 ids dropped from the interface + 1 select rewritten: %d (%s)", r, info);
    for (size_t w = 5; w < on; w += o[w] >> 16) if ((o[w] & 0xffffu) == 15u) CHECK((o[w] >> 16) == 6u, "entry point keeps only the Output: %u words", o[w] >> 16);
    {
        char path[] = "/tmp/spvvar-XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0, "mkstemp");
        FILE *f = fdopen(fd, "wb");
        fwrite(o, 4, on, f); fclose(f);
        char cmd[300];
        snprintf(cmd, sizeof cmd, "command -v spirv-val >/dev/null 2>&1 && ! spirv-val --target-env vulkan1.1 %s", path);
        int bad13 = system(cmd) == 0;
        unlink(path);
        CHECK(!bad13, "downgrade13 module is valid for Vulkan 1.1 (SPIR-V 1.3)");
    }
    free(o); o = NULL;
    CHECK(lagfx_spv_variant_downgrade13(k_t2, sizeof k_t2 / 4, &o, &on, info, sizeof info) == 0 && !o, "downgrade13 refuses a module that is not 1.4");

    /* (M3b / M4a) input-attachment renumbering */
    r = lagfx_spv_variant_ia_rebind(k_t4, sizeof k_t4 / 4, 0, 2, &o, &on, info, sizeof info);
    CHECK(r == 3 && o && on == sizeof k_t4 / 4, "ia_rebind mode 0: 3 bindings renumbered (2 attachments + the SSBO): %d (%s)", r, info);
    {
        unsigned seen = 0;
        for (size_t w = 5; w < on; w += o[w] >> 16) if ((o[w] & 0xffffu) == 71u && o[w + 2] == 33u) { seen++; CHECK(o[w + 3] == 0u || o[w + 3] == 1u || o[w + 3] == 5u, "binding %u is 0 / 1 / 5", o[w + 3]); }
        CHECK(seen == 3, "three Binding decorations");
    }
    CHECK(validate("ia_rebind0", o, on) == 0, "ia_rebind mode 0 valid");
    free(o); o = NULL;
    r = lagfx_spv_variant_ia_rebind(k_t4, sizeof k_t4 / 4, 1, 2, &o, &on, info, sizeof info);
    CHECK(r == 4 && o, "ia_rebind mode 1: both attachments to set 1 and binding = index: %d (%s)", r, info);
    CHECK(validate("ia_rebind1", o, on) == 0, "ia_rebind mode 1 valid");
    free(o); o = NULL;
    CHECK(lagfx_spv_variant_ia_rebind(k_t2, sizeof k_t2 / 4, 1, 2, &o, &on, info, sizeof info) == 0 && !o, "ia_rebind does not apply to t2");

    /* malformed input */
    uint32_t bad[6] = { 0x07230203u, 0x10600, 0, 8, 0, 0xffff0001u };
    CHECK(lagfx_spv_variant_index32(bad, 6, &o, &on, NULL, 0) == -1 && lagfx_spv_variant_strip_caps(bad, 6, &o, &on, NULL, 0) == -1 && lagfx_spv_variant_undef_composites(bad, 6, &o, &on, NULL, 0) == -1 && lagfx_spv_variant_undef_struct_ptr(bad, 6, &o, &on, NULL, 0) == -1 && lagfx_spv_variant_drop_dead_undef(bad, 6, &o, &on, NULL, 0) == -1 && lagfx_spv_variant_undef_returns(bad, 6, &o, &on, NULL, 0) == -1 && lagfx_spv_variant_downgrade13(bad, 6, &o, &on, NULL, 0) == -1 && lagfx_spv_variant_ia_rebind(bad, 6, 0, 2, &o, &on, NULL, 0) == -1, "malformed module rejected");
    printf("spv variants: ok\n");
    return 0;
}
