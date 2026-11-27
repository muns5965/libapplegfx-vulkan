/*
 * libapplegfx-vulkan — ARM64 pipeline-state resolver. See arm64_pipeline.h.
 */
#include "arm64_pipeline.h"
#include "arm64_objects.h"
#include "arm64_exec_decode.h"
#include "arm64_dump.h"
#include "../air2spv/constexpr_sampler.h"
#include "../air2spv/spv_reflect.h"
#include "../air2spv/translate.h"
#include "../air2spv/binding_alloc.h"
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

uint32_t lagfx_a64_frag_base(const uint8_t *vspv, size_t vsz) { return lagfx_ba_stage_base(vspv, vsz, LAGFX_A64_FRAG_BINDING_BASE); }

int lagfx_a64_fbfetch_check(int stage, bool have_local_read, uint32_t attachment, uint32_t n_bind) {
    if (stage == 0) return LAGFX_A64_FBF_VERTEX_STAGE;
    if (!have_local_read) return LAGFX_A64_FBF_NO_LOCAL_READ;
    /* the colour attachment is the InputAttachmentIndex, never the binding number (the number is a position in the module's table: it moves when resources are added) */
    if (attachment > 3u) return LAGFX_A64_FBF_BINDING_RANGE;
    if (n_bind >= LAGFX_A64_MAX_BINDINGS) return LAGFX_A64_FBF_TOO_MANY;   /* = LAGFX_A64_MAX_BINDINGS (kept literal: that macro lives in the exec header) */
    return LAGFX_A64_FBF_OK;
}
const char *lagfx_a64_fbfetch_reason_name(int reason) {
    static const char *const n[LAGFX_A64_FBF_REASONS] = { "ok", "vertex_stage", "no_local_read", "attachment_index", "too_many_bindings" };
    return reason >= 0 && reason < LAGFX_A64_FBF_REASONS ? n[reason] : "?";
}

/* Structural decode of the serialized MTLVertexDescriptor. Grammar (protocol facts, from the independent notes in the reference tree and checked against the 43
 * descriptors of the device dump): a field is `tag, 0x04, u32`; an entry is `u8 nfields` + fields; the descriptor body (after the 16-byte header) is one entry whose
 * tag 3 is the byte offset, from the header end, of the vertex descriptor root (an entry again: tag 0 = attribute section, tag 1 = layout section, offsets from the root
 * start). A section is `u32 n`, `u32 rel[n]` (offsets from the section start), then n entries. Attribute entry: tag 0 location, 1 format, 2 offset, 3 buffer index; layout
 * entry: tag 0 buffer index, 1 step function, 2 step rate, 3 stride. **A property equal to its default is not serialized**: an attribute at offset 0 has no tag 2 and one on
 * buffer 0 has no tag 3; a layout on buffer 0 has no tag 0 either. The byte-pattern scan this replaces needed all three of tags 1,2,3 back to
 * back and so dropped every attribute at offset 0 or on buffer 0 (the Float4 position of the compositor's 48-byte vertex, and BOTH attributes of the 32-byte quad vertex,
 * whose stride then fell back to the reflected tight 24). false = the blob does not have this shape (caller falls back to the scan). */
static bool entry_fields(const uint8_t *b, uint32_t n, uint32_t at, uint32_t tags_max, uint32_t *val, uint8_t *have, uint32_t *end) {
    if (at >= n) return false;
    uint32_t nf = b[at], p = at + 1u;
    for (uint32_t i = 0; i < tags_max; i++) val[i] = 0, have[i] = 0;
    for (uint32_t i = 0; i < nf; i++, p += 6u) {
        if (p + 6u > n || b[p + 1u] != 4u) return false;
        if (b[p] < tags_max) val[b[p]] = lagfx_le32(b + p + 2u), have[b[p]] = 1;
    }
    if (end) *end = p;
    return true;
}
static bool vdesc_section(const uint8_t *b, uint32_t n, uint64_t sec, uint32_t *cnt) {
    if (sec + 4u > n) return false;
    uint32_t c = lagfx_le32(b + sec);
    if (c > 64u || sec + 4u + 4ull * c > n) return false;
    *cnt = c;
    return true;
}
static bool decode_vertex_desc(const uint8_t *b, uint32_t limit, lagfx_a64_pdesc_t *out) {
    uint32_t val[8]; uint8_t have[8];
    if (!entry_fields(b, limit, 16u, 8u, val, have, NULL) || !have[3]) return false;
    uint64_t root = 16ull + val[3];
    uint32_t rv[4]; uint8_t rh[4];
    if (root >= limit || !entry_fields(b, limit, (uint32_t)root, 4u, rv, rh, NULL) || !rh[0]) return false;
    uint64_t asec = root + rv[0], lsec = root + rv[1];
    uint32_t na = 0, nl = 0;
    if (!vdesc_section(b, limit, asec, &na) || (rh[1] && !vdesc_section(b, limit, lsec, &nl))) return false;
    out->nattrs = 0, out->stride = 0;
    for (uint32_t i = 0; i < na; i++) {
        uint64_t e = asec + lagfx_le32(b + asec + 4u + 4u * i);
        uint32_t av[4]; uint8_t ah[4];
        if (e >= limit || !entry_fields(b, limit, (uint32_t)e, 4u, av, ah, NULL)) return false;
        if (av[1] == 0u || av[1] > 64u) continue;   /* format 0 = invalid: the attribute reads nothing */
        if (out->nattrs < LAGFX_A64_PDESC_MAX_ATTRS) out->attrs[out->nattrs++] = (lagfx_a64_vattr_t){ av[1], av[2], av[3] };
    }
    for (uint32_t a = 1; a < out->nattrs; a++) {   /* sorted by offset, like the scan (the pipeline builder matches attributes to the reflected inputs by order) */
        lagfx_a64_vattr_t t = out->attrs[a];
        uint32_t j = a;
        while (j > 0u && out->attrs[j - 1u].off > t.off) { out->attrs[j] = out->attrs[j - 1u]; j--; }
        out->attrs[j] = t;
    }
    if (out->nattrs) {   /* the layout of the attributes' buffer (the executor feeds one stream; the first attribute's buffer after the offset sort picks it) */
        uint32_t bi = out->attrs[0].bufidx;
        for (uint32_t i = 0; i < nl; i++) {
            uint64_t e = lsec + lagfx_le32(b + lsec + 4u + 4u * i);
            uint32_t lv[4]; uint8_t lh[4];
            if (e >= limit || !entry_fields(b, limit, (uint32_t)e, 4u, lv, lh, NULL)) return false;
            if (lv[0] == bi && lh[3]) { out->stride = lv[3]; break; }
        }
    }
    return true;
}

const char *lagfx_a64_root_status_name(int st) {
    static const char *const n[LAGFX_A64_ROOT_N] = { "n/a", "ok", "malformed", "missing", "same", "bad_type" };
    return st >= 0 && st < LAGFX_A64_ROOT_N ? n[st] : "?";
}

bool lagfx_a64_pdesc_decode(const uint8_t *blob, uint32_t n, bool (*is_fn)(void *ctx, uint32_t ref), void *ctx, lagfx_a64_pdesc_t *out) {
    if (!blob || !out || n < 16u) return false;
    memset(out, 0, sizeof *out);
    out->tag = lagfx_le32(blob), out->size = lagfx_le32(blob + 4), out->ref = lagfx_le32(blob + 8), out->length = lagfx_le32(blob + 12);
    uint32_t limit = out->length && 16ull + out->length <= n ? 16u + out->length : n;   /* the body the header declares, else what we have */

    /* `04 <u32>` tokens naming a function object (x86 object_resolver.c scans `04 XX`; ARM64 object numbers are not limited to a byte). DIAGNOSTIC ONLY for a render
     * pipeline (tag 0xe): the token scan also hits unrelated fields (the root's tag 0 constant 31 / 25 / 19, a vertex format 29, an offset 32 ...), and once such a number
     * is a function object in the task (run 20261126-231501: object 31 became a fragment function after it had been a texture) it is a plausible-looking but WRONG stage
     * function. */
    uint32_t nscan = 0, scan[LAGFX_A64_PDESC_MAX_REFS];
    for (uint32_t i = 16u; i + 5u <= limit && nscan < LAGFX_A64_PDESC_MAX_REFS; i++) {
        if (blob[i] != 0x04u) continue;
        uint32_t v = lagfx_le32(blob + i + 1u);
        if (!v || (is_fn && !is_fn(ctx, v))) continue;
        bool seen = false;
        for (uint32_t k = 0; k < nscan; k++) if (scan[k] == v) { seen = true; break; }
        if (!seen) scan[nscan++] = v;
    }
    memcpy(out->scan, scan, nscan * sizeof scan[0]);
    out->nscan = nscan;
    /* The functions come from the ROOT entry (the first entry after the header; all 37 + 36 tag-0xe PSOs of the device dumps of runs 231501 / 230109 carry tag 1 = vertex
     * function ref and tag 2 = fragment function ref there, tile PSOs (tag 0x33) tag 1 = kernel). A root that does not name two distinct function objects is NOT repaired from
     * the scan: root_status says why and the resolver refuses the pipeline. Only descriptors that have no such root (other tags; a tile PSO without tag 1) fall back to the
     * scanned function tokens, which the is_fn type check already restricts to function objects. */
    uint32_t rv[4]; uint8_t rh[4];
    bool rp = entry_fields(blob, limit, 16u, 4u, rv, rh, NULL);
    out->root_status = LAGFX_A64_ROOT_NA;
    if (rp) out->root_ref1 = rh[1] ? rv[1] : 0u, out->root_ref2 = rh[2] ? rv[2] : 0u;
    if (out->tag == 0xeu) {
        if (!rp) out->root_status = LAGFX_A64_ROOT_MALFORMED;
        else if (!rh[1] || !rh[2] || !rv[1] || !rv[2]) out->root_status = LAGFX_A64_ROOT_MISSING;
        else if (rv[1] == rv[2]) out->root_status = LAGFX_A64_ROOT_SAME;
        else if (is_fn && (!is_fn(ctx, rv[1]) || !is_fn(ctx, rv[2]))) out->root_status = LAGFX_A64_ROOT_BAD_TYPE;
        else out->root_status = LAGFX_A64_ROOT_OK, out->refs[0] = rv[1], out->refs[1] = rv[2], out->nrefs = 2;
    } else if (out->tag == 0x33u && rp && rh[1] && rv[1]) {
        if (is_fn && !is_fn(ctx, rv[1])) out->root_status = LAGFX_A64_ROOT_BAD_TYPE;
        else out->root_status = LAGFX_A64_ROOT_OK, out->refs[0] = rv[1], out->nrefs = 1;
    } else {
        memcpy(out->refs, scan, nscan * sizeof scan[0]);
        out->nrefs = nscan;
    }
    if (!decode_vertex_desc(blob, limit, out)) {   /* not the serialized-descriptor shape: the legacy byte scan */
    out->nattrs = 0, out->stride = 0;
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
    }
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
    lagfx_spv_cxs_t cx[LAGFX_CONSTEXPR_SAMPLER_MAX];
    size_t ncx = lagfx_spv_reflect_constexpr_samplers(spv, len, cx, LAGFX_CONSTEXPR_SAMPLER_MAX);
    if (ncx > LAGFX_CONSTEXPR_SAMPLER_MAX) ncx = LAGFX_CONSTEXPR_SAMPLER_MAX;
    for (size_t t = 0; t < nt; t++) {
        if (tmp[t].set != 0u) continue;
        if (t && tmp[t - 1].set == 0u && tmp[t - 1].binding == tmp[t].binding) continue;   /* byte-buffer views of one buffer alias its binding (translator): ONE descriptor and ONE Metal argument, not one per view */
        if (tmp[t].kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT) {   /* framebuffer fetch: the input attachment with InputAttachmentIndex n reads colour attachment n of the current pass */
            int why = lagfx_a64_fbfetch_check(st, have_local_read, tmp[t].role_index, *n_bind);
            if (why != LAGFX_A64_FBF_OK) { if (fbf_why) *fbf_why = why; return LAGFX_A64_MAP_FBFETCH; }
            bind[*n_bind].binding = (uint8_t)tmp[t].binding, bind[*n_bind].kind = (uint8_t)LAGFX_SPV_BINDING_INPUT_ATTACHMENT, bind[*n_bind].writable = 0;
            bind[*n_bind].stage = 1, bind[*n_bind].metal_index = (int16_t)tmp[t].role_index;
            bind[*n_bind].ia_class = tmp[t].sampled_class;
            (*n_bind)++;
            continue;
        }
        if (tmp[t].kind != LAGFX_SPV_BINDING_STORAGE_BUFFER && tmp[t].kind != LAGFX_SPV_BINDING_SAMPLED_IMAGE && tmp[t].kind != LAGFX_SPV_BINDING_SAMPLER &&
            tmp[t].kind != LAGFX_SPV_BINDING_STORAGE_IMAGE) return LAGFX_A64_MAP_FAIL;
        if (tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE && (st == 0 || !have_storage_img_write)) return st == 0 ? LAGFX_A64_MAP_VERTEX_RES : LAGFX_A64_MAP_FAIL;   /* fragment writes only; needs the device features */
        if (*n_bind >= LAGFX_A64_MAX_BINDINGS || tmp[t].binding > 255u) return LAGFX_A64_MAP_FAIL;
        uint8_t want = tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER ? 1 : (tmp[t].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE || tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE) ? 2 : 3;
        uint8_t want_w = tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_IMAGE;   /* textures split by access: written ones are storage images */
        if (tmp[t].kind == LAGFX_SPV_BINDING_SAMPLER) {   /* a constexpr sampler of the shader: no Metal slot; the host builds the VkSampler from the decoded AIR word */
            size_t q = 0;
            while (q < ncx && cx[q].binding != tmp[t].binding) q++;
            if (q < ncx) {
                bind[*n_bind].binding = (uint8_t)tmp[t].binding, bind[*n_bind].kind = (uint8_t)tmp[t].kind, bind[*n_bind].stage = (uint8_t)st, bind[*n_bind].metal_index = -1;
                bind[*n_bind].writable = 0, bind[*n_bind].cxs = 1, bind[*n_bind].cxs_raw = cx[q].raw;
                (*n_bind)++;
                continue;
            }
        }
        uint32_t n = nth[tmp[t].kind]++, seen = 0;
        int midx = -1; uint8_t wr = 0;
        for (size_t k = 0; k < nab; k++) if (ab[k].kind == want && (want != 2 || ab[k].written == want_w) && seen++ == n) { midx = ab[k].metal_index; wr = ab[k].written; break; }
        if (midx > 31 || (midx < 0 && tmp[t].kind != LAGFX_SPV_BINDING_SAMPLER)) return LAGFX_A64_MAP_FAIL;   /* a buffer/texture nobody can feed */
        bind[*n_bind].binding = (uint8_t)tmp[t].binding;
        bind[*n_bind].kind = (uint8_t)tmp[t].kind;
        bind[*n_bind].stage = (uint8_t)st;
        bind[*n_bind].metal_index = (int16_t)midx;
        bind[*n_bind].cxs = 0, bind[*n_bind].cxs_raw = 0, bind[*n_bind].dim3 = (uint8_t)(tmp[t].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE && tmp[t].image_dim == 2u);   /* Dim3D sampled image: needs a 3D view */
        /* writable = the arg metadata allows writes (air.write / air.read_write: every non-const `device T *`) AND the translated module has a use that may write through the
         * binding (reflect may_write). Metadata alone over-reports: VfxXgh's `vertex_buffer` is air.read_write, yet the function is `readonly` and its SPIR-V never stores to it. */
        bind[*n_bind].writable = tmp[t].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER ? (uint8_t)(wr && tmp[t].may_write) : 0u;
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
    VkShaderModule vs, fs;
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
    if ((d.tag == 0xeu || d.tag == 0x33u) && d.root_status != LAGFX_A64_ROOT_OK && d.root_status != LAGFX_A64_ROOT_NA) {   /* the root entry does not name the stage functions: refuse, never guess from the token scan */
        CNTP(p).rf_root[d.root_status]++;
        LAGFX_WARN("arm64 pipeline %u: descriptor root does not name valid stage functions (%s: tag1=%u tag2=%u; %u scanned function tokens ignored); pipeline refused", ref, lagfx_a64_root_status_name(d.root_status), d.root_ref1, d.root_ref2, d.nscan);
        goto done;
    }
    if (d.tag == 0x33u) { ok = lagfx_a64_tile_resolve(p, task, ref, blob, blen, &d, out); goto done; }   /* tile pipeline: a compute kernel (arm64_tile.c) */
    if (d.tag != 0xeu) { CNTP(p).rf_bad_tag++; goto done; }
    if (!d.nrefs) { CNTP(p).rf_no_refs++; goto done; }

    const lagfx_metallib_function_t *vf = NULL, *ff = NULL;
    for (uint32_t i = 0; i < 2u && i < d.nrefs; i++) {   /* refs[0] = tag 1 must yield a vertex function, refs[1] = tag 2 a fragment function; no other ref is tried */
        lib_t *l = &libs[nlib];
        memset(l, 0, sizeof *l);
        l->lib = lagfx_a64_read_function_lib(p, task, d.refs[i], &l->len, NULL);
        if (!l->lib) continue;
        nlib++;
        if (lagfx_metallib_extract_functions(l->lib, l->len, l->fn, 8, &l->nfn) != LAGFX_OK) continue;
        for (size_t k = 0; k < l->nfn && k < 8; k++) {
            if (!l->fn[k].bitcode || !l->fn[k].bitcode_len) continue;
            if (i == 0 && !vf && l->fn[k].stage == LAGFX_METALLIB_STAGE_VERTEX) vf = &l->fn[k];
            if (i == 1 && !ff && l->fn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT) ff = &l->fn[k];
        }
    }
    if (!nlib) { CNTP(p).rf_no_lib++; goto done; }
    if (!vf || !ff) { CNTP(p).rf_no_stage++; CNTP(p).rf_root_stage++; LAGFX_WARN("arm64 pipeline %u: root tag1=%u / tag2=%u name no vertex / fragment function (vertex %s, fragment %s); pipeline refused", ref, d.refs[0], d.refs[1], vf ? "ok" : "wrong stage", ff ? "ok" : "wrong stage"); goto done; }
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
    for (size_t v = 0; v < nvi; v++) out->desc.vtx_in_loc[v] = (uint8_t)vin[v].location, out->desc.vtx_in_comp[v] = (uint8_t)vin[v].components,
        out->desc.vtx_in_src_known[v] = vin[v].has_src_offset, out->desc.vtx_in_src_unfed[v] = vin[v].src_unfed, out->desc.vtx_in_src_off[v] = vin[v].src_offset,
        out->desc.vtx_in_buf_known[v] = vin[v].has_src_buffer, out->desc.vtx_in_buf[v] = vin[v].src_buffer;
    out->desc.n_vtx_inputs = (uint8_t)nvi;
    out->desc.pso_attrs_serialized = nvi ? 1u : 0u;   /* the descriptor is the layout source: a reflected input without a decoded attribute fails the pipeline (lagfx_pipeline_vertex_input) */
    if (nvi && d.nattrs) {
        for (uint32_t a = 1; a < d.nattrs; a++) if (d.attrs[a].bufidx != d.attrs[0].bufidx) { CNTP(p).rf_multi_stream++; goto done; }
        out->vertex_buffer_index = d.attrs[0].bufidx;
        out->desc.n_pso_attrs = (uint8_t)d.nattrs;
        for (uint32_t a = 0; a < d.nattrs; a++) out->desc.pso_attr_fmt[a] = d.attrs[a].fmt, out->desc.pso_attr_off[a] = d.attrs[a].off, out->desc.pso_attr_buf[a] = d.attrs[a].bufidx;
        out->desc.pso_attr_buf_known = 1u;
        out->desc.vtx_stride = d.stride;
    }

    /* descriptor bindings: fragment bindings move to a range disjoint from the vertex module's (as the x86 draw path does), then the merged set-0 layout */
    lagfx_spv_offset_bindings(fspv, fsz, lagfx_a64_frag_base(vspv, vsz));
    const uint8_t *blobs[2] = { vspv, fspv };
    const size_t lens[2] = { vsz, fsz };
    r = calloc(1, sizeof *r);
    if (!r) goto done;
    r->dev = vk->device;
    {   /* the pipeline's binding table: one derivation (shared allocator), refused loudly when a set+binding holds two descriptor types; its key identifies the layout in the pipeline caches */
        VkDescriptorSetLayoutBinding lb[LAGFX_LAYOUT_MAX_BINDINGS];
        uint32_t nlb = 0;
        lagfx_layout_info_t li;
        if (lagfx_layout_derive(blobs, lens, 2, lb, LAGFX_LAYOUT_MAX_BINDINGS, &nlb, &li) != LAGFX_OK) { CNTP(p).rf_layout++; LAGFX_WARN("arm64 pipeline %u: invalid descriptor binding table, pipeline refused:\n%s", ref, li.report); goto done; }
        out->layout_key = li.key;
    }
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
    if (!spv_entry_name(vspv, vsz, r->vname, sizeof r->vname)) strcpy(r->vname, "main");
    if (!spv_entry_name(fspv, fsz, r->fname, sizeof r->fname)) strcpy(r->fname, "main");

    out->desc.vertex_shader = r->vs, out->desc.fragment_shader = r->fs;
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
