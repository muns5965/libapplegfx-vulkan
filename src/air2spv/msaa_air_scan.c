/* libapplegfx-vulkan — AIR multisample operations and builtins: detection and counting only (see msaa_air_scan.h). SPDX-License-Identifier: MIT */
#include "msaa_air_scan.h"

#include <stdio.h>
#include <string.h>

static struct { uint64_t modules, ms_ops, sample_builtins; } g_tot;

int lagfx_air_msaa_is_ms_op(const char *n) {
    if (!n || strncmp(n, "air.", 4) != 0) return 0;
    if (strstr(n, "num_samples")) return 1;
    for (const char *q = strstr(n, "_ms"); q; q = strstr(q + 1, "_ms")) {
        const char c = q[3];
        if (c == '\0' || c == '.' || c == '_') return 1;   /* texture_2d_ms, texture_2d_ms_array, ..._ms.v4f32 (not "_msg" / "_mst") */
    }
    return 0;
}

int lagfx_air_msaa_is_sample_builtin(const char *s) {
    if (!s) return 0;
    static const char *const k[] = { "air.sample_id", "air.sample_mask", "air.sample_position", "air.get_sample_position", "air.set_sample_mask" };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (!strncmp(s, k[i], strlen(k[i]))) return 1;
    return 0;
}

void lagfx_air2spv_msaa_scan(const lagfx_air_module_t *m, lagfx_air_msaa_scan_t *out) {
    lagfx_air_msaa_scan_t o; memset(&o, 0, sizeof o);
    if (m) {
        uint32_t nf = 0;
        const lagfx_air_function_t *f = lagfx_air_module_functions(m, &nf);
        for (uint32_t i = 0; f && i < nf; i++) {
            if (!f[i].name_length) continue;
            const char *nm = lagfx_air_module_string(m, f[i].name_offset);
            if (lagfx_air_msaa_is_ms_op(nm)) { if (!o.first[0]) snprintf(o.first, sizeof o.first, "%s", nm); o.ms_ops++; }
        }
        uint32_t ns = 0;
        const char *const *st = lagfx_air_module_metadata_strings(m, &ns);
        for (uint32_t i = 0; st && i < ns; i++)
            if (lagfx_air_msaa_is_sample_builtin(st[i])) { if (!o.first[0]) snprintf(o.first, sizeof o.first, "%s", st[i]); o.sample_builtins++; }
    }
    if (o.ms_ops || o.sample_builtins) {
        __atomic_fetch_add(&g_tot.modules, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_tot.ms_ops, o.ms_ops, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_tot.sample_builtins, o.sample_builtins, __ATOMIC_RELAXED);
    }
    if (out) *out = o;
}

void lagfx_air2spv_msaa_totals(lagfx_air_msaa_totals_t *out) {
    if (!out) return;
    out->modules = __atomic_load_n(&g_tot.modules, __ATOMIC_RELAXED);
    out->ms_ops = __atomic_load_n(&g_tot.ms_ops, __ATOMIC_RELAXED);
    out->sample_builtins = __atomic_load_n(&g_tot.sample_builtins, __ATOMIC_RELAXED);
}
