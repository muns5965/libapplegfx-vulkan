/* AIR atomics -> SPIR-V: scope / storage-class / memory-model consistency, validated with spirv-val.
 * SPDX-License-Identifier: MIT
 *
 * Fixtures: tests/fixtures/atomic_*.air.bc (HAND-WRITTEN IR, NOT Apple output; see gen-atomic-fixtures.sh). No Vulkan device needed.
 *
 * Checked for every translated module (device-memory, threadgroup, 64-bit, helper-function and fragment-stage atomics):
 *   1. each OpAtomic* instruction's pointer has Storage Class StorageBuffer and Scope Device (1), or Workgroup and Scope Workgroup (2) - never any other pairing; the Semantics operand(s)
 *      are the constant 0 (None = memory_order_relaxed);
 *   2. the module declares `OpMemoryModel Logical GLSL450`, no OpCapability VulkanMemoryModel / VulkanMemoryModelDeviceScope and no SPV_KHR_vulkan_memory_model extension: Device scope on
 *      StorageBuffer atomics needs none of them under GLSL450, and the Vulkan device features vulkanMemoryModel / vulkanMemoryModelDeviceScope are therefore NOT required;
 *   3. spirv-val accepts it for vulkan1.2 and vulkan1.3;
 *   4. the same module with the memory model changed to VulkanKHR (capability + extension inserted) is REJECTED by spirv-val for Device scope until
 *      OpCapability VulkanMemoryModelDeviceScope is added: that is what would be silently required if the module ever moved to the Vulkan memory model (a threadgroup-only module is
 *      accepted with VulkanMemoryModel alone). The test records both outcomes so a spirv-val change is noticed.
 * Exit 77 (skip) when spirv-val is not installed, after the structural checks (1, 2) passed.
 */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "air/bitcode_reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)

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

static int translate(const char *name, int lenient, uint8_t **spv, size_t *n) {
    size_t an = 0; uint8_t *air = slurp(name, &an);
    CHECK(air, "fixture %s not found", name);
    lagfx_air_module_t *m = NULL;
    CHECK(lagfx_air_module_open(air, an, &m) == LAGFX_OK && m, "%s: module_open", name);
    lagfx_air2spv_set_atomic_lenient(lenient);
    lagfx_status_t st = lagfx_air2spv_translate_module(m, spv, n);
    lagfx_air2spv_set_atomic_lenient(-1);
    CHECK(st == LAGFX_OK && *spv, "%s: translate = %d", name, (int)st);
    lagfx_air_module_free(m); free(air);
    return 0;
}

/* ---- a small SPIR-V reader: pointer storage classes and constants -------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t n_dev, n_wg, n_other; int unknown; } atomic_census_t;

static int census(const uint32_t *w, size_t n, atomic_census_t *out, int *mm_logical_glsl, int *has_vkmm_cap, int *has_vkmm_ext) {
    uint32_t bound = w[3];
    uint32_t *ptr_sc = calloc(bound, 4), *pty = calloc(bound, 4), *cval = calloc(bound, 4);
    uint8_t *is_ptr_ty = calloc(bound, 1), *is_const = calloc(bound, 1);
    memset(out, 0, sizeof *out); *mm_logical_glsl = 0; *has_vkmm_cap = 0; *has_vkmm_ext = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) { free(ptr_sc); free(pty); free(cval); free(is_ptr_ty); free(is_const); return 1; }
        if (op == 17 && len >= 2 && (w[i + 1] == 5345u || w[i + 1] == 5346u)) *has_vkmm_cap = 1;     /* VulkanMemoryModel, VulkanMemoryModelDeviceScope */
        if (op == 10 && strstr((const char *)&w[i + 1], "vulkan_memory_model")) *has_vkmm_ext = 1;     /* OpExtension */
        if (op == 14 && len >= 3) *mm_logical_glsl = (w[i + 1] == 0u /* Logical */ && w[i + 2] == 1u /* GLSL450 */);
        if (op == 32 && len >= 4) { is_ptr_ty[w[i + 1]] = 1; ptr_sc[w[i + 1]] = w[i + 2]; }            /* OpTypePointer id sc pointee */
        if (op == 43 && len >= 4) { is_const[w[i + 2]] = 1; cval[w[i + 2]] = w[i + 3]; }                /* OpConstant type id value */
        if ((op == 59 || op == 65 || op == 66 || op == 67 || op == 70 || op == 57 || op == 245 || op == 169 || op == 83) && len >= 3) pty[w[i + 2]] = w[i + 1];
        i += len;
    }
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if ((op >= 227u && op <= 242u) || op == 6035u) {
            const int store = op == 228u;
            const uint32_t pid = store ? w[i + 1] : w[i + 3];
            const uint32_t scope_id = store ? w[i + 2] : w[i + 4];
            const uint32_t sem_a = store ? w[i + 3] : w[i + 5];
            uint32_t sc = ~0u;
            if (pid < bound && pty[pid] && is_ptr_ty[pty[pid]]) sc = ptr_sc[pty[pid]];
            const uint32_t scope = (scope_id < bound && is_const[scope_id]) ? cval[scope_id] : ~0u;
            const uint32_t sem = (sem_a < bound && is_const[sem_a]) ? cval[sem_a] : ~0u;
            if (sem != 0u) out->unknown++;                                       /* Semantics must be the constant None */
            if (sc == 12u && scope == 1u) out->n_dev++;                          /* StorageBuffer + Device */
            else if (sc == 4u && scope == 2u) out->n_wg++;                       /* Workgroup + Workgroup */
            else out->n_other++;                                                 /* any other pairing, or an unresolved class: a defect */
        }
        i += len;
    }
    free(ptr_sc); free(pty); free(cval); free(is_ptr_ty); free(is_const);
    return 0;
}

/* ---- spirv-val -------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static int g_have_val = -1;
/* 0 = valid, 1 = rejected (msg = first line of the output), -1 = spirv-val missing */
static int run_val(const void *spv, size_t bytes, const char *env, char *msg, size_t msgn) {
    char tmpl[] = "/tmp/atomic-spv-unit-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    if (write(fd, spv, bytes) != (ssize_t)bytes) { close(fd); unlink(tmpl); return -1; }
    close(fd);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "spirv-val --target-env %s %s 2>&1", env, tmpl);
    FILE *p = popen(cmd, "r");
    int rc = -1;
    if (p) {
        char line[512] = ""; msg[0] = 0;
        if (fgets(line, sizeof line, p)) { snprintf(msg, msgn, "%s", line); size_t l = strlen(msg); if (l && msg[l - 1] == '\n') msg[l - 1] = 0; }
        int st = pclose(p);
        rc = (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) == 127) ? -1 : WEXITSTATUS(st) == 0 ? 0 : 1;
    }
    unlink(tmpl);
    return rc;
}

/* The module with OpMemoryModel Logical VulkanKHR: OpCapability VulkanMemoryModel [+ VulkanMemoryModelDeviceScope] after the last OpCapability, OpExtension SPV_KHR_vulkan_memory_model after
 * the capabilities (SPIR-V 1.4 needs the extension), memory model word = 3. Returns a malloc'd copy. */
static uint32_t *patch_vulkan_model(const uint32_t *w, size_t n, int with_device_scope, size_t *out_n) {
    uint32_t *o = malloc((n + 16) * 4);
    size_t k = 5, i = 5;
    memcpy(o, w, 5 * 4);
    while (i < n && (w[i] & 0xffffu) == 17u) { uint32_t len = w[i] >> 16; memcpy(o + k, w + i, len * 4); k += len; i += len; }
    o[k++] = (2u << 16) | 17u; o[k++] = 5345u;
    if (with_device_scope) { o[k++] = (2u << 16) | 17u; o[k++] = 5346u; }
    { const char *e = "SPV_KHR_vulkan_memory_model"; uint32_t words = (uint32_t)((strlen(e) + 1 + 3) / 4); o[k++] = ((1u + words) << 16) | 10u; memset(o + k, 0, words * 4); memcpy(o + k, e, strlen(e)); k += words; }
    while (i < n) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffffu;
        memcpy(o + k, w + i, len * 4);
        if (op == 14u) o[k + 2] = 3u;   /* VulkanKHR */
        k += len; i += len;
    }
    o[3] = w[3];
    *out_n = k;
    return o;
}

typedef struct { const char *fixture; int lenient; int want_dev, want_wg; } fx_t;

int main(void) {
    static const fx_t fx[] = {
        { "atomic_rmw.air.bc",        0, 1, 0 },   /* device memory, every op */
        { "atomic_cas.air.bc",        0, 1, 0 },   /* synthetic compare-exchange */
        { "atomic_cas_wb.air.bc",     0, 1, 0 },
        { "atomic_tg.air.bc",         0, 0, 1 },   /* threadgroup */
        { "atomic_i64.air.bc",        0, 1, 0 },   /* 64-bit, device memory */
        { "atomic_i64_tg.air.bc",     0, 0, 1 },   /* 64-bit, threadgroup */
        { "atomic_i64_both.air.bc",   0, 1, 1 },
        { "atomic_helper_tg.air.bc",  0, 0, 1 },   /* atomics inside non-entry helper functions */
        { "atomic_frag_discard.air.bc", 0, 1, 0 }, /* fragment stage */
        { "atomic_f32.air.bc",        1, 1, 0 },   /* float add: lenient only */
        { "atomic_f32_tg.air.bc",     1, 0, 1 },
    };
    int rc_skip = 0;
    for (size_t t = 0; t < sizeof fx / sizeof fx[0]; t++) {
        uint8_t *spv = NULL; size_t sn = 0;
        if (translate(fx[t].fixture, fx[t].lenient, &spv, &sn)) return 1;
        const uint32_t *w = (const uint32_t *)(const void *)spv;
        atomic_census_t c; int mm = 0, cap = 0, ext = 0;
        CHECK(!census(w, sn / 4, &c, &mm, &cap, &ext), "%s: malformed SPIR-V", fx[t].fixture);
        CHECK(c.n_other == 0 && c.unknown == 0, "%s: %u atomic(s) with a storage-class / scope pairing other than StorageBuffer+Device / Workgroup+Workgroup, %d with non-None semantics", fx[t].fixture, c.n_other, c.unknown);
        CHECK(fx[t].want_dev ? c.n_dev > 0 : c.n_dev == 0, "%s: StorageBuffer+Device atomics = %u", fx[t].fixture, c.n_dev);
        CHECK(fx[t].want_wg ? c.n_wg > 0 : c.n_wg == 0, "%s: Workgroup+Workgroup atomics = %u", fx[t].fixture, c.n_wg);
        CHECK(mm, "%s: the memory model must be Logical GLSL450", fx[t].fixture);
        CHECK(!cap && !ext, "%s: the module declares a Vulkan-memory-model capability / extension (cap %d ext %d)", fx[t].fixture, cap, ext);
        char msg[512];
        int v12 = run_val(spv, sn, "vulkan1.2", msg, sizeof msg);
        if (v12 < 0) { g_have_val = 0; rc_skip = 1; printf("note: %s: spirv-val not available (structural checks passed)\n", fx[t].fixture); free(spv); continue; }
        CHECK(v12 == 0, "%s: spirv-val vulkan1.2 rejected the module: %s", fx[t].fixture, msg);
        int v13 = run_val(spv, sn, "vulkan1.3", msg, sizeof msg);
        CHECK(v13 == 0, "%s: spirv-val vulkan1.3 rejected the module: %s", fx[t].fixture, msg);
        g_have_val = 1;
        /* the Vulkan-memory-model variant: what would be silently required if the model ever changed */
        size_t pn = 0;
        uint32_t *p1 = patch_vulkan_model(w, sn / 4, 0, &pn);
        int r1 = run_val(p1, pn * 4, "vulkan1.2", msg, sizeof msg);
        const int device_scope_used = c.n_dev > 0;
        if (device_scope_used) {
            CHECK(r1 == 1 && strstr(msg, "VulkanMemoryModelDeviceScope"), "%s: VulkanKHR without the DeviceScope capability was expected to be rejected for Device scope (rc %d: %s)", fx[t].fixture, r1, msg);
        } else {
            CHECK(r1 == 0, "%s: a Workgroup-only module must validate under VulkanKHR with VulkanMemoryModel alone (rc %d: %s)", fx[t].fixture, r1, msg);
        }
        free(p1);
        uint32_t *p2 = patch_vulkan_model(w, sn / 4, 1, &pn);
        int r2 = run_val(p2, pn * 4, "vulkan1.2", msg, sizeof msg);
        CHECK(r2 == 0, "%s: VulkanKHR + VulkanMemoryModel + VulkanMemoryModelDeviceScope must validate (rc %d: %s)", fx[t].fixture, r2, msg);
        free(p2);
        printf("PASS: %-28s dev(StorageBuffer/Device)=%-2u wg(Workgroup/Workgroup)=%-2u  GLSL450, no VulkanMemoryModel*; spirv-val vulkan1.2 + 1.3 OK; VulkanKHR variant: %s\n", fx[t].fixture, c.n_dev, c.n_wg,
               device_scope_used ? "Device scope needs VulkanMemoryModelDeviceScope (rejected without it)" : "valid with VulkanMemoryModel alone");
        free(spv);
    }
    if (rc_skip) { printf("SKIP: spirv-val not installed (structural checks passed)\n"); return 77; }
    printf("PASS air2spv-atomic-spv-unit\n");
    return 0;
}
