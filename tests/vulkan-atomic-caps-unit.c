/* Atomic device capabilities: separate caps for device-memory and threadgroup-memory 64-bit atomics, per-feature caps for 32-bit float atomics (VK_EXT_shader_atomic_float).
 * SPDX-License-Identifier: MIT
 *
 * No device is needed for parts 1-3 (fake feature masks through lagfx_vk_atomic_feat_t, the same struct lagfx_vk_init fills from the physical device); part 4 runs lagfx_vk_init on the
 * available ICD (lavapipe) with lagfx_vk_atomic_feat_inject lowering the features, and skips (77 after the pure parts passed) without one.
 *
 *   1. lagfx_vk_atomic_caps: shaderBufferInt64Atomics alone -> atomic64_buf only; shaderSharedInt64Atomics alone -> atomic64_shared only; both need shaderInt64; the five float features map
 *      to five caps and need VK_EXT_shader_atomic_float to be advertised.
 *   2. The scan (lagfx_spv_required_caps_ex) attributes Int64Atomics / AtomicFloat32AddEXT to the storage class of the atomic's pointer (StorageBuffer / Workgroup); a declared
 *      capability that no use explains is charged to every class (fail closed).
 *   3. The pipeline gate rule (need & ~enabled): a device with the buffer feature only runs the device-memory 64-bit module and refuses the threadgroup one (and vice versa), each
 *      refusal naming ITS cap (stub_missing_cap_atomic64_shared / _buf, the per-cap refusal counter of the pipeline gate).
 *   4. lagfx_vk_init with injected feature masks: the enabled caps are exactly lagfx_vk_atomic_caps of the probed (and lowered) features, the device is created, and the real float features
 *      of the ICD are enabled one by one (only the supported ones).
 */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air/bitcode_reader.h"
#include "vulkan/caps.h"
#include "vulkan/instance.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define B(c) LAGFX_VK_CAP_BIT(c)

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
/* required caps of the SPIR-V the translator produces for `name` (lenient: the float fixtures are only translated then) */
static int need_of(const char *name, int lenient, uint32_t *need) {
    size_t an = 0; uint8_t *air = slurp(name, &an);
    CHECK(air, "fixture %s not found", name);
    lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t sn = 0;
    CHECK(lagfx_air_module_open(air, an, &m) == LAGFX_OK, "open %s", name);
    lagfx_air2spv_set_atomic_lenient(lenient);
    lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &sn);
    lagfx_air2spv_set_atomic_lenient(-1);
    CHECK(st == LAGFX_OK && spv, "%s: translate = %d", name, (int)st);
    CHECK(lagfx_spv_required_caps_ex(spv, sn, need), "%s: caps scan", name);
    free(spv); lagfx_air_module_free(m); free(air);
    return 0;
}

int main(void) {
    const uint32_t A64B = B(LAGFX_VK_CAP_ATOMIC64_BUF), A64S = B(LAGFX_VK_CAP_ATOMIC64_SHARED);
    const uint32_t FB = B(LAGFX_VK_CAP_ATOMIC_F32_BUF), FBA = B(LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD), FS = B(LAGFX_VK_CAP_ATOMIC_F32_SHARED), FSA = B(LAGFX_VK_CAP_ATOMIC_F32_SHARED_ADD), FI = B(LAGFX_VK_CAP_ATOMIC_F32_IMAGE);
    /* ---- 1. the feature -> cap rule ---- */
    {
        static const struct { const char *what; lagfx_vk_atomic_feat_t f; uint32_t want; } t[] = {
            { "buffer 64-bit only",            { .shader_int64 = 1, .buffer_int64_atomics = 1 }, A64B },
            { "shared 64-bit only",            { .shader_int64 = 1, .shared_int64_atomics = 1 }, A64S },
            { "both 64-bit",                   { .shader_int64 = 1, .buffer_int64_atomics = 1, .shared_int64_atomics = 1 }, A64B | A64S },
            { "neither 64-bit",                { .shader_int64 = 1 }, 0 },
            { "64-bit atomics without shaderInt64", { .buffer_int64_atomics = 1, .shared_int64_atomics = 1 }, 0 },
            { "float features without the extension", { .buffer_f32 = 1, .buffer_f32_add = 1, .shared_f32 = 1, .shared_f32_add = 1, .image_f32 = 1 }, 0 },
            { "buffer float add only",         { .af_ext = 1, .buffer_f32_add = 1 }, FBA },
            { "shared float add only",         { .af_ext = 1, .shared_f32_add = 1 }, FSA },
            { "buffer float load/store only",  { .af_ext = 1, .buffer_f32 = 1 }, FB },
            { "shared float load/store only",  { .af_ext = 1, .shared_f32 = 1 }, FS },
            { "image float only",              { .af_ext = 1, .image_f32 = 1 }, FI },
            { "all five float features",       { .af_ext = 1, .buffer_f32 = 1, .buffer_f32_add = 1, .shared_f32 = 1, .shared_f32_add = 1, .image_f32 = 1 }, FB | FBA | FS | FSA | FI },
        };
        for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
            uint32_t got = lagfx_vk_atomic_caps(&t[i].f);
            CHECK(got == t[i].want, "%s: caps 0x%x, want 0x%x", t[i].what, got, t[i].want);
        }
        CHECK((A64B | A64S | FB | FBA | FS | FSA | FI) == LAGFX_VK_CAPS_ATOMIC_ALL, "LAGFX_VK_CAPS_ATOMIC_ALL does not list every atomic cap");
        CHECK(!strcmp(lagfx_vk_cap_name(LAGFX_VK_CAP_ATOMIC64_BUF), "atomic64_buf") && !strcmp(lagfx_vk_cap_name(LAGFX_VK_CAP_ATOMIC64_SHARED), "atomic64_shared") &&
              lagfx_vk_caps_parse("atomic64_shared,atomic_f32_buf_add") == (A64S | FBA), "cap names / LAGFX_VK_CAPS_OFF parsing");
        CHECK(LAGFX_VK_CAP_COUNT <= 32, "the cap mask and stub_missing_cap[32] hold at most 32 caps");
        printf("PASS: lagfx_vk_atomic_caps: buffer / shared 64-bit are separate caps (both need shaderInt64); the five VK_EXT_shader_atomic_float features give five caps and need the extension (%zu cases)\n", sizeof t / sizeof t[0]);
    }
    /* ---- 2. what a translated module needs ---- */
    uint32_t n_buf64, n_sh64, n_both64, n_fbuf, n_fsh, n_32;
    CHECK(!need_of("atomic_rmw.air.bc", 0, &n_32) && !need_of("atomic_i64.air.bc", 0, &n_buf64) && !need_of("atomic_i64_tg.air.bc", 0, &n_sh64) && !need_of("atomic_i64_both.air.bc", 0, &n_both64) &&
          !need_of("atomic_f32.air.bc", 1, &n_fbuf) && !need_of("atomic_f32_tg.air.bc", 1, &n_fsh), "translate");
    CHECK((n_32 & LAGFX_VK_CAPS_ATOMIC_ALL) == 0u, "32-bit integer atomics need no atomic cap (0x%x)", n_32);
    CHECK((n_buf64 & LAGFX_VK_CAPS_ATOMIC_ALL) == A64B, "device-memory 64-bit: need 0x%x, want atomic64_buf only", n_buf64);
    CHECK((n_sh64 & LAGFX_VK_CAPS_ATOMIC_ALL) == A64S, "threadgroup 64-bit: need 0x%x, want atomic64_shared only", n_sh64);
    CHECK((n_both64 & LAGFX_VK_CAPS_ATOMIC_ALL) == (A64B | A64S), "both: need 0x%x", n_both64);
    CHECK((n_fbuf & LAGFX_VK_CAPS_ATOMIC_ALL) == FBA, "device float add: need 0x%x, want atomic_f32_buf_add only", n_fbuf);
    CHECK((n_fsh & LAGFX_VK_CAPS_ATOMIC_ALL) == FSA, "threadgroup float add: need 0x%x, want atomic_f32_shared_add only", n_fsh);
    {   /* fail closed: Int64Atomics / AtomicFloat32AddEXT declared, no atomic instruction to explain them */
        uint32_t spv[] = { 0x07230203u, 0x00010400u, 0, 8, 0,
                           (2u << 16) | 17u, 1u /* Shader */, (2u << 16) | 17u, 12u /* Int64Atomics */, (2u << 16) | 17u, 6033u /* AtomicFloat32AddEXT */, (3u << 16) | 14u, 0u, 1u };
        uint32_t need = 0;
        CHECK(lagfx_spv_required_caps_ex(spv, sizeof spv, &need), "declared-only module not analysable");
        CHECK((need & (A64B | A64S)) == (A64B | A64S) && (need & (FBA | FSA)) == (FBA | FSA), "declared Int64Atomics / AtomicFloat32AddEXT with no attributable use must need every class (need 0x%x)", need);
    }
    printf("PASS: the scan attributes Int64Atomics / AtomicFloat32AddEXT to the pointer's storage class (device 64-bit -> atomic64_buf, threadgroup 64-bit -> atomic64_shared, float add -> *_buf_add / *_shared_add) and fails closed for an unexplained declaration\n");
    /* ---- 3. the gate (need & ~enabled) on fake devices ---- */
    {
        const uint32_t base = B(LAGFX_VK_CAP_INT64) | B(LAGFX_VK_CAP_SPIRV14);
        lagfx_vk_atomic_feat_t buf_only = { .shader_int64 = 1, .buffer_int64_atomics = 1 }, sh_only = { .shader_int64 = 1, .shared_int64_atomics = 1 };
        uint32_t en_buf = base | lagfx_vk_atomic_caps(&buf_only), en_sh = base | lagfx_vk_atomic_caps(&sh_only);
        uint32_t refusals[LAGFX_VK_CAP_COUNT]; memset(refusals, 0, sizeof refusals);
        struct { const char *dev, *mod; uint32_t en, need, want_miss; } g[] = {
            { "buffer-only", "device 64-bit",      en_buf, n_buf64,  0 },
            { "buffer-only", "threadgroup 64-bit", en_buf, n_sh64,   A64S },
            { "buffer-only", "both",               en_buf, n_both64, A64S },
            { "shared-only", "device 64-bit",      en_sh,  n_buf64,  A64B },
            { "shared-only", "threadgroup 64-bit", en_sh,  n_sh64,   0 },
            { "shared-only", "both",               en_sh,  n_both64, A64B },
            { "buffer-only", "32-bit integer",     en_buf, n_32,     0 },
            { "buffer-only", "device float add",   en_buf, n_fbuf,   FBA },
        };
        for (size_t i = 0; i < sizeof g / sizeof g[0]; i++) {
            uint32_t miss = g[i].need & ~g[i].en;
            CHECK(miss == g[i].want_miss, "%s device, %s module: missing 0x%x, want 0x%x", g[i].dev, g[i].mod, miss, g[i].want_miss);
            for (unsigned c = 0; c < LAGFX_VK_CAP_COUNT; c++) if (miss & B(c)) refusals[c]++;   /* what arm64_pipeline.c does per missing cap: stub_missing_cap[c]++ */
        }
        CHECK(refusals[LAGFX_VK_CAP_ATOMIC64_SHARED] == 2u && refusals[LAGFX_VK_CAP_ATOMIC64_BUF] == 2u && refusals[LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD] == 1u, "per-cap refusal counters %u / %u / %u", refusals[LAGFX_VK_CAP_ATOMIC64_SHARED], refusals[LAGFX_VK_CAP_ATOMIC64_BUF], refusals[LAGFX_VK_CAP_ATOMIC_F32_BUF_ADD]);
        printf("PASS: gate: a buffer-only device runs the device-memory 64-bit module and refuses the threadgroup one (stub_missing_cap_atomic64_shared), a shared-only device the opposite (stub_missing_cap_atomic64_buf); 32-bit atomics always pass\n");
    }
    /* ---- 4. lagfx_vk_init with injected features ---- */
    {
        struct lagfx_vk_state *vk = NULL;
        lagfx_device_descriptor_t dd; memset(&dd, 0, sizeof dd);
        unsetenv("LAGFX_VK_CAPS_OFF");
        lagfx_vk_atomic_feat_inject = NULL;
        if (lagfx_vk_init(&vk, &dd) != LAGFX_OK || !vk || !vk->initialized) { printf("note: no usable Vulkan device: the lagfx_vk_init part was skipped\n"); printf("PASS (pure parts) vulkan-atomic-caps-unit\n"); return 77; }
        /* what the ICD itself reports */
        VkPhysicalDeviceShaderAtomicFloatFeaturesEXT paf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT };
        VkPhysicalDeviceVulkan12Features p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &paf };
        VkPhysicalDeviceFeatures2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &p12 };
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(vk->phys_device, NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep); vkEnumerateDeviceExtensionProperties(vk->phys_device, NULL, &ne, ep);
        int has_af = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_EXT_shader_atomic_float")) has_af = 1;
        free(ep);
        if (!has_af) paf.sType = (VkStructureType)0, p12.pNext = NULL;
        vkGetPhysicalDeviceFeatures2(vk->phys_device, &p2);
        lagfx_vk_atomic_feat_t real = { .shader_int64 = p2.features.shaderInt64 == VK_TRUE, .buffer_int64_atomics = p12.shaderBufferInt64Atomics == VK_TRUE, .shared_int64_atomics = p12.shaderSharedInt64Atomics == VK_TRUE,
            .af_ext = has_af, .buffer_f32 = has_af && paf.shaderBufferFloat32Atomics, .buffer_f32_add = has_af && paf.shaderBufferFloat32AtomicAdd, .shared_f32 = has_af && paf.shaderSharedFloat32Atomics,
            .shared_f32_add = has_af && paf.shaderSharedFloat32AtomicAdd, .image_f32 = has_af && paf.shaderImageFloat32Atomics };
        const uint32_t real_caps = lagfx_vk_atomic_caps(&real);
        CHECK((vk->caps_enabled & LAGFX_VK_CAPS_ATOMIC_ALL) == real_caps, "lagfx_vk_init enabled atomic caps 0x%x, the ICD reports 0x%x", vk->caps_enabled & LAGFX_VK_CAPS_ATOMIC_ALL, real_caps);
        printf("PASS: lagfx_vk_init on the ICD: atomic caps enabled = exactly what it reports: 0x%x (atomic64_buf %d, atomic64_shared %d, f32: buf %d buf_add %d shared %d shared_add %d image %d; VK_EXT_shader_atomic_float %s)\n", real_caps,
               !!(real_caps & A64B), !!(real_caps & A64S), !!(real_caps & FB), !!(real_caps & FBA), !!(real_caps & FS), !!(real_caps & FSA), !!(real_caps & FI), has_af ? "present" : "absent");
        lagfx_vk_shutdown(vk); vk = NULL;
        static const struct { const char *what; lagfx_vk_atomic_feat_t inj; } inj[] = {
            { "buffer 64-bit only", { .shader_int64 = 1, .buffer_int64_atomics = 1, .shared_int64_atomics = 0, .af_ext = 1, .buffer_f32 = 1, .buffer_f32_add = 1, .shared_f32 = 1, .shared_f32_add = 1, .image_f32 = 1 } },
            { "shared 64-bit only", { .shader_int64 = 1, .buffer_int64_atomics = 0, .shared_int64_atomics = 1, .af_ext = 1, .buffer_f32 = 1, .buffer_f32_add = 1, .shared_f32 = 1, .shared_f32_add = 1, .image_f32 = 1 } },
            { "no 64-bit, float: buffer add only", { .shader_int64 = 1, .af_ext = 1, .buffer_f32_add = 1 } },
            { "no float extension", { .shader_int64 = 1, .buffer_int64_atomics = 1, .shared_int64_atomics = 1 } },
            { "nothing", { 0 } },
        };
        for (size_t i = 0; i < sizeof inj / sizeof inj[0]; i++) {
            lagfx_vk_atomic_feat_inject = &inj[i].inj;
            lagfx_status_t st = lagfx_vk_init(&vk, &dd);
            CHECK(st == LAGFX_OK && vk && vk->initialized, "lagfx_vk_init failed with the '%s' injection (a device missing one feature must still be created)", inj[i].what);
            lagfx_vk_atomic_feat_t eff = real;   /* the injection only lowers: real AND injected */
            eff.shader_int64 &= inj[i].inj.shader_int64; eff.buffer_int64_atomics &= inj[i].inj.buffer_int64_atomics; eff.shared_int64_atomics &= inj[i].inj.shared_int64_atomics; eff.af_ext &= inj[i].inj.af_ext;
            eff.buffer_f32 &= inj[i].inj.buffer_f32; eff.buffer_f32_add &= inj[i].inj.buffer_f32_add; eff.shared_f32 &= inj[i].inj.shared_f32; eff.shared_f32_add &= inj[i].inj.shared_f32_add; eff.image_f32 &= inj[i].inj.image_f32;
            const uint32_t want = lagfx_vk_atomic_caps(&eff);
            CHECK((vk->caps_enabled & LAGFX_VK_CAPS_ATOMIC_ALL) == want, "'%s': enabled atomic caps 0x%x, want 0x%x", inj[i].what, vk->caps_enabled & LAGFX_VK_CAPS_ATOMIC_ALL, want);
            if (!(inj[i].inj.shared_int64_atomics)) CHECK(!(vk->caps_enabled & A64S), "'%s': atomic64_shared must be off", inj[i].what);
            if (!(inj[i].inj.buffer_int64_atomics)) CHECK(!(vk->caps_enabled & A64B), "'%s': atomic64_buf must be off", inj[i].what);
            printf("PASS: lagfx_vk_init with '%s': atomic caps 0x%x (device created)\n", inj[i].what, want);
            lagfx_vk_shutdown(vk); vk = NULL;
        }
        lagfx_vk_atomic_feat_inject = NULL;
        /* LAGFX_VK_CAPS_OFF=int64 also takes the 64-bit atomic caps away (they need shaderInt64) */
        setenv("LAGFX_VK_CAPS_OFF", "int64", 1);
        CHECK(lagfx_vk_init(&vk, &dd) == LAGFX_OK && vk && vk->initialized, "init with LAGFX_VK_CAPS_OFF=int64");
        CHECK(!(vk->caps_enabled & (A64B | A64S)), "LAGFX_VK_CAPS_OFF=int64 must drop the 64-bit atomic caps (0x%x)", vk->caps_enabled);
        lagfx_vk_shutdown(vk); vk = NULL;
        unsetenv("LAGFX_VK_CAPS_OFF");
        printf("PASS: LAGFX_VK_CAPS_OFF=int64 drops atomic64_buf and atomic64_shared with it\n");
    }
    printf("PASS vulkan-atomic-caps-unit\n");
    return 0;
}
