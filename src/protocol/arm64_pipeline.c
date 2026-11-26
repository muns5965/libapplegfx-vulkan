/*
 * libapplegfx-vulkan — ARM64 pipeline-state resolver. See arm64_pipeline.h.
 */
#include "arm64_pipeline.h"
#include "arm64_objects.h"
#include "arm64_exec_decode.h"
#include "arm64_dump.h"
#include "../air2spv/spv_reflect.h"
#include "../air2spv/translate.h"
#include "../common/le.h"
#include "../common/log.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---- pure decode ---- */

uint64_t lagfx_a64_hash64(const void *data, size_t n) {
    const uint8_t *b = data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* Colour attachment array: the LAST chunk `0c 00 00 00 <len u32> <count u8>` whose first element is `00 04 <0 u32>` (attachment index 0) followed by
 * `01 04 <pixel format>` (earlier chunks hold vertex attributes, whose `01 04` value is a vertex format). `count` is the number of 6-byte tokens of attachment 0
 * (index + format + the non-default properties); the real dump (36 descriptors) always satisfies len == 6 * count + 13: the count byte, the tokens, and the
 * start of the NEXT attachment record (`03 00 04 <idx 1>`, `01 04 <fmt>`, optionally `09 04 <writeMask>`), which `len` ends inside of. A record is only
 * accepted when len/count/tokens agree, otherwise found stays false (partial decodes are not trusted). */
static bool is_pixfmt(uint32_t f) { return f == 10u || f == 70u || f == 80u || f == 81u || f == 90u || f == 94u || f == 115u || f == 125u || f == 55u || f == 71u || f == 8u || f == 20u || f == 30u || f == 40u || f == 60u || f == 65u || f == 110u || f == 105u || f == 100u; }
static void decode_color0(const uint8_t *b, uint32_t limit, lagfx_a64_blend_t *o) {
    memset(o, 0, sizeof *o);
    o->src_rgb = 1, o->dst_rgb = 0, o->op_rgb = 0, o->src_a = 1, o->dst_a = 0, o->op_a = 0, o->mask = 0xfu, o->mask1 = 0xfu;
    o->a1.src_rgb = 1, o->a1.src_a = 1;
    if (limit < 21u) return;
    for (uint32_t i = limit - 21u;; i--) {   /* last chunk first; every read below is < i + 21 <= limit */
        if (b[i] == 0x0cu && !b[i + 1] && !b[i + 2] && !b[i + 3] && b[i + 9] == 0x00u && b[i + 10] == 0x04u && !lagfx_le32(b + i + 11) &&
            b[i + 15] == 0x01u && b[i + 16] == 0x04u && is_pixfmt(lagfx_le32(b + i + 17))) {
            uint32_t len = lagfx_le32(b + i + 4), count = b[i + 8];
            if (count >= 2u && len == 6u * count + 13u && (uint64_t)i + 8u + len <= limit) {
                lagfx_a64_blend_t t = *o;
                t.pixfmt = lagfx_le32(b + i + 17);
                uint32_t at = i + 21u, prev = 1u, ntok = 2u;
                bool ok = true;
                while (ntok < count) {
                    if (at + 6u > limit || b[at + 1] != 0x04u || b[at] <= prev || b[at] > 9u || lagfx_le32(b + at + 2u) > 0xffu) { ok = false; break; }
                    uint32_t key = b[at], v = lagfx_le32(b + at + 2u);
                    switch (key) {
                    case 2: t.enabled_token = true; t.enabled_value = (uint8_t)v; break;
                    case 3: t.src_rgb = (uint8_t)v; t.any_blend_token = true; break;
                    case 4: t.dst_rgb = (uint8_t)v; t.any_blend_token = true; break;
                    case 5: t.op_rgb = (uint8_t)v; t.any_blend_token = true; break;
                    case 6: t.src_a = (uint8_t)v; t.any_blend_token = true; break;
                    case 7: t.dst_a = (uint8_t)v; t.any_blend_token = true; break;
                    case 8: t.op_a = (uint8_t)v; t.any_blend_token = true; break;
                    case 9: t.mask = (uint8_t)v; break;
                    default: break;
                    }
                    prev = key; at += 6u; ntok++;
                }
                if (ok) {
                    if (t.enabled_token) t.any_blend_token = true;
                    t.found = true;
                    /* attachment 1: `03 00 04 01 00 00 00 01 04 <fmt>` [`09 04 <writeMask>`] right after attachment 0 */
                    if (at + 13u <= limit && b[at] == 0x03u && b[at + 1] == 0x00u && b[at + 2] == 0x04u && lagfx_le32(b + at + 3) == 1u && b[at + 7] == 0x01u && b[at + 8] == 0x04u && is_pixfmt(lagfx_le32(b + at + 9))) {
                        t.found1 = true, t.pixfmt1 = lagfx_le32(b + at + 9);
                        /* tokens of attachment 1 after its pixel format (same grammar as attachment 0: increasing keys 2..9, value <= 0xff); stops at the first byte that is not one */
                        uint32_t q = at + 13u, pk = 1u;
                        while (q + 6u <= limit && b[q + 1] == 0x04u && b[q] > pk && b[q] <= 9u && lagfx_le32(b + q + 2u) <= 0xffu) {
                            uint32_t key = b[q], v = lagfx_le32(b + q + 2u);
                            lagfx_a64_att_blend_t *a = &t.a1;
                            switch (key) {
                            case 2: a->enabled_token = true; a->enabled_value = (uint8_t)v; break;
                            case 3: a->src_rgb = (uint8_t)v; a->any_blend_token = true; break;
                            case 4: a->dst_rgb = (uint8_t)v; a->any_blend_token = true; break;
                            case 5: a->op_rgb = (uint8_t)v; a->any_blend_token = true; break;
                            case 6: a->src_a = (uint8_t)v; a->any_blend_token = true; break;
                            case 7: a->dst_a = (uint8_t)v; a->any_blend_token = true; break;
                            case 8: a->op_a = (uint8_t)v; a->any_blend_token = true; break;
                            case 9: if (v <= 0xfu) t.mask1 = (uint8_t)v; break;
                            default: break;
                            }
                            pk = key; q += 6u;
                        }
                    }
                    *o = t;
                    return;
                }
            }
        }
        if (!i) break;
    }
}

int lagfx_a64_fbfetch_check(int stage, bool have_local_read, uint32_t spv_binding, uint32_t n_bind) {
    if (stage == 0) return LAGFX_A64_FBF_VERTEX_STAGE;
    if (!have_local_read) return LAGFX_A64_FBF_NO_LOCAL_READ;
    /* fragment modules are offset by LAGFX_A64_FRAG_BINDING_BASE before reflection, so attachment n sits at BASE + 24 + n (the first bug: the unshifted 24..27 never matched) */
    uint32_t lo = LAGFX_A64_FRAG_BINDING_BASE + LAGFX_A64_FBF_BINDING_BASE;
    if (spv_binding < lo || spv_binding > lo + 3u) return LAGFX_A64_FBF_BINDING_RANGE;
    if (n_bind >= LAGFX_A64_MAX_BINDINGS) return LAGFX_A64_FBF_TOO_MANY;   /* = LAGFX_A64_MAX_BINDINGS (kept literal: that macro lives in the exec header) */
    return LAGFX_A64_FBF_OK;
}
const char *lagfx_a64_fbfetch_reason_name(int reason) {
    static const char *const n[LAGFX_A64_FBF_REASONS] = { "ok", "vertex_stage", "no_local_read", "binding_range", "too_many_bindings" };
    return reason >= 0 && reason < LAGFX_A64_FBF_REASONS ? n[reason] : "?";
}

bool lagfx_a64_pdesc_decode(const uint8_t *blob, uint32_t n, bool (*is_fn)(void *ctx, uint32_t ref), void *ctx, lagfx_a64_pdesc_t *out) {
    if (!blob || !out || n < 16u) return false;
    memset(out, 0, sizeof *out);
    out->tag = lagfx_le32(blob), out->size = lagfx_le32(blob + 4), out->ref = lagfx_le32(blob + 8), out->length = lagfx_le32(blob + 12);
    uint32_t limit = out->length && 16ull + out->length <= n ? 16u + out->length : n;   /* the body the header declares, else what we have */

    /* function refs: `04 <u32>` tokens (x86 object_resolver.c scans `04 XX`; ARM64 object numbers are not limited to a byte) naming a function object */
    for (uint32_t i = 16u; i + 5u <= limit && out->nrefs < LAGFX_A64_PDESC_MAX_REFS; i++) {
        if (blob[i] != 0x04u) continue;
        uint32_t v = lagfx_le32(blob + i + 1u);
        if (!v || (is_fn && !is_fn(ctx, v))) continue;
        bool seen = false;
        for (uint32_t k = 0; k < out->nrefs; k++) if (out->refs[k] == v) { seen = true; break; }
        if (!seen) out->refs[out->nrefs++] = v;
    }
    /* vertex attributes: `01 04 <fmt> 02 04 <off> 03 04 <bufidx>` (object_resolver.c lagfx_parse_pso_vertex_attrs, same plausibility limits) */
    for (uint32_t i = 16u; i + 18u <= limit; i++) {
        if (blob[i] != 0x01u || blob[i + 1u] != 0x04u || blob[i + 6u] != 0x02u || blob[i + 7u] != 0x04u || blob[i + 12u] != 0x03u || blob[i + 13u] != 0x04u) continue;
        uint32_t fmt = lagfx_le32(blob + i + 2u), off = lagfx_le32(blob + i + 8u), bid = lagfx_le32(blob + i + 14u);
        if (fmt == 0u || fmt > 64u || off > 240u || bid > 30u) continue;
        uint32_t k = 0;
        for (; k < out->nattrs; k++) if (out->attrs[k].off == off && out->attrs[k].bufidx == bid) break;
        if (k < out->nattrs) continue;
        if (out->nattrs < LAGFX_A64_PDESC_MAX_ATTRS) out->attrs[out->nattrs++] = (lagfx_a64_vattr_t){ fmt, off, bid };
        i += 17u;
    }
    for (uint32_t a = 1; a < out->nattrs; a++) {
        lagfx_a64_vattr_t t = out->attrs[a];
        uint32_t j = a;
        while (j > 0u && out->attrs[j - 1u].off > t.off) { out->attrs[j] = out->attrs[j - 1u]; j--; }
        out->attrs[j] = t;
    }
    /* stride: the most repeated plausible `04 <v> 02|01` value (object_resolver.c lagfx_parse_pso_vertex_stride) */
    uint32_t cand[64], hits[64], nc = 0;
    for (uint32_t i = 16u; i + 6u <= limit;) {
        if (blob[i] != 0x04u) { i++; continue; }
        uint32_t val = lagfx_le32(blob + i + 1u);
        uint8_t tag = blob[i + 5u];
        if ((tag == 0x02u || tag == 0x01u) && val >= 8u && val <= 256u && (val & 3u) == 0u) {
            uint32_t k = 0;
            for (; k < nc; k++) if (cand[k] == val) { hits[k]++; break; }
            if (k == nc && nc < 64u) cand[nc] = val, hits[nc] = 1u, nc++;
        }
        i += 6u;
    }
    uint32_t best = 0, bestn = 0;
    for (uint32_t k = 0; k < nc; k++) if (hits[k] > bestn || (hits[k] == bestn && cand[k] > best)) best = cand[k], bestn = hits[k];
    out->stride = best;
    decode_color0(blob, limit, &out->color0);
    return true;
}

/* ---- guest reads ---- */

uint8_t *lagfx_a64_read_payload(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint32_t entry[3], uint32_t *len, uint32_t *full_size) {
    uint32_t size = entry[0] >> 8;
    if (full_size) *full_size = size;
    if (!size) return NULL;
    uint32_t n = size > LAGFX_A64_PIPE_MAX_BLOB ? LAGFX_A64_PIPE_MAX_BLOB : size;
    uint8_t *b = malloc(n);
    if (!b) return NULL;
    if (!lagfx_arm64_task_read(p, task, (uint64_t)entry[2] << 32 | entry[1], b, n)) { free(b); return NULL; }
    if (len) *len = n;
    return b;
}

bool lagfx_a64_payload_hash(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint32_t entry[3], uint64_t *hash) {
    uint32_t n = 0;
    uint8_t *b = lagfx_a64_read_payload(p, task, entry, &n, NULL);
    if (!b) return false;
    *hash = lagfx_a64_hash64(b, n);
    free(b);
    return true;
}

bool lagfx_a64_is_function_ref(void *ctx, uint32_t ref) {
    lagfx_a64_fnctx_t *c = ctx;
    uint32_t e[3];
    return lagfx_a64_objects_read_entry(c->p, c->task, ref, e) && (e[0] & 0xffu) == LAGFX_A64_OBJ_FUNCTION;
}

uint8_t *lagfx_a64_read_function_lib(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, uint32_t *lib_len, uint64_t *lib_va) {
    uint32_t e[3];
    uint8_t h[20];
    if (!lagfx_a64_objects_read_entry(p, task, ref, e) || (e[0] & 0xffu) != LAGFX_A64_OBJ_FUNCTION) return NULL;
    lagfx_a64_dump_touch_object(p, task, ref, e);   /* a function object read through a pipeline (tag 116) that no resource list named */
    if (!lagfx_arm64_task_read(p, task, (uint64_t)e[2] << 32 | e[1], h, sizeof h)) return NULL;   /* {u64 off, u32 len, u32 tag, u32 size} (makeFunction) */
    uint64_t va = lagfx_le64(h);
    uint32_t len = lagfx_le32(h + 8);
    if (!len || len > LAGFX_A64_LIB_MAX) return NULL;
    uint8_t *b = malloc(len);
    if (!b) return NULL;
    if (!lagfx_arm64_task_read(p, task, va, b, len)) { free(b); return NULL; }
    if (lib_len) *lib_len = len;
    if (lib_va) *lib_va = va;
    return b;
}


/* ---- descriptor binding map (pure; shared by the resolver and examples/dump-draws) ---- */

/* Per stage: the reflected set-0 bindings in binding order, each matched to the n-th Metal argument of its KIND (buffer / texture / sampler) from Apple's arg
 * metadata. By kind, not by position: the translator emits an extra sampler binding for an inline `constexpr sampler` that has no argument; such a sampler
 * gets metal_index -1 = the default sampler. Appends to bind[*n_bind]; on a failure the entries already appended stay (the pipeline is dropped anyway). */
int lagfx_a64_map_stage_bindings(const uint8_t *spv, size_t len, int st, const lagfx_air_arg_binding_t *ab, size_t nab, bool have_local_read, bool have_storage_img_write,
                                 lagfx_a64_bind_t *bind, uint32_t *n_bind, int *fbf_why) {
    lagfx_spv_binding_t tmp[LAGFX_A64_MAX_REFLECT];
    size_t nt = lagfx_spv_reflect_bindings(spv, len, tmp, LAGFX_A64_MAX_REFLECT);
    if (nt > LAGFX_A64_MAX_REFLECT) return LAGFX_A64_MAP_FAIL;   /* reflect returns the TOTAL: more than the buffer holds must fail, not be truncated */
    for (size_t a = 1; a < nt; a++) { lagfx_spv_binding_t x = tmp[a]; size_t b = a; while (b && tmp[b - 1].binding > x.binding) tmp[b] = tmp[b - 1], b--; tmp[b] = x; }
    uint32_t nth[6] = { 0, 0, 0, 0, 0, 0 };   /* by lagfx_spv_binding_kind_t */
    for (size_t t = 0; t < nt; t++) {
        if (tmp[t].set != 0u) continue;
        if (t && tmp[t - 1].set == 0u && tmp[t - 1].binding == tmp[t].binding) continue;   /* byte-buffer views of one buffer alias its binding (translator): ONE descriptor and ONE Metal argument, not one per view */
        if (tmp[t].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT) {   /* framebuffer fetch: binding 24 + n reads colour attachment n of the current pass */
            int why = lagfx_a64_fbfetch_check(st, have_local_read, tmp[t].binding, *n_bind);
            if (why != LAGFX_A64_FBF_OK) { if (fbf_why) *fbf_why = why; return LAGFX_A64_MAP_FBFETCH; }
            bind[*n_bind].binding = (uint8_t)tmp[t].binding, bind[*n_bind].kind = (uint8_t)LAGFX_SPV_BINDING_INPUT_ATTACHMENT, bind[*n_bind].writable = 0;
            bind[*n_bind].stage = 1, bind[*n_bind].metal_index = (int16_t)(tmp[t].binding - LAGFX_A64_FRAG_BINDING_BASE - LAGFX_A64_FBF_BINDING_BASE);
            (*n_bind)++;
            continue;
        }
        if (tmp[t].kind != LAGFX_SPV_BINDING_STORAGE_BUFFER && tmp[t].kind != LAGFX_SPV_BINDING_SAMPLED_IMAGE && tmp[t].kind != LAGFX_SPV_BINDING_SAMPLER &&
            tmp[t].kind != LAGFX_SPV_BINDING_STORAGE_IMAGE) return LAGFX_A64_MAP_FAIL;
        if (tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE && (st == 0 || !have_storage_img_write)) return st == 0 ? LAGFX_A64_MAP_VERTEX_RES : LAGFX_A64_MAP_FAIL;   /* fragment writes only; needs the device features */
        if (*n_bind >= LAGFX_A64_MAX_BINDINGS || tmp[t].binding > 255u) return LAGFX_A64_MAP_FAIL;
        uint8_t want = tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER ? 1 : (tmp[t].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE || tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE) ? 2 : 3;
        uint8_t want_w = tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE;   /* textures split by access: written ones are storage images */
        uint32_t n = nth[tmp[t].kind]++, seen = 0;
        int midx = -1; uint8_t wr = 0;
        for (size_t k = 0; k < nab; k++) if (ab[k].kind == want && (want != 2 || ab[k].written == want_w) && seen++ == n) { midx = ab[k].metal_index; wr = ab[k].written; break; }
        if (midx > 31 || (midx < 0 && tmp[t].kind != LAGFX_SPV_BINDING_SAMPLER)) return LAGFX_A64_MAP_FAIL;   /* a buffer/texture nobody can feed */
        bind[*n_bind].binding = (uint8_t)tmp[t].binding;
        bind[*n_bind].kind = (uint8_t)tmp[t].kind;
        bind[*n_bind].stage = (uint8_t)st;
        bind[*n_bind].metal_index = (int16_t)midx;
        bind[*n_bind].writable = tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER ? wr : 0u;
        (*n_bind)++;
    }
    return LAGFX_A64_MAP_OK;
}

/* ---- the resolver (Vulkan) ---- */

#ifndef LAGFX_HAVE_VULKAN
bool lagfx_a64_pipeline_resolve(void *ctx, lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t entry[3], lagfx_a64_pipeline_src_t *out) {
    (void)ctx; (void)p; (void)task; (void)ref; (void)entry; (void)out;
    return false;
}
#else

#include "../device.h"
#include "arm64_tile.h"
#include "../vulkan/instance.h"
#include "../vulkan/descriptor_layout.h"
#include "../vulkan/pipeline_bundle.h"
#include "../air/bitcode_reader.h"
#include "../air2spv/translate.h"
#include "../air2spv/spv_reflect.h"
#include "../air2spirv/metallib_extract.h"
#include <vulkan/vulkan.h>

#define CNTP(p) ((p)->diag2.exec.pipe)

typedef struct {
    VkDevice dev;
    VkShaderModule vs, fs, fs_alt;   /* fs_alt: fs with explicit LOD 0 sampling, for pipelines that are drawn with an unnormalized-coordinates sampler */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    char vname[64], fname[64];
    uint8_t *vspv, *fspv; size_t vsz, fsz;   /* the SPIR-V the modules were made from (diagnostics of a failed vkCreateGraphicsPipelines; owned) */
    lagfx_pipeline_diag_t diag;              /* failure bundle inputs (only filled when the bundle option is on: pipeline_bundle.h) */
    uint8_t *vbc, *fbc;                      /* copies of the AIR bitcode, same condition (owned) */
    char vfn[96], ffn[96], origin[96];
} pres_t;

static void pres_release(void *u) {
    pres_t *r = u;
    if (!r) return;
    if (r->pl) vkDestroyPipelineLayout(r->dev, r->pl, NULL);
    if (r->dsl) vkDestroyDescriptorSetLayout(r->dev, r->dsl, NULL);
    if (r->vs) vkDestroyShaderModule(r->dev, r->vs, NULL);
    if (r->fs) vkDestroyShaderModule(r->dev, r->fs, NULL);
    if (r->fs_alt) vkDestroyShaderModule(r->dev, r->fs_alt, NULL);
    free(r->vspv); free(r->fspv); free(r->vbc); free(r->fbc);
    free(r);
}

/* The first OpEntryPoint's name (so the module is used under the name the translator gave it). */
static bool spv_entry_name(const uint8_t *spv, size_t n, char *out, size_t cap) {
    if (n < 20u || (n & 3u)) return false;
    for (size_t w = 5; w * 4u + 4u <= n;) {
        uint32_t ins = lagfx_le32(spv + w * 4u), op = ins & 0xffffu, wc = ins >> 16;
        if (!wc || (w + wc) * 4u > n) return false;
        if (op == 15u && wc >= 4u) {   /* OpEntryPoint model id name... */
            size_t room = (size_t)(wc - 3u) * 4u;
            const char *s = (const char *)spv + (w + 3u) * 4u;
            size_t l = strnlen(s, room);
            if (l >= cap || l == room) return false;
            memcpy(out, s, l + 1u);
            return true;
        }
        w += wc;
    }
    return false;
}

/* True if the SPIR-V decorates a variable BuiltIn PointSize. */
static bool spv_writes_point_size(const uint8_t *spv, size_t n) {
    if (n < 20u || (n & 3u)) return false;
    for (size_t w = 5; w * 4u + 4u <= n;) {
        uint32_t ins = lagfx_le32(spv + w * 4u), op = ins & 0xffffu, wc = ins >> 16;
        if (!wc || (w + wc) * 4u > n) return false;
        if (op == 71u && wc >= 4u && lagfx_le32(spv + (w + 2u) * 4u) == 11u && lagfx_le32(spv + (w + 3u) * 4u) == 1u) return true;   /* OpDecorate id BuiltIn PointSize */
        w += wc;
    }
    return false;
}

typedef struct {
    uint8_t *lib; uint32_t len;
    lagfx_metallib_function_t fn[8]; size_t nfn;
} lib_t;

/* Translates one function (AIR bitcode) with the in-process air2spv translator; also returns its Metal resource map. */
static bool translate(lagfx_protocol_t *p, const lagfx_metallib_function_t *f, const lagfx_stage_keys_t *vs_keys, lagfx_stage_keys_t *keys_out, uint8_t **spv, size_t *sz, lagfx_air_arg_binding_t *ab, size_t *nab) {
    lagfx_air_module_t *m = NULL;
    *nab = 0;
    if (lagfx_air_module_open(f->bitcode, f->bitcode_len, &m) != LAGFX_OK || !m) { CNTP(p).rf_module++; return false; }
    if (keys_out && !lagfx_air_stage_keys(m, true, keys_out)) memset(keys_out, 0, sizeof *keys_out);   /* vertex: its output keys, for the paired fragment */
    lagfx_stage_link_totals_t l0, l1;
    lagfx_air2spv_link_totals(&l0);
    lagfx_status_t st = lagfx_air2spv_translate_module_ex(m, vs_keys, spv, sz);   /* fragment: stage-in Locations follow the vertex keys; a typed vs_keys also links the interface types */
    lagfx_air2spv_link_totals(&l1);   /* process-wide counters: the delta is this translation's (resolution runs on one thread) */
    CNTP(p).link_mismatch_fixed += l1.mismatch_fixed - l0.mismatch_fixed; CNTP(p).link_missing_zeroed += l1.missing_zeroed - l0.missing_zeroed; CNTP(p).link_unlinked += l1.unlinked - l0.unlinked;
    if (st == LAGFX_OK && *spv) {
        *nab = lagfx_air_arg_bindings(m, ab, LAGFX_A64_MAX_ARGS);
        if (*nab > LAGFX_A64_MAX_ARGS) *nab = LAGFX_A64_MAX_ARGS;
    }
    lagfx_air_module_free(m);
    if (st != LAGFX_OK || !*spv || !*sz) { free(*spv); *spv = NULL; CNTP(p).rf_translate++; return false; }
    CNTP(p).xlate_air2spv++;
    return true;
}

bool lagfx_a64_pipeline_resolve(void *ctx, lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t entry[3], lagfx_a64_pipeline_src_t *out) {
    (void)ctx; (void)ref;
    bool ok = false;
    uint8_t *blob = NULL, *vspv = NULL, *fspv = NULL;
    size_t vsz = 0, fsz = 0, nvab = 0, nfab = 0;
    lagfx_air_arg_binding_t vab[LAGFX_A64_MAX_ARGS], fab[LAGFX_A64_MAX_ARGS];
    lib_t libs[LAGFX_A64_PDESC_MAX_REFS];
    uint32_t nlib = 0;
    pres_t *r = NULL;
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    struct lagfx_vk_state *vk = dev ? dev->vk : NULL;
    CNTP(p).resolve_failed++;   /* turned back into resolved on success */
    if (!vk || !vk->initialized || vk->device == VK_NULL_HANDLE) { CNTP(p).rf_no_vulkan++; return false; }

    uint32_t blen = 0, full = 0;
    blob = lagfx_a64_read_payload(p, task, entry, &blen, &full);
    lagfx_a64_pdesc_t d;
    lagfx_a64_fnctx_t fc = { p, task };
    if (!blob || blen < 16u || full > LAGFX_A64_PIPE_MAX_BLOB) { CNTP(p).rf_no_blob++; goto done; }
    if (!lagfx_a64_pdesc_decode(blob, blen, lagfx_a64_is_function_ref, &fc, &d)) { CNTP(p).rf_no_blob++; goto done; }
    if (d.tag == 0x33u) { ok = lagfx_a64_tile_resolve(p, task, ref, blob, blen, &d, out); goto done; }   /* tile pipeline: a compute kernel (arm64_tile.c) */
    if (d.tag != 0xeu) { CNTP(p).rf_bad_tag++; goto done; }
    if (!d.nrefs) { CNTP(p).rf_no_refs++; goto done; }

    const lagfx_metallib_function_t *vf = NULL, *ff = NULL;
    for (uint32_t i = 0; i < d.nrefs && (!vf || !ff); i++) {
        lib_t *l = &libs[nlib];
        memset(l, 0, sizeof *l);
        l->lib = lagfx_a64_read_function_lib(p, task, d.refs[i], &l->len, NULL);
        if (!l->lib) continue;
        nlib++;
        if (lagfx_metallib_extract_functions(l->lib, l->len, l->fn, 8, &l->nfn) != LAGFX_OK) continue;
        for (size_t k = 0; k < l->nfn && k < 8; k++) {
            if (!l->fn[k].bitcode || !l->fn[k].bitcode_len) continue;
            if (!vf && l->fn[k].stage == LAGFX_METALLIB_STAGE_VERTEX) vf = &l->fn[k];
            if (!ff && l->fn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT) ff = &l->fn[k];
        }
    }
    if (!nlib) { CNTP(p).rf_no_lib++; goto done; }
    if (!vf || !ff) { CNTP(p).rf_no_stage++; goto done; }
    lagfx_stage_keys_t vkeys;
    memset(&vkeys, 0, sizeof vkeys);
    if (!translate(p, vf, NULL, &vkeys, &vspv, &vsz, vab, &nvab)) goto done;
    {   /* LAGFX_A64_NO_STAGE_LINK=1: untyped keys -> the fragment translator keeps its pre-linker behaviour (Locations by key, no conversion, no zeroed inputs) */
        const char *nl = getenv("LAGFX_A64_NO_STAGE_LINK");
        if (nl && nl[0] && nl[0] != '0') vkeys.typed = 0;
    }
    if (!translate(p, ff, &vkeys, NULL, &fspv, &fsz, fab, &nfab)) goto done;
    {   /* device-capability gate: a module that declares a capability the device was not created with must not reach vkCreateShaderModule / a pipeline */
        uint32_t nv, nf;
        bool okv = lagfx_spv_required_caps_ex(vspv, vsz, &nv), okf = lagfx_spv_required_caps_ex(fspv, fsz, &nf);
        if (!okv || !okf) { CNTP(p).rf_missing_cap++; LAGFX_WARN("arm64 pipeline %u: shader module could not be analysed for capabilities; pipeline not built", ref); goto done; }
        uint32_t need = nv | nf;
        uint32_t miss = need & ~vk->caps_enabled;
        if (miss) {
            for (unsigned c = 0; c < LAGFX_VK_CAP_COUNT; c++) if (miss & LAGFX_VK_CAP_BIT(c)) CNTP(p).stub_missing_cap[c]++;
            CNTP(p).rf_missing_cap++;
            LAGFX_WARN("arm64 pipeline %u: shader needs device capabilities that are not enabled (mask 0x%x); pipeline not built, its draws are skipped", ref, miss);
            goto done;
        }
    }

    /* vertex layout: one Vulkan stream (the builder's limit), fed by the Metal buffer slot the attributes name */
    memset(out, 0, sizeof *out);
    snprintf(out->vs_name, sizeof out->vs_name, "%s", vf->name), snprintf(out->fs_name, sizeof out->fs_name, "%s", ff->name);   /* diagnostics: names the shader pair in logs */
    out->vs_hash = lagfx_a64_hash64(vf->bitcode, vf->bitcode_len), out->fs_hash = lagfx_a64_hash64(ff->bitcode, ff->bitcode_len);
    lagfx_spv_vertex_input_t vin[8];
    size_t nvi = lagfx_spv_reflect_vertex_inputs(vspv, vsz, vin, 8);
    if (nvi > 8u) nvi = 8u;
    for (size_t v = 0; v < nvi; v++) out->desc.vtx_in_loc[v] = (uint8_t)vin[v].location, out->desc.vtx_in_comp[v] = (uint8_t)vin[v].components;
    out->desc.n_vtx_inputs = (uint8_t)nvi;
    if (nvi && d.nattrs) {
        for (uint32_t a = 1; a < d.nattrs; a++) if (d.attrs[a].bufidx != d.attrs[0].bufidx) { CNTP(p).rf_multi_stream++; goto done; }
        out->vertex_buffer_index = d.attrs[0].bufidx;
        out->desc.n_pso_attrs = (uint8_t)d.nattrs;
        for (uint32_t a = 0; a < d.nattrs; a++) out->desc.pso_attr_fmt[a] = d.attrs[a].fmt, out->desc.pso_attr_off[a] = d.attrs[a].off;
        out->desc.vtx_stride = d.stride;
    }

    /* descriptor bindings: fragment bindings move to a disjoint range (as the x86 draw path does), then the merged set-0 layout */
    lagfx_spv_offset_bindings(fspv, fsz, LAGFX_A64_FRAG_BINDING_BASE);
    const uint8_t *blobs[2] = { vspv, fspv };
    const size_t lens[2] = { vsz, fsz };
    r = calloc(1, sizeof *r);
    if (!r) goto done;
    r->dev = vk->device;
    if (lagfx_build_pipeline_layout_from_spv(vk->device, blobs, lens, 2, &r->dsl, &r->pl) != LAGFX_OK) { CNTP(p).rf_layout++; goto done; }
    /* Per stage: the reflected bindings in binding order, each matched to the n-th Metal argument of its KIND (buffer / texture / sampler) from Apple's arg
     * metadata. By kind, not by position: the translator emits an extra sampler binding for an inline `constexpr sampler` that has no argument; such a sampler
     * gets metal_index -1 = the default sampler. */
    for (int st = 0; st < 2; st++) {
        int why = 0;
        int mr = lagfx_a64_map_stage_bindings(blobs[st], lens[st], st, st ? fab : vab, st ? nfab : nvab, vk->have_local_read, vk->have_storage_img_write, out->bind, &out->n_bind, &why);
        if (mr == LAGFX_A64_MAP_FBFETCH) {
            CNTP(p).stub_fbfetch++;
            if (CNTP(p).stub_fbfetch_why[why]++ == 0)
                LAGFX_LOG("arm64 pipeline %u: framebuffer-fetch pipeline not built (%s): stage=%d n_bind=%u local_read=%d", ref, lagfx_a64_fbfetch_reason_name(why), st, out->n_bind, (int)vk->have_local_read);
            goto done;
        }
        if (mr == LAGFX_A64_MAP_VERTEX_RES) { CNTP(p).rf_vertex_res++; goto done; }
        if (mr != LAGFX_A64_MAP_OK) { CNTP(p).rf_bindings++; goto done; }
        for (uint32_t k = 0; k < out->n_bind; k++) if (out->bind[k].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT && out->bind[k].stage == 1u) CNTP(p).fbfetch_pipes++;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = vsz, ci.pCode = (const uint32_t *)vspv;
    if (vkCreateShaderModule(vk->device, &ci, NULL, &r->vs) != VK_SUCCESS) { CNTP(p).rf_module++; goto done; }
    ci.codeSize = fsz, ci.pCode = (const uint32_t *)fspv;
    if (vkCreateShaderModule(vk->device, &ci, NULL, &r->fs) != VK_SUCCESS) { CNTP(p).rf_module++; goto done; }
    {   /* a fragment shader that samples with implicit LOD cannot be used with an unnormalized sampler: keep a variant with explicit LOD 0 (the exec picks it per draw) */
        bool fsamp = false;
        for (uint32_t k = 0; k < out->n_bind; k++) fsamp |= out->bind[k].stage == 1 && out->bind[k].kind == LAGFX_A64_BIND_SAMPLER;
        uint8_t *alt = NULL; size_t altsz = 0;
        if (fsamp && lagfx_spv_explicit_lod0(fspv, fsz, &alt, &altsz) == 1) {
            ci.codeSize = altsz, ci.pCode = (const uint32_t *)alt;
            if (vkCreateShaderModule(vk->device, &ci, NULL, &r->fs_alt) != VK_SUCCESS) r->fs_alt = VK_NULL_HANDLE;
            free(alt);
        }
    }
    if (!spv_entry_name(vspv, vsz, r->vname, sizeof r->vname)) strcpy(r->vname, "main");
    if (!spv_entry_name(fspv, fsz, r->fname, sizeof r->fname)) strcpy(r->fname, "main");

    out->desc.vertex_shader = r->vs, out->desc.fragment_shader = r->fs, out->alt_fragment_shader = r->fs_alt;
    out->desc.vertex_entry_point = r->vname, out->desc.fragment_entry_point = r->fname;
    out->desc.layout = r->pl ? r->pl : vk->empty_layout;
    out->dsl = r->dsl;
    out->no_point_size = !spv_writes_point_size(vspv, vsz);
    out->desc.vertex_spv = (const uint32_t *)vspv, out->desc.vertex_spv_bytes = vsz;   /* ownership moves into r (freed by pres_release) */
    out->desc.fragment_spv = (const uint32_t *)fspv, out->desc.fragment_spv_bytes = fsz;
    out->desc.caps_enabled = vk->caps_enabled;
    {   /* blend state of colour attachment 0 (see lagfx_a64_blend_t: only what the descriptor actually carries) */
        static const uint8_t fac[15] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
            VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA };
        static const uint8_t ops[5] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX };
        const lagfx_a64_blend_t *b = &d.color0;
        /* BlendColor factors (11..14) need vkCmdSetBlendConstants, which the exec path does not issue yet: such a pipeline keeps the fixed state (counted as a stub) */
        bool ok_vals = b->found && b->src_rgb < 11u && b->dst_rgb < 11u && b->src_a < 11u && b->dst_a < 11u && b->op_rgb < 5u && b->op_a < 5u && (b->mask & ~0xfu) == 0u;
        if (ok_vals) {
            out->desc.blend_set = 1;
            out->desc.blend_enable = lagfx_a64_blend_on(b);   /* key 2 = blendingEnabled; absent = Metal default = off (factors can be serialized without it: 13 of the 36 dump descriptors) */
            out->desc.blend_src_rgb = fac[b->src_rgb], out->desc.blend_dst_rgb = fac[b->dst_rgb], out->desc.blend_op_rgb = ops[b->op_rgb];
            out->desc.blend_src_a = fac[b->src_a], out->desc.blend_dst_a = fac[b->dst_a], out->desc.blend_op_a = ops[b->op_a];
            out->desc.color_write_mask = ((b->mask & 8u) ? VK_COLOR_COMPONENT_R_BIT : 0u) | ((b->mask & 4u) ? VK_COLOR_COMPONENT_G_BIT : 0u) | ((b->mask & 2u) ? VK_COLOR_COMPONENT_B_BIT : 0u) | ((b->mask & 1u) ? VK_COLOR_COMPONENT_A_BIT : 0u);
            for (uint32_t i = 0; i < 4u; i++) out->desc.color_write_masks[i] = 0xfu;
            out->desc.color_write_masks[0] = out->desc.color_write_mask;
            out->desc.masks_valid = 1;
            if (b->found1) {
                const lagfx_a64_att_blend_t *a = &b->a1;
                if (a->src_rgb < 11u && a->dst_rgb < 11u && a->src_a < 11u && a->dst_a < 11u && a->op_rgb < 5u && a->op_a < 5u) {
                    out->desc.slot_blend[1].set = 1, out->desc.slot_blend[1].enable = a->enabled_token && a->enabled_value != 0u;
                    out->desc.slot_blend[1].src_rgb = fac[a->src_rgb], out->desc.slot_blend[1].dst_rgb = fac[a->dst_rgb], out->desc.slot_blend[1].op_rgb = ops[a->op_rgb];
                    out->desc.slot_blend[1].src_a = fac[a->src_a], out->desc.slot_blend[1].dst_a = fac[a->dst_a], out->desc.slot_blend[1].op_a = ops[a->op_a];
                }   /* else (BlendColor factors): slot 1 copies attachment 0 and the pipeline counts it as a mismatch */   /* attachment 1's write mask (the dump's second attachment record carries writeMask 0) */
                uint8_t m1 = b->mask1;
                out->desc.color_write_masks[1] = ((m1 & 8u) ? VK_COLOR_COMPONENT_R_BIT : 0u) | ((m1 & 4u) ? VK_COLOR_COMPONENT_G_BIT : 0u) | ((m1 & 2u) ? VK_COLOR_COMPONENT_B_BIT : 0u) | ((m1 & 1u) ? VK_COLOR_COMPONENT_A_BIT : 0u);
            }
            CNTP(p).blend_decoded++;
            if (!out->desc.blend_enable) CNTP(p).blend_off++;
            if (b->found1) CNTP(p).color1_seen++;
        } else CNTP(p).stub_blend++;
    }
    if (lagfx_pipeline_bundle_dir()) {   /* option on: keep what a failure bundle needs (the AIR bitcode is freed with the libraries below) */
        r->vbc = malloc(vf->bitcode_len), r->fbc = malloc(ff->bitcode_len);
        if (r->vbc) memcpy(r->vbc, vf->bitcode, vf->bitcode_len);
        if (r->fbc) memcpy(r->fbc, ff->bitcode, ff->bitcode_len);
        snprintf(r->vfn, sizeof r->vfn, "%.95s", vf->name), snprintf(r->ffn, sizeof r->ffn, "%.95s", ff->name);
        snprintf(r->origin, sizeof r->origin, "arm64 pso task=%u ref=%u", task->id, ref);
        r->diag = (lagfx_pipeline_diag_t){ .vk = vk, .vs_bc = r->vbc, .fs_bc = r->fbc, .vs_bc_bytes = r->vbc ? vf->bitcode_len : 0, .fs_bc_bytes = r->fbc ? ff->bitcode_len : 0, .vs_fn = r->vfn, .fs_fn = r->ffn, .origin = r->origin };
        out->desc.diag = &r->diag;
    }
    r->vspv = vspv, r->vsz = vsz, r->fspv = fspv, r->fsz = fsz;
    vspv = fspv = NULL;
    out->blob_hash = lagfx_a64_hash64(blob, blen);
    out->release = pres_release, out->user = r;
    /* depth/stencil and raster state are no longer stubs: they are encoder state (tags 104 107 108 115 119, arm64_exec_run.c), applied dynamically. What stays taken from the pass
     * instead of the descriptor is the attachment pixel formats (stub_color_format_from_pass). stub_depth_stencil now counts passes the device could not give depth state to. */
    CNTP(p).stub_color_format_from_pass++;
    CNTP(p).resolve_failed--, CNTP(p).resolved++;
    ok = true;
    r = NULL;   /* owned by out now */
done:
    pres_release(r);
    free(blob); free(vspv); free(fspv);
    for (uint32_t i = 0; i < nlib; i++) free(libs[i].lib);
    return ok;
}
#endif /* LAGFX_HAVE_VULKAN */
