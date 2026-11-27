/*
 * libapplegfx-vulkan — Phase 5 AIR-module → SPIR-V translator (skeleton)
 * src/air2spv/translate.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * MVP: uses Phase 3 named-metadata helpers to identify the entry-point
 * stage, then dispatches to a reference emitter that produces a
 * constant-output shader of the matching shape. The actual semantic
 * translation of the function body is Phase 5 work that lands in
 * subsequent commits — this skeleton establishes the API surface so
 * op_0x74 (Phase 6) has something to wire against without waiting.
 */

#include "translate.h"
#include "msaa_air_scan.h"
#include <string.h>
#include "emit_position.h"
#include "emit_render_target.h"
#include "translate_function.h"

#include "common/log.h"
#include "vulkan/caps.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

typedef enum {
    LAGFX_TRANS_STAGE_UNKNOWN  = 0,
    LAGFX_TRANS_STAGE_VERTEX   = 1,
    LAGFX_TRANS_STAGE_FRAGMENT = 2,
    LAGFX_TRANS_STAGE_COMPUTE  = 3,   /* air.kernel: a compute kernel or a tile function */
} lagfx_translate_stage_t;

static lagfx_translate_stage_t discover_stage(const lagfx_air_module_t *m) {
    /* Apple's AIR convention: the metallib's entry-point function is
     * named via one of these well-known METADATA_NAMED_NODE records.
     * Each named-node operands list references the function's
     * description-NODE through one or more metadata-IDs.
     *
     * On triangle, "air.vertex" → NODE → function descriptor that
     * pins the stage. We don't need to walk into the descriptor for
     * the MVP — the named-node's existence is enough. */
    if (lagfx_air_module_named_metadata(m, "air.vertex"))   return LAGFX_TRANS_STAGE_VERTEX;
    if (lagfx_air_module_named_metadata(m, "air.fragment")) return LAGFX_TRANS_STAGE_FRAGMENT;
    if (lagfx_air_module_named_metadata(m, "air.kernel"))   return LAGFX_TRANS_STAGE_COMPUTE;
    return LAGFX_TRANS_STAGE_UNKNOWN;
}

/* === Entry-point arg-metadata walk (Metal resource indexes) ========
 *
 * LLVM/AIR metadata ID rules (validated against llvm.module.flags on the
 * login corpus: {Max, 'air.max_device_buffers', 31} resolves exactly):
 *   - NAMED_NODE operands are RAW global metadata IDs.
 *   - NODE operands are global-ID + 1 (0 = null).
 *   - global ID < num_strings  -> strings[id]; else metadata[id-num_strings].
 *   - VALUE records are [type_index, value_id]; value_id enumerates
 *     globalvars, then functions, then module constants — so constant
 *     index = value_id - (num_globalvars + num_functions).
 *   - CST_NULL means integer 0 (LLVM emits i32 0 as CST_CODE_NULL).
 *
 * The entry-point node (air.vertex/air.fragment op) is
 *   [fn VALUE, outputs NODE, args NODE]
 * and each per-arg NODE is a key/value list over the strings pool:
 *   [argidx VALUE, 'air.buffer'|'air.vertex_input'|..., 'air.location_index',
 *    <loc VALUE>, ...] */

static const lagfx_air_metadata_t *
md_record(const lagfx_air_module_t *m, uint32_t global_id) {
    uint32_t nms = 0, nmd = 0;
    (void)lagfx_air_module_metadata_strings(m, &nms);
    const lagfx_air_metadata_t *mds = lagfx_air_module_metadata(m, &nmd);
    if (!mds || global_id < nms || global_id - nms >= nmd) return NULL;
    return &mds[global_id - nms];
}

static const char *
md_string(const lagfx_air_module_t *m, uint32_t global_id) {
    uint32_t nms = 0;
    const char *const *strs = lagfx_air_module_metadata_strings(m, &nms);
    if (!strs || global_id >= nms) return NULL;
    return strs[global_id];
}

/* Resolve a VALUE metadata record to its integer constant, or -1. */
static int64_t
md_value_int(const lagfx_air_module_t *m, const lagfx_air_metadata_t *v) {
    if (!v || v->kind != LAGFX_AIR_MD_VALUE || v->num_operands < 2u) return -1;
    uint32_t value_id = v->operands[1];
    uint32_t ng = lagfx_air_module_num_globalvars(m);
    uint32_t nf = 0;
    (void)lagfx_air_module_functions(m, &nf);
    uint32_t base = ng + nf;
    if (value_id < base) return -1;
    uint32_t nc = 0;
    const lagfx_air_constant_t *cs = lagfx_air_module_constants(m, &nc);
    if (!cs || value_id - base >= nc) return -1;
    const lagfx_air_constant_t *c = &cs[value_id - base];
    if (c->kind == LAGFX_AIR_CONST_INTEGER) return c->payload.i64;
    if (c->kind == LAGFX_AIR_CONST_NULL)    return 0;
    return -1;
}

size_t
lagfx_air_arg_bindings(const lagfx_air_module_t *m,
                        lagfx_air_arg_binding_t  *out,
                        size_t                    max) {
    if (!m || !out || max == 0u) return 0u;
    const lagfx_air_metadata_t *named = lagfx_air_module_named_metadata(m, "air.vertex");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.fragment");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.kernel");
    if (!named || named->num_operands < 1u) return 0u;
    /* Entry-point node: NAMED_NODE ops are raw IDs. */
    const lagfx_air_metadata_t *ep = md_record(m, named->operands[0]);
    if (!ep || ep->kind != LAGFX_AIR_MD_NODE || ep->num_operands < 3u) return 0u;
    /* [fn, outputs, args] — args node ops are ID+1. */
    uint32_t args_id = ep->operands[2];
    if (args_id == 0u) return 0u;
    const lagfx_air_metadata_t *args = md_record(m, args_id - 1u);
    if (!args || args->kind != LAGFX_AIR_MD_NODE) return 0u;

    size_t n_out = 0u;
    for (uint32_t a = 0; a < args->num_operands && n_out < max; a++) {
        if (args->operands[a] == 0u) continue;
        const lagfx_air_metadata_t *arg = md_record(m, args->operands[a] - 1u);
        if (!arg || arg->kind != LAGFX_AIR_MD_NODE) continue;
        uint8_t kind = 0;      /* 1=buffer 2=texture 3=sampler */
        int64_t loc = -1;
        bool is_stagein = false, written = false;
        for (uint32_t j = 0; j < arg->num_operands; j++) {
            if (arg->operands[j] == 0u) continue;
            const char *s = md_string(m, arg->operands[j] - 1u);
            if (!s) continue;
            if (strcmp(s, "air.buffer") == 0)            kind = kind ? kind : 1u;
            else if (strcmp(s, "air.texture") == 0)      kind = kind ? kind : 2u;
            else if (strcmp(s, "air.sampler") == 0)      kind = kind ? kind : 3u;
            else if (strcmp(s, "air.write") == 0 || strcmp(s, "air.read_write") == 0)
                written = true;
            else if (strcmp(s, "air.vertex_input") == 0
                     || strcmp(s, "air.position") == 0
                     || strcmp(s, "air.vertex_id") == 0
                     || strcmp(s, "air.fragment_input") == 0)
                is_stagein = true;
            else if (strcmp(s, "air.location_index") == 0
                     && j + 1u < arg->num_operands
                     && arg->operands[j + 1u] != 0u) {
                loc = md_value_int(m, md_record(m, arg->operands[j + 1u] - 1u));
            }
        }
        if (is_stagein || kind == 0u) continue;   /* stage-in attr / vid — not a resource */
        out[n_out].kind        = kind;
        out[n_out].metal_index = (loc >= 0 && loc < 0x7fff) ? (int16_t)loc : -1;
        out[n_out].written     = ((kind == 2u || kind == 1u) && written) ? 1u : 0u;   /* texture: storage image; buffer: the pipeline may write it */
        n_out++;
    }
    return n_out;
}

bool lagfx_air_arg_is_written_texture(const lagfx_air_module_t *m, uint32_t arg_idx) {
    if (!m) return false;
    const lagfx_air_metadata_t *named = lagfx_air_module_named_metadata(m, "air.vertex");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.fragment");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.kernel");
    if (!named || named->num_operands < 1u) return false;
    const lagfx_air_metadata_t *ep = md_record(m, named->operands[0]);
    if (!ep || ep->kind != LAGFX_AIR_MD_NODE || ep->num_operands < 3u || ep->operands[2] == 0u) return false;
    const lagfx_air_metadata_t *args = md_record(m, ep->operands[2] - 1u);
    if (!args || args->kind != LAGFX_AIR_MD_NODE || arg_idx >= args->num_operands || args->operands[arg_idx] == 0u)
        return false;
    const lagfx_air_metadata_t *arg = md_record(m, args->operands[arg_idx] - 1u);
    if (!arg || arg->kind != LAGFX_AIR_MD_NODE) return false;
    bool tex = false, wr = false;
    for (uint32_t j = 0; j < arg->num_operands; j++) {
        if (arg->operands[j] == 0u) continue;
        const char *s = md_string(m, arg->operands[j] - 1u);
        if (!s) continue;
        if (strcmp(s, "air.texture") == 0) tex = true;
        else if (strcmp(s, "air.write") == 0 || strcmp(s, "air.read_write") == 0) wr = true;
    }
    return tex && wr;
}

/* True when entry-point argument `arg_idx` is a `texture3d<...>` (its air.arg_type_name string starts with "texture3d"). Under opaque pointers the LLVM pointee type does not
 * tell texture2d from texture3d, the argument metadata does. */
bool lagfx_air_arg_is_texture3d(const lagfx_air_module_t *m, uint32_t arg_idx) {
    if (!m) return false;
    const lagfx_air_metadata_t *named = lagfx_air_module_named_metadata(m, "air.vertex");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.fragment");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.kernel");
    if (!named || named->num_operands < 1u) return false;
    const lagfx_air_metadata_t *ep = md_record(m, named->operands[0]);
    if (!ep || ep->kind != LAGFX_AIR_MD_NODE || ep->num_operands < 3u || ep->operands[2] == 0u) return false;
    const lagfx_air_metadata_t *args = md_record(m, ep->operands[2] - 1u);
    if (!args || args->kind != LAGFX_AIR_MD_NODE || arg_idx >= args->num_operands || args->operands[arg_idx] == 0u) return false;
    const lagfx_air_metadata_t *arg = md_record(m, args->operands[arg_idx] - 1u);
    if (!arg || arg->kind != LAGFX_AIR_MD_NODE) return false;
    bool tex = false, t3 = false;
    for (uint32_t j = 0; j < arg->num_operands; j++) {
        if (arg->operands[j] == 0u) continue;
        const char *str = md_string(m, arg->operands[j] - 1u);
        if (!str) continue;
        if (strcmp(str, "air.texture") == 0) tex = true;
        else if (strncmp(str, "texture3d", 9) == 0) t3 = true;
    }
    return tex && t3;
}

/* True when entry-point argument `arg_idx` is a `texture2d_array<...>` (its air.arg_type_name string starts with "texture2d_array" (INFER by analogy with texture3d: no fixture / capture carries an array texture)). Under opaque pointers the LLVM pointee type does not
 * tell texture2d from texture3d, the argument metadata does. */
bool lagfx_air_arg_is_texture_array(const lagfx_air_module_t *m, uint32_t arg_idx) {
    if (!m) return false;
    const lagfx_air_metadata_t *named = lagfx_air_module_named_metadata(m, "air.vertex");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.fragment");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.kernel");
    if (!named || named->num_operands < 1u) return false;
    const lagfx_air_metadata_t *ep = md_record(m, named->operands[0]);
    if (!ep || ep->kind != LAGFX_AIR_MD_NODE || ep->num_operands < 3u || ep->operands[2] == 0u) return false;
    const lagfx_air_metadata_t *args = md_record(m, ep->operands[2] - 1u);
    if (!args || args->kind != LAGFX_AIR_MD_NODE || arg_idx >= args->num_operands || args->operands[arg_idx] == 0u) return false;
    const lagfx_air_metadata_t *arg = md_record(m, args->operands[arg_idx] - 1u);
    if (!arg || arg->kind != LAGFX_AIR_MD_NODE) return false;
    bool tex = false, ta = false;
    for (uint32_t j = 0; j < arg->num_operands; j++) {
        if (arg->operands[j] == 0u) continue;
        const char *str = md_string(m, arg->operands[j] - 1u);
        if (!str) continue;
        if (strcmp(str, "air.texture") == 0) tex = true;
        else if (strncmp(str, "texture2d_array", 15) == 0) ta = true;
    }
    return tex && ta;
}

/* Key of one per-arg / per-output metadata NODE: the Metal link name. The
 * "air.position" / "air.point_size" / "air.point_coord" markers key
 * themselves; a user varying is keyed by its "generated(...)" string, the
 * string right after "air.vertex_output" / "air.fragment_input". */
static void md_iface_key(const lagfx_air_module_t *m, const lagfx_air_metadata_t *node,
                         char *out, size_t cap) {
    out[0] = 0;
    bool after_io = false;
    for (uint32_t j = 0; j < node->num_operands; j++) {
        if (node->operands[j] == 0u) continue;
        const char *s = md_string(m, node->operands[j] - 1u);
        if (!s) continue;
        if (strcmp(s, "air.position") == 0 || strcmp(s, "air.point_size") == 0 ||
            strcmp(s, "air.point_coord") == 0 ||
            strcmp(s, "air.vertex_id") == 0 || strcmp(s, "air.instance_id") == 0) { snprintf(out, cap, "%s", s); return; }
        if (strcmp(s, "air.render_target") == 0) {   /* "air.render_target:<index>": the colour attachment a fragment input (dest_N, framebuffer fetch) or output refers to */
            int64_t idx = (j + 1u < node->num_operands && node->operands[j + 1u]) ? md_value_int(m, md_record(m, node->operands[j + 1u] - 1u)) : -1;
            if (idx >= 0 && idx < 8) snprintf(out, cap, "%s:%d", s, (int)idx); else snprintf(out, cap, "%s", s);
            return;
        }
        if (strcmp(s, "air.vertex_output") == 0 || strcmp(s, "air.fragment_input") == 0) { after_io = true; continue; }
        if (after_io) {
            if (strncmp(s, "air.", 4) != 0) snprintf(out, cap, "%s", s);
            return;
        }
    }
}

/* Key of one kernel argument: the first "air.*" tag of its metadata NODE -- "air.buffer", "air.texture", "air.imageblock", or the thread-position
 * builtin ("air.threadgroup_position_in_grid", "air.thread_index_in_simdgroup", ...). The leading i32 is the argument index, not a tag. */
static void md_kernel_key(const lagfx_air_module_t *m, const lagfx_air_metadata_t *node, char *out, size_t cap) {
    out[0] = 0;
    for (uint32_t j = 0; j < node->num_operands; j++) {
        if (node->operands[j] == 0u) continue;
        const char *s = md_string(m, node->operands[j] - 1u);
        if (s && strncmp(s, "air.", 4) == 0) { snprintf(out, cap, "%s", s); return; }
    }
}

/* The entry function of a module: the first non-prototype function that is not a C++ static initializer. (uint32_t)-1 when there is none. */
static uint32_t pick_body_fn(const lagfx_air_module_t *m) {
    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &n_fns);
    uint32_t body_fn = (uint32_t)-1;
    for (uint32_t i = 0; i < n_fns; i++) {
        if (fns[i].is_proto || fns[i].body_offset == 0u) continue;
        /* Skip C++ static-initializer functions (`_GLOBAL__sub_I_*.metal`, void(), section "air.static_init"). Composite SkyLight shaders carry one BEFORE the
         * real entry shader (function-constant init); picking the first body function then translated the static-init (num_args=0, no resources) instead of
         * e.g. UberCompositeFragment -> every texture + buffer dropped -> black. The real entry shader is the non-static-init body function. */
        const char *nm = lagfx_air_module_string(m, fns[i].name_offset);
        if (nm && (strstr(nm, "_GLOBAL__sub_I") || strstr(nm, "global_ctors") || strstr(nm, ".static_init"))) {
            LAGFX_TRACE("air2spv: skipping static-init fn[%u] '%s'", i, nm);
            continue;
        }
        body_fn = i; break;
    }
    return body_fn;
}

/* Shape of the Output variable the vertex translator declares for a member of AIR type `ty` (mirrors vertex_output_type_spv_idx + iface_var_type in
 * translate_function.c: a float/half member or a float/half vector is a float32 scalar/vector, a 32-bit integer vector keeps its type, any other member type
 * falls back to float4). Only 2..4 lanes are modelled for vectors. */
static void out_shape(const lagfx_air_type_t *ts, uint32_t nt, uint32_t ty, uint8_t *kind, uint8_t *lanes) {
    *kind = LAGFX_SHAPE_NONE; *lanes = 0;
    if (ty >= nt) return;
    if (ts[ty].kind == LAGFX_AIR_TYPE_FLOAT || ts[ty].kind == LAGFX_AIR_TYPE_HALF) { *kind = LAGFX_SHAPE_FLOAT; *lanes = 1; return; }
    if (ts[ty].kind == LAGFX_AIR_TYPE_VECTOR && ts[ty].num_op >= 2u && ts[ty].op[1] < nt && ts[ty].op[0] >= 2u && ts[ty].op[0] <= 4u) {
        lagfx_air_type_kind_t ek = ts[ts[ty].op[1]].kind;
        if (ek == LAGFX_AIR_TYPE_FLOAT || ek == LAGFX_AIR_TYPE_HALF) { *kind = LAGFX_SHAPE_FLOAT; *lanes = (uint8_t)ts[ty].op[0]; }
        else if (ek == LAGFX_AIR_TYPE_INTEGER && ts[ts[ty].op[1]].num_op >= 1u && ts[ts[ty].op[1]].op[0] == 32u) { *kind = LAGFX_SHAPE_INT; *lanes = (uint8_t)ts[ty].op[0]; }
        return;
    }
    if (ts[ty].kind != LAGFX_AIR_TYPE_VECTOR) { *kind = LAGFX_SHAPE_FLOAT; *lanes = 4; }   /* translator default for a non float/vector member (e.g. a scalar int): float4 */
}

static void fill_out_shapes(const lagfx_air_module_t *m, lagfx_stage_keys_t *out) {
    uint32_t nt = 0; const lagfx_air_type_t *ts = lagfx_air_module_types(m, &nt);
    uint32_t nf = 0; const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &nf);
    uint32_t bi = pick_body_fn(m);
    if (bi == (uint32_t)-1 || bi >= nf) return;
    uint32_t fty = fns[bi].type_index;
    if (fty >= nt || ts[fty].kind != LAGFX_AIR_TYPE_FUNCTION || ts[fty].num_op < 2u) return;
    uint32_t rt = ts[fty].op[1];
    if (rt >= nt || (ts[rt].kind != LAGFX_AIR_TYPE_STRUCT_ANON && ts[rt].kind != LAGFX_AIR_TYPE_STRUCT_NAMED)) return;
    uint32_t nmem = ts[rt].num_op > 1u ? ts[rt].num_op - 1u : 0u;
    for (uint32_t k = 0; k < out->n && k < nmem; k++) out_shape(ts, nt, ts[rt].op[1u + k], &out->kind[k], &out->lanes[k]);
    out->typed = 1;
}

bool
lagfx_air_stage_keys(const lagfx_air_module_t *m, bool outputs, lagfx_stage_keys_t *out) {
    if (!m || !out) return false;
    memset(out, 0, sizeof *out);
    const lagfx_air_metadata_t *named = lagfx_air_module_named_metadata(m, "air.vertex");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.fragment");
    bool kernel = false;
    if (!named) { named = lagfx_air_module_named_metadata(m, "air.kernel"); kernel = named != NULL; }
    if (!named || named->num_operands < 1u) return false;
    const lagfx_air_metadata_t *ep = md_record(m, named->operands[0]);
    if (!ep || ep->kind != LAGFX_AIR_MD_NODE || ep->num_operands < 3u) return false;
    uint32_t id = outputs ? ep->operands[1] : ep->operands[2];
    if (id == 0u) return false;
    const lagfx_air_metadata_t *list = md_record(m, id - 1u);
    if (!list || list->kind != LAGFX_AIR_MD_NODE) return false;
    if (list->num_operands > LAGFX_STAGE_KEYS_MAX) LAGFX_WARN("air2spv: stage-key list has %u entries, only the first %u are keyed", list->num_operands, (unsigned)LAGFX_STAGE_KEYS_MAX);
    for (uint32_t a = 0; a < list->num_operands && out->n < LAGFX_STAGE_KEYS_MAX; a++) {
        const lagfx_air_metadata_t *e = list->operands[a] ? md_record(m, list->operands[a] - 1u) : NULL;
        if (e && e->kind == LAGFX_AIR_MD_NODE) {
            if (kernel) md_kernel_key(m, e, out->key[out->n], LAGFX_STAGE_KEY_LEN);
            else        md_iface_key(m, e, out->key[out->n], LAGFX_STAGE_KEY_LEN);
        }
        out->n++;
    }
    if (outputs && !kernel) fill_out_shapes(m, out);
    return true;
}

lagfx_status_t
lagfx_air2spv_translate_module(const lagfx_air_module_t *m,
                                uint8_t                 **out_blob,
                                size_t                   *out_size_bytes) {
    return lagfx_air2spv_translate_module_ex(m, NULL, out_blob, out_size_bytes);
}

static lagfx_undef_totals_t g_undef_totals;
void lagfx_air2spv_undef_totals(lagfx_undef_totals_t *out) { if (out) *out = g_undef_totals; }

lagfx_status_t
lagfx_air2spv_translate_module_ex(const lagfx_air_module_t *m,
                                const lagfx_stage_keys_t *vs_out,
                                uint8_t                 **out_blob,
                                size_t                   *out_size_bytes) {
    if (!m || !out_blob || !out_size_bytes) return LAGFX_ERR_INVALID_ARG;
    *out_blob = NULL;
    *out_size_bytes = 0u;

    lagfx_translate_stage_t stage = discover_stage(m);
    int rc = -1;
    lagfx_air2spv_msaa_scan(m, NULL);   /* counts AIR multisample reads / sample builtins (not translated: spelling unverified, see msaa_air_scan.h) */

    /* Per-function body translation is the default path for modules with a
     * non-prototype function; the legacy reference emitters remain the
     * fallback when it fails. */
    {
        uint32_t body_fn = pick_body_fn(m);
        if (body_fn != (uint32_t)-1 && stage != LAGFX_TRANS_STAGE_UNKNOWN) {
            lagfx_xlate_stage_t xs = (stage == LAGFX_TRANS_STAGE_VERTEX)   ? LAGFX_XLATE_STAGE_VERTEX
                                   : (stage == LAGFX_TRANS_STAGE_COMPUTE) ? LAGFX_XLATE_STAGE_COMPUTE
                                                                           : LAGFX_XLATE_STAGE_FRAGMENT;
            LAGFX_TRACE("air2spv: translating fn[%u]", body_fn);
            lagfx_stage_keys_t own;
            bool have_own = (xs == LAGFX_XLATE_STAGE_COMPUTE)
                              ? lagfx_air_stage_keys(m, false, &own)   /* a kernel's builtin arguments are identified by these keys */
                              : (xs == LAGFX_XLATE_STAGE_VERTEX)   ? lagfx_air_stage_keys(m, false, &own)   /* [[vertex_id]] vs [[instance_id]] are told apart by these keys */
                              : ((xs == LAGFX_XLATE_STAGE_FRAGMENT) && vs_out && lagfx_air_stage_keys(m, false, &own));
            lagfx_status_t bst = lagfx_air2spv_translate_function_ex(m, body_fn, xs,
                                                                    have_own ? &own : NULL,
                                                                    (have_own && xs != LAGFX_XLATE_STAGE_COMPUTE) ? vs_out : NULL,
                                                                    out_blob, out_size_bytes);
            if (bst == LAGFX_OK) {
                /* the translator does not always declare the 8/16-bit storage capabilities it relies on: add them so a device that has the feature accepts the module */
                if (!lagfx_spv_declare_used_caps(out_blob, out_size_bytes)) { free(*out_blob); *out_blob = NULL; *out_size_bytes = 0u; return LAGFX_ERR_OUT_OF_MEMORY; }
                lagfx_spv_undef_report_t ur; lagfx_spv_undef_analyze(*out_blob, *out_size_bytes, &ur);
                uint32_t ctl = ur.reach_branch + ur.reach_switch + ur.reach_select_cond;
                g_undef_totals.modules++; g_undef_totals.undef_used += ur.undef_used; g_undef_totals.reach_ctl += ctl; g_undef_totals.reach_store += ur.reach_store;
                if (ctl) g_undef_totals.modules_ctl++;
                if (ctl) {
                    LAGFX_WARN("air2spv: undef_used=%u of %u OpUndef; may-be-undefined values control %u branch / %u switch / %u select condition(s) and feed %u store(s): result is driver dependent",
                               ur.undef_used, ur.undef_defs, ur.reach_branch, ur.reach_switch, ur.reach_select_cond, ur.reach_store);
                    /* FAIL CLOSED: a module whose control flow depends on an unresolved operand is not built (no stub fallback either: a stub would silently draw the wrong thing). On the
                     * 39 shaders of the real dump the count is 0 (tools/check-dump-spirv.sh), so this rejects nothing valid there; LAGFX_ALLOW_UNDEF_CONTROL=1 restores the old behaviour.
                     * The analysis is a MAY-reach over-approximation: a reported module may still be correct, which is why the override exists. */
                    g_undef_totals.rejected++;
                    if (!getenv("LAGFX_ALLOW_UNDEF_CONTROL")) { free(*out_blob); *out_blob = NULL; *out_size_bytes = 0u; return LAGFX_ERR_PROTOCOL; }
                } else LAGFX_TRACE("air2spv: undef_used=%u of %u OpUndef, %u store(s) of possibly undefined values, no control flow affected", ur.undef_used, ur.undef_defs, ur.reach_store);
                return LAGFX_OK;
            }
            LAGFX_WARN("air2spv: translate_function failed (st=%d); falling back",
                       (int)bst);
            if (*out_blob) { free(*out_blob); *out_blob = NULL; *out_size_bytes = 0u; }
        }
    }

    switch (stage) {
        case LAGFX_TRANS_STAGE_VERTEX:
            LAGFX_TRACE("air2spv: detected vertex stage; emitting reference Position output");
            rc = lagfx_air2spv_emit_position_stub(out_blob, out_size_bytes);
            break;
        case LAGFX_TRANS_STAGE_FRAGMENT:
            LAGFX_TRACE("air2spv: detected fragment stage; emitting reference RenderTarget output");
            rc = lagfx_air2spv_emit_render_target_stub(out_blob, out_size_bytes);
            break;
        case LAGFX_TRANS_STAGE_COMPUTE:
            /* No reference stub exists for a kernel: a failed body translation is a failed translation (never a shader that silently does nothing). */
            LAGFX_LOG("air2spv: compute kernel translation failed");
            return LAGFX_ERR_PROTOCOL;
        default:
            LAGFX_LOG("air2spv: could not discover entry-point stage (no air.vertex / air.fragment / air.kernel NAMED_NODE)");
            return LAGFX_ERR_PROTOCOL;
    }

    if (rc != 0 || !*out_blob) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }
    return LAGFX_OK;
}
