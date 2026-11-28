/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — SPIR-V descriptor-binding reflection
 * src/air2spv/spv_reflect.c
 *
 * Copyright © 2026 Matthew Jackson
 */
#include "spv_reflect.h"

#include "common/le.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPV_MAGIC                0x07230203u
#define OP_DECORATE              71u
#define OP_TYPE_IMAGE            25u
#define OP_TYPE_SAMPLER          26u
#define OP_TYPE_SAMPLED_IMAGE    27u
#define OP_TYPE_STRUCT           30u
#define OP_TYPE_POINTER          32u
#define OP_VARIABLE              59u
#define OP_FUNCTION              54u
#define OP_LOAD                  61u
#define OP_ACCESS_CHAIN          65u
#define OP_IN_BOUNDS_ACCESS_CHAIN 66u
#define OP_PTR_ACCESS_CHAIN      67u
#define OP_ARRAY_LENGTH          68u
#define OP_IN_BOUNDS_PTR_ACCESS_CHAIN 70u
#define OP_COPY_OBJECT           83u
#define OP_BITCAST               124u
#define OP_ATOMIC_LOAD           227u
#define OP_TYPE_FLOAT            22u
#define OP_TYPE_VECTOR           23u
#define DECOR_BINDING            33u
#define DECOR_DESCRIPTOR_SET     34u
#define DECOR_INPUT_ATTACHMENT_INDEX 43u
#define DECOR_LOCATION           30u
#define STORAGE_UNIFORM_CONSTANT 0u
#define STORAGE_INPUT            1u
#define STORAGE_UNIFORM          2u
#define STORAGE_STORAGE_BUFFER   12u

/* Bounded id-indexed scratch. SPIR-V ids are dense and small for our
 * translator output; cap generously and ignore anything past it. */
#define MAXID 8192u

size_t lagfx_spv_reflect_bindings(const uint8_t *spv, size_t spv_len,
                                  lagfx_spv_binding_t *out, size_t cap) {
    if (!spv || spv_len < 20u) return 0;
    const uint8_t *p = spv;
    if (lagfx_le32(p) != SPV_MAGIC) return 0;
    size_t nwords = spv_len / 4u;
    size_t total = 0; /* declared up front: the OOM goto must not skip it */

    /* Use calloc'd arrays sized to MAXID, allocated on the heap to keep
     * the stack small. */
    uint8_t  *has_set = NULL, *has_bind = NULL, *is_var = NULL;
    uint32_t *dec_set = NULL, *dec_bind = NULL;
    uint32_t *var_ptr = NULL, *var_sc = NULL;         /* OpVariable: ptr type id, storage class */
    uint32_t *ptr_pointee = NULL;                     /* OpTypePointer: pointee id */
    uint8_t  *is_image = NULL, *is_sampler = NULL, *is_struct = NULL, *is_sampled_image = NULL, *image_dim = NULL, *image_arr = NULL;
    uint8_t  *type_class = NULL, *image_class = NULL;      /* OpTypeInt / OpTypeFloat id -> numeric class (0 float, 1 uint, 2 sint); OpTypeImage id -> class of its Sampled Type */
    uint32_t *root = NULL;                            /* id -> the storage-buffer variable a pointer id derives from (0 = none) */
    uint8_t  *var_written = NULL;                     /* per variable: something that may write through it was found */
    uint32_t *dec_role = NULL;                        /* InputAttachmentIndex / imageblock render target of the variable; LAGFX_SPV_NO_ROLE = none */
    bool      overflow_ids = false;

    size_t sz = MAXID;
    has_set = calloc(sz, 1); has_bind = calloc(sz, 1); is_var = calloc(sz, 1);
    dec_set = calloc(sz, sizeof(uint32_t)); dec_bind = calloc(sz, sizeof(uint32_t));
    var_ptr = calloc(sz, sizeof(uint32_t)); var_sc = calloc(sz, sizeof(uint32_t));
    ptr_pointee = calloc(sz, sizeof(uint32_t));
    is_image = calloc(sz, 1); image_dim = calloc(sz, 1); image_arr = calloc(sz, 1); is_sampler = calloc(sz, 1); is_struct = calloc(sz, 1);
    is_sampled_image = calloc(sz, 1);
    root = calloc(sz, sizeof(uint32_t)); var_written = calloc(sz, 1);
    type_class = calloc(sz, 1); image_class = calloc(sz, 1);
    dec_role = malloc(sz * sizeof(uint32_t));
    if (!root || !var_written || !dec_role || !type_class || !image_class) goto done;
    memset(dec_role, 0xff, sz * sizeof(uint32_t));
    if (!has_set || !has_bind || !is_var || !dec_set || !dec_bind || !var_ptr ||
        !var_sc || !ptr_pointee || !is_image || !image_dim || !image_arr || !is_sampler || !is_struct ||
        !is_sampled_image) {
        goto done; /* OOM -> report 0 */
    }

    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (i + wc > nwords) break;
        #define W(n) lagfx_le32(spv + (i + (n)) * 4u)
        switch (op) {
            case OP_DECORATE:
                if (wc >= 4u) {
                    uint32_t id = W(1), dec = W(2);
                    if (id < MAXID) {
                        if (dec == DECOR_DESCRIPTOR_SET) { has_set[id] = 1; dec_set[id] = W(3); }
                        else if (dec == DECOR_BINDING)   { has_bind[id] = 1; dec_bind[id] = W(3); }
                        else if (dec == DECOR_INPUT_ATTACHMENT_INDEX) dec_role[id] = W(3);
                    }
                }
                break;
            case 5u /* OpName */:
                if (wc >= 3u && W(1) < MAXID) {   /* "lagfx.imageblock.<n>": a translator imageblock storage image */
                    const char *nm = (const char *)(spv + (i + 2u) * 4u);
                    size_t maxlen = (size_t)(wc - 2u) * 4u, pl = sizeof LAGFX_IMAGEBLOCK_NAME_PREFIX - 1u, k = 0;
                    while (k < maxlen && nm[k]) k++;
                    if (k > pl && k < maxlen && memcmp(nm, LAGFX_IMAGEBLOCK_NAME_PREFIX, pl) == 0) {
                        uint32_t v = 0; bool ok = true;
                        for (size_t h = pl; h < k; h++) { if (nm[h] < '0' || nm[h] > '9' || v > 1000u) { ok = false; break; } v = v * 10u + (uint32_t)(nm[h] - '0'); }
                        if (ok) dec_role[W(1)] = v;
                    }
                }
                break;
            case OP_TYPE_IMAGE:
                if (wc >= 4u && W(1) < MAXID) { is_image[W(1)] = (W(3) == 6u) ? 2 : (wc >= 8u && W(7) == 2u) ? 3 : 1; image_dim[W(1)] = (uint8_t)W(3); if (wc >= 6u) image_arr[W(1)] = W(5) ? 1u : 0u; }
                if (wc >= 4u && W(1) < MAXID && W(2) < MAXID) image_class[W(1)] = type_class[W(2)];   /* Dim SubpassData = input attachment; Sampled=2 = storage image */
                break;
            case 21u /* OpTypeInt: Width, Signedness */:
                if (wc >= 4u && W(1) < MAXID) type_class[W(1)] = W(3) ? 2u : 1u;
                break;
            case OP_TYPE_SAMPLER:
                if (wc >= 2u && W(1) < MAXID) is_sampler[W(1)] = 1;
                break;
            case OP_TYPE_SAMPLED_IMAGE:
                if (wc >= 2u && W(1) < MAXID) is_sampled_image[W(1)] = 1;
                break;
            case OP_TYPE_STRUCT:
                if (wc >= 2u && W(1) < MAXID) is_struct[W(1)] = 1;
                break;
            case OP_TYPE_POINTER:
                if (wc >= 4u && W(1) < MAXID) ptr_pointee[W(1)] = W(3);
                break;
            case OP_VARIABLE:
                if (wc >= 4u && W(2) < MAXID) {
                    is_var[W(2)] = 1; var_ptr[W(2)] = W(1); var_sc[W(2)] = W(3);
                }
                break;
            default: break;
        }
        #undef W
        i += wc;
    }

    /* Which storage-buffer variables can be written? A pointer id derives from a variable through OpAccessChain / OpInBoundsAccessChain / OpPtrAccessChain /
     * OpInBoundsPtrAccessChain / OpBitcast / OpCopyObject. Inside function bodies the only uses that cannot write are OpLoad, OpAtomicLoad and OpArrayLength of
     * such a pointer; ANY other instruction that mentions a derived id (OpStore, atomics, OpCopyMemory*, OpFunctionCall, OpPhi, OpSelect, a pointer stored as a value,
     * an extended instruction, ...) marks the variable written. A literal that equals a derived id marks it too: only ever towards "may write". Declarations (everything
     * before the first OpFunction: decorations, types, constants, the variables themselves) are not uses. Definitions precede uses (SPIR-V rule) except OpPhi operands,
     * which are conservative anyway. */
    for (uint32_t id = 0; id < MAXID; id++)
        if (is_var[id] && var_sc[id] == STORAGE_STORAGE_BUFFER) root[id] = id;
    bool in_body = false;
    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        if (op == OP_FUNCTION) in_body = true;
        if (in_body) {
            #define W(n) lagfx_le32(spv + (i + (n)) * 4u)
            bool chain = (op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN || op == OP_IN_BOUNDS_PTR_ACCESS_CHAIN || op == OP_BITCAST || op == OP_COPY_OBJECT) && wc >= 4u;
            if (chain) {
                uint32_t res = W(2), base = W(3);
                if (base < MAXID && root[base]) {
                    if (res < MAXID) root[res] = root[base]; else overflow_ids = true;
                }
                if (op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN || op == OP_IN_BOUNDS_PTR_ACCESS_CHAIN)   /* indices are ids of integers, not pointers */
                    { i += wc; continue; }
            }
            if (!chain) {
                /* Word ranges [lo, hi) that can hold an ID operand which is (or carries) a pointer. Positions are per opcode so that literals (CompositeExtract indices, memory-access
                 * alignments, ...) are never mistaken for ids: a small literal equals the id of an early variable. Anything not listed here and not a known pointer-free opcode is
                 * handled by `default`. */
                uint32_t lo = 0, hi = 0, step = 1;
                switch (op) {
                    case 62u:                                   /* OpStore ptr object */
                    case 63u: lo = 1, hi = 3; break;            /* OpCopyMemory target source (a copy out of a buffer reads it, a copy into it writes it; both marked) */
                    case 64u: lo = 1, hi = 3; break;            /* OpCopyMemorySized target source size */
                    case 57u: lo = 4, hi = wc; break;           /* OpFunctionCall type id fn args... */
                    case 245u: lo = 3, hi = wc; step = 2; break;/* OpPhi type id (value, parent)... : values at 3, 5, ... */
                    case 169u: lo = 4, hi = 6; break;           /* OpSelect type id cond a b */
                    case 12u: lo = 5, hi = wc; break;           /* OpExtInst type id set inst args... (all args are ids) */
                    case 80u: lo = 3, hi = wc; break;           /* OpCompositeConstruct */
                    case 82u: lo = 3, hi = 5; break;            /* OpCompositeInsert type id object composite literals... */
                    case 254u: lo = 1, hi = 2; break;           /* OpReturnValue */
                    case 400u: lo = 3, hi = 4; break;           /* OpCopyLogical */
                    case 117u: case 121u: case 122u: case 123u: lo = 3, hi = 4; break;   /* OpConvertPtrToU, OpPtrCastToGeneric, OpGenericCastToPtr(+Explicit) */
                    case 228u: case 319u: lo = 1, hi = 2; break;   /* OpAtomicStore ptr ..., OpAtomicFlagClear ptr ... */
                    case 318u: lo = 3, hi = 4; break;              /* OpAtomicFlagTestAndSet type id ptr ... */
                    default:
                        if (op >= 229u && op <= 242u) lo = 3, hi = 4;          /* OpAtomicExchange .. OpAtomicXor: type id ptr ... */
                        else if (op >= 4000u) lo = 1, hi = wc;                 /* extension opcodes (float atomics, cooperative matrices, ...): unknown layout, every word */
                        break;
                }
                for (uint32_t w = lo; w < hi && w < wc; w += step) {
                    uint32_t v = W(w);
                    if (v < MAXID && root[v]) var_written[root[v]] = 1;
                }
            }
            #undef W
        }
        i += wc;
    }

    /* Emit a binding for every variable carrying BOTH set + binding. */
    for (uint32_t id = 0; id < MAXID; id++) {
        if (!is_var[id] || !has_set[id] || !has_bind[id]) continue;
        uint32_t ptr = var_ptr[id];
        uint32_t pointee = (ptr < MAXID) ? ptr_pointee[ptr] : 0u;
        lagfx_spv_binding_kind_t kind = LAGFX_SPV_BINDING_UNKNOWN;
        if (pointee < MAXID) {
            if (is_image[pointee] == 2)
                kind = LAGFX_SPV_BINDING_INPUT_ATTACHMENT;
            else if (is_image[pointee] == 3)
                kind = LAGFX_SPV_BINDING_STORAGE_IMAGE;
            else if (is_image[pointee] || is_sampled_image[pointee])
                kind = LAGFX_SPV_BINDING_SAMPLED_IMAGE;
            else if (is_sampler[pointee])
                kind = LAGFX_SPV_BINDING_SAMPLER;
            else if (is_struct[pointee] && var_sc[id] == STORAGE_STORAGE_BUFFER)
                kind = LAGFX_SPV_BINDING_STORAGE_BUFFER;
        }
        if (kind == LAGFX_SPV_BINDING_UNKNOWN) continue;
        if (total < cap) {
            out[total].set = dec_set[id];
            out[total].binding = dec_bind[id];
            out[total].kind = kind;
            out[total].image_dim = (kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE || kind == LAGFX_SPV_BINDING_STORAGE_IMAGE) && pointee < MAXID ? image_dim[pointee] : 0u;
            out[total].image_arrayed = (kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE || kind == LAGFX_SPV_BINDING_STORAGE_IMAGE) && pointee < MAXID ? image_arr[pointee] : 0u;
            out[total].may_write = kind == LAGFX_SPV_BINDING_STORAGE_BUFFER ? (uint8_t)(var_written[id] || overflow_ids) : 0u;
            out[total].sampled_class = (kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT || kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE || kind == LAGFX_SPV_BINDING_STORAGE_IMAGE) && pointee < MAXID ? image_class[pointee] : 0u;
            out[total].role_index = (kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT || kind == LAGFX_SPV_BINDING_STORAGE_IMAGE) ? dec_role[id] : LAGFX_SPV_NO_ROLE;
        }
        total++;
    }

    /* Insertion-sort the emitted prefix by (set, binding) for a stable,
     * caller-friendly order (descriptor-set layout binding arrays want
     * ascending bindings). Only the prefix actually written is sorted. */
    size_t n = total < cap ? total : cap;
    for (size_t a = 1; a < n; a++) {
        lagfx_spv_binding_t key = out[a];
        size_t b = a;
        while (b > 0 &&
               (out[b - 1].set > key.set ||
                (out[b - 1].set == key.set && out[b - 1].binding > key.binding))) {
            out[b] = out[b - 1];
            b--;
        }
        out[b] = key;
    }

    /* views of one binding (byte-buffer views of the same buffer) share the descriptor: it may be written when any view may */
    for (size_t a = 0; a < n; a++)
        if (out[a].kind == LAGFX_SPV_BINDING_STORAGE_BUFFER && out[a].may_write)
            for (size_t b = 0; b < n; b++) if (out[b].set == out[a].set && out[b].binding == out[a].binding) out[b].may_write = 1;

done:
    free(root); free(var_written); free(type_class); free(image_class);
    free(has_set); free(has_bind); free(is_var);
    free(dec_set); free(dec_bind); free(var_ptr); free(var_sc);
    free(ptr_pointee); free(is_image); free(image_dim); free(image_arr); free(is_sampler); free(is_struct);
    free(is_sampled_image); free(dec_role);
    return total;
}

uint32_t lagfx_spv_reflect_exec_model(const uint8_t *spv, size_t spv_len) {
    if (!spv || spv_len < 20u || lagfx_le32(spv) != SPV_MAGIC) return 0xffffffffu;
    size_t nwords = spv_len / 4u;
    for (size_t i = 5u; i < nwords;) {
        uint32_t hdr = lagfx_le32(spv + i * 4u), wc = hdr >> 16, op = hdr & 0xFFFFu;
        if (wc == 0u || i + wc > nwords) break;
        if (op == 15u /* OpEntryPoint */ && wc >= 4u) return lagfx_le32(spv + (i + 1u) * 4u);
        i += wc;
    }
    return 0xffffffffu;
}

void lagfx_spv_offset_bindings(uint8_t *spv, size_t spv_len, uint32_t base) {
    if (!spv || spv_len < 20u || base == 0u) return;
    if (lagfx_le32(spv) != SPV_MAGIC) return;
    size_t nwords = spv_len / 4u;
    for (size_t i = 5u; i < nwords;) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        /* OpDecorate <id> Binding <literal>: wc==4, W(2)==Binding(33). Add base
         * to the binding literal at word i+3. DescriptorSet (34) untouched. */
        if (op == OP_DECORATE && wc >= 4u
            && lagfx_le32(spv + (i + 2u) * 4u) == DECOR_BINDING) {
            uint32_t b = lagfx_le32(spv + (i + 3u) * 4u);
            lagfx_put_le32(spv + (i + 3u) * 4u, b + base);
        }
        i += wc;
    }
}

int lagfx_spv_explicit_lod0(const uint8_t *spv, size_t spv_len, uint8_t **out, size_t *out_len) {
    if (!spv || !out || !out_len || spv_len < 20u || (spv_len & 3u) || lagfx_le32(spv) != SPV_MAGIC) return -1;
    size_t nwords = spv_len / 4u, n_impl = 0, extra = 0, first_fn = 0;
    uint32_t f32 = 0, zero = 0;
    for (size_t i = 5u; i < nwords;) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint32_t wc = hdr >> 16, op = hdr & 0xFFFFu;
        if (wc == 0u || i + wc > nwords) return -1;
        if (op == OP_TYPE_FLOAT && wc >= 3u && lagfx_le32(spv + (i + 2u) * 4u) == 32u && !f32) f32 = lagfx_le32(spv + (i + 1u) * 4u);
        if (op == 43u && wc == 4u && f32 && lagfx_le32(spv + (i + 1u) * 4u) == f32 && lagfx_le32(spv + (i + 3u) * 4u) == 0u && !zero) zero = lagfx_le32(spv + (i + 2u) * 4u);   /* OpConstant %f32 %id 0.0 */
        if (op == 54u && !first_fn) first_fn = i;   /* OpFunction */
        if (op == 87u) {
            if (wc == 5u) extra += 2u;                                                               /* + mask, Lod */
            else if (wc == 7u && lagfx_le32(spv + (i + 5u) * 4u) == 0x8u) extra += 1u;               /* ConstOffset: mask |= Lod, + Lod id */
            else return -1;
            n_impl++;
        }
        i += wc;
    }
    if (!n_impl) return 0;
    if (!f32 || !first_fn) return -1;
    size_t new_const = zero ? 0u : 4u;
    uint32_t bound = lagfx_le32(spv + 12u);
    uint32_t zid = zero ? zero : bound;
    uint8_t *o = malloc((nwords + extra + new_const) * 4u);
    if (!o) return -1;
    size_t w = 0;
    #define EMIT(v) do { lagfx_put_le32(o + w * 4u, (v)); w++; } while (0)
    for (size_t i = 0; i < 5u; i++) EMIT(i == 3u ? bound + (zero ? 0u : 1u) : lagfx_le32(spv + i * 4u));
    for (size_t i = 5u; i < nwords;) {
        uint32_t hdr = lagfx_le32(spv + i * 4u), wc = hdr >> 16, op = hdr & 0xFFFFu;
        if (i == first_fn && new_const) { EMIT(4u << 16 | 43u); EMIT(f32); EMIT(zid); EMIT(0u); }   /* OpConstant %f32 %zid 0.0 */
        if (op == 87u) {
            EMIT((wc + (wc == 5u ? 2u : 1u)) << 16 | 88u);                                           /* OpImageSampleExplicitLod */
            for (uint32_t k = 1; k <= 4u; k++) EMIT(lagfx_le32(spv + (i + k) * 4u));                 /* type, result, sampled image, coordinate */
            if (wc == 5u) { EMIT(0x2u); EMIT(zid); }
            else { EMIT(0xAu); EMIT(zid); EMIT(lagfx_le32(spv + (i + 6u) * 4u)); }
        } else for (uint32_t k = 0; k < wc; k++) EMIT(lagfx_le32(spv + (i + k) * 4u));
        i += wc;
    }
    #undef EMIT
    *out = o, *out_len = w * 4u;
    return 1;
}

bool lagfx_spv_has_image_sample(const uint8_t *spv, size_t spv_len) {
    if (!spv || spv_len < 20u || lagfx_le32(spv) != SPV_MAGIC) return false;
    size_t nwords = spv_len / 4u;
    for (size_t i = 5u; i < nwords;) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        /* SPIR-V image-sample / fetch / read opcodes (the shader reads a
         * texture): OpImageSample* 87..96, OpImageFetch 95, OpImageGather 96/97,
         * OpImageRead 98, OpImageSparse* 305..320. If any appear, the shader
         * USES a texture — it must have a bound sampled-image descriptor. */
        if ((op >= 87u && op <= 98u) || (op >= 305u && op <= 320u)) return true;
        i += wc;
    }
    return false;
}

typedef struct { uint32_t id, off, bytes, buf; uint8_t has_off, has_bytes, has_buf, unfed; } vin_src_t;

size_t lagfx_spv_reflect_vertex_inputs(const uint8_t *spv, size_t spv_len,
                                       lagfx_spv_vertex_input_t *out, size_t cap) {
    vin_src_t vin_tab[16]; uint32_t vin_n = 0;
    if (!spv || spv_len < 20u) return 0;
    if (lagfx_le32(spv) != SPV_MAGIC) return 0;
    size_t nwords = spv_len / 4u;
    size_t total = 0;

    uint8_t  *has_loc = calloc(MAXID, 1);
    uint32_t *dec_loc = calloc(MAXID, sizeof(uint32_t));
    uint8_t  *is_var  = calloc(MAXID, 1);
    uint32_t *var_ptr = calloc(MAXID, sizeof(uint32_t));
    uint32_t *var_sc  = calloc(MAXID, sizeof(uint32_t));
    uint32_t *ptr_pointee = calloc(MAXID, sizeof(uint32_t));
    uint8_t  *vec_comp = calloc(MAXID, 1);   /* OpTypeVector: component count */
    if (!has_loc || !dec_loc || !is_var || !var_ptr || !var_sc || !ptr_pointee || !vec_comp) {
        goto done;
    }

    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        #define W(n) lagfx_le32(spv + (i + (n)) * 4u)
        switch (op) {
            case OP_DECORATE:
                if (wc >= 4u && W(1) < MAXID && W(2) == DECOR_LOCATION) {
                    has_loc[W(1)] = 1; dec_loc[W(1)] = W(3);
                }
                break;
            case OP_TYPE_POINTER:
                if (wc >= 4u && W(1) < MAXID) ptr_pointee[W(1)] = W(3);
                break;
            case OP_TYPE_VECTOR:
                /* W(1)=result id, W(3)=component count (W(2)=component type). */
                if (wc >= 4u && W(1) < MAXID) {
                    uint32_t c = W(3); vec_comp[W(1)] = (uint8_t)(c > 4u ? 4u : c);
                }
                break;
            case OP_TYPE_FLOAT:
                /* A scalar float type as a vertex input → 1 component. Mark it
                 * via vec_comp=1 so a non-vector float attribute still resolves. */
                if (wc >= 2u && W(1) < MAXID) vec_comp[W(1)] = 1u;
                break;
            case 5u: /* OpName: "lagfx.vin.o<off>[.s<bytes>][.b<slot>]" */
                if (wc >= 3u && W(1) < MAXID) {
                    const char *str = (const char *)(spv + (i + 2u) * 4u);
                    size_t maxlen = (size_t)(wc - 2u) * 4u, k = 0;
                    while (k < maxlen && str[k]) k++;
                    static const char pre[] = "lagfx.vin.o";
                    if (k < maxlen && !strcmp(str, "lagfx.vin.u") && vin_n < 16u) {
                        vin_src_t *e = &vin_tab[vin_n++]; memset(e, 0, sizeof *e); e->id = W(1); e->unfed = 1;
                    } else if (k < maxlen && k > sizeof pre - 1u && !memcmp(str, pre, sizeof pre - 1u) && vin_n < 16u) {
                        vin_src_t *e = &vin_tab[vin_n]; memset(e, 0, sizeof *e); e->id = W(1);
                        const char *q = str + sizeof pre - 1u; char tag = 'o'; bool bad = false;
                        while (*q && !bad) {
                            if (*q < '0' || *q > '9') { bad = true; break; }
                            uint64_t v = 0; while (*q >= '0' && *q <= '9') { v = v * 10u + (uint64_t)(*q++ - '0'); if (v > 0x7fffffffu) bad = true; }
                            if (tag == 'o') e->off = (uint32_t)v, e->has_off = 1; else if (tag == 's') e->bytes = (uint32_t)v, e->has_bytes = 1; else e->buf = (uint32_t)v, e->has_buf = 1;
                            if (!*q) break;
                            if (*q != '.' || (q[1] != 's' && q[1] != 'b')) { bad = true; break; }
                            tag = q[1]; q += 2;
                        }
                        if (!bad && e->has_off) vin_n++;
                    }
                }
                break;
            case OP_VARIABLE:
                if (wc >= 4u && W(2) < MAXID) {
                    is_var[W(2)] = 1; var_ptr[W(2)] = W(1); var_sc[W(2)] = W(3);
                }
                break;
            default: break;
        }
        #undef W
        i += wc;
    }

    for (uint32_t id = 0; id < MAXID; id++) {
        if (!is_var[id] || var_sc[id] != STORAGE_INPUT || !has_loc[id]) continue;
        uint32_t ptr = var_ptr[id];
        uint32_t pointee = (ptr < MAXID) ? ptr_pointee[ptr] : 0u;
        uint32_t comp = (pointee < MAXID) ? vec_comp[pointee] : 0u;
        if (comp == 0u) comp = 4u; /* default to vec4 if the type wasn't a vector/float */
        if (total < cap) {
            out[total].location = dec_loc[id];
            out[total].components = comp;
            out[total].has_src_offset = out[total].has_src_bytes = out[total].has_src_buffer = 0;
            out[total].src_offset = out[total].src_bytes = out[total].src_buffer = 0;
            out[total].src_unfed = 0;
            for (uint32_t t = 0; t < vin_n; t++) if (vin_tab[t].id == id) {
                if (vin_tab[t].unfed) { out[total].src_unfed = 1; continue; }
                out[total].has_src_offset = 1, out[total].src_offset = vin_tab[t].off;
                if (vin_tab[t].has_bytes) out[total].has_src_bytes = 1, out[total].src_bytes = vin_tab[t].bytes;
                if (vin_tab[t].has_buf) out[total].has_src_buffer = 1, out[total].src_buffer = vin_tab[t].buf;
            }
        }
        total++;
    }

    /* Sort the written prefix by location. */
    size_t n = total < cap ? total : cap;
    for (size_t a = 1; a < n; a++) {
        lagfx_spv_vertex_input_t key = out[a];
        size_t b = a;
        while (b > 0 && out[b - 1].location > key.location) { out[b] = out[b - 1]; b--; }
        out[b] = key;
    }

done:
    free(has_loc); free(dec_loc); free(is_var); free(var_ptr); free(var_sc);
    free(ptr_pointee); free(vec_comp);
    return total;
}

/* Constexpr samplers: the translator names each such sampler variable "lagfx.cxs.<16 hex>" (OpName); the binding is the variable's Binding decoration. */
size_t lagfx_spv_reflect_constexpr_samplers(const uint8_t *spv, size_t spv_len, lagfx_spv_cxs_t *out, size_t cap) {
    if (!spv || spv_len < 20u || lagfx_le32(spv) != SPV_MAGIC) return 0;
    size_t nwords = spv_len / 4u, total = 0;
    uint64_t *raw = calloc(MAXID, sizeof *raw);
    uint32_t *bind = calloc(MAXID, sizeof *bind);
    uint8_t *has_raw = calloc(MAXID, 1), *has_bind = calloc(MAXID, 1);
    if (!raw || !bind || !has_raw || !has_bind) goto done;
    static const char pre[] = "lagfx.cxs.";
    for (size_t i = 5u; i < nwords;) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16), op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        #define W(n) lagfx_le32(spv + (i + (n)) * 4u)
        if (op == 5u /* OpName */ && wc >= 3u && W(1) < MAXID) {
            const char *str = (const char *)(spv + (i + 2u) * 4u);
            size_t maxlen = (size_t)(wc - 2u) * 4u, k = 0;
            while (k < maxlen && str[k]) k++;
            if (k == sizeof pre - 1u + 16u && memcmp(str, pre, sizeof pre - 1u) == 0) {
                uint64_t v = 0; bool ok = true;
                for (size_t h = 0; h < 16u; h++) {
                    char ch = str[sizeof pre - 1u + h]; uint32_t d;
                    if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0'); else if (ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10); else { ok = false; break; }
                    v = v << 4 | d;
                }
                if (ok) { raw[W(1)] = v; has_raw[W(1)] = 1; }
            }
        } else if (op == OP_DECORATE && wc >= 4u && W(1) < MAXID && W(2) == DECOR_BINDING) { bind[W(1)] = W(3); has_bind[W(1)] = 1; }
        #undef W
        i += wc;
    }
    for (uint32_t id = 0; id < MAXID; id++) {
        if (!has_raw[id] || !has_bind[id]) continue;
        if (total < cap) out[total].binding = bind[id], out[total].raw = raw[id];
        total++;
    }
done:
    free(raw); free(bind); free(has_raw); free(has_bind);
    return total;
}


/* ---- workgroup memory reflection ---- */
typedef struct {
    uint8_t  kind[MAXID];            /* 1 int, 2 float, 3 vector, 4 array, 5 struct, 6 pointer */
    uint32_t a[MAXID], b[MAXID];     /* int/float: width; vector: component type / count; array: element / length id; pointer: pointee */
    uint8_t  has_val[MAXID];  uint32_t val[MAXID];     /* OpConstant / OpSpecConstant value */
    uint8_t  is_spec[MAXID];  uint32_t spec_id[MAXID];
    uint32_t members_at[MAXID], nmembers[MAXID];       /* struct: word offset of the member list */
    char     name[MAXID][24];        /* OpName, only "lagfx.tgarg." names are kept */
} wg_info_t;

static const uint32_t *wg_words(const uint8_t *spv, size_t len, size_t *nw) {
    if (!spv || len < 20u || (len & 3u) || *(const uint32_t *)(const void *)spv != SPV_MAGIC) return NULL;
    *nw = len / 4u; return (const uint32_t *)(const void *)spv;
}

/* Size and natural alignment of a Workgroup-storage type. Scalars: width/8; vectors: component size times count, aligned to the power of two >= their size (3-vectors round up to 4); arrays: element
 * size times length (the element's size is its own stride, no extra padding); structs: members laid out in order at their natural alignment, the total rounded up to the largest member alignment.
 * Drivers may add more (std430 rounding of an array stride, 16-byte vec3): the figure is an estimate that includes alignment padding, not a guarantee of a device's allocation. */
/* checked arithmetic: any size past 2^40 bytes (far beyond every device limit) or an overflow makes the type unsizeable, i.e. the caller refuses */
#define WG_MAX (1ull << 40)
static bool wg_mul(uint64_t a, uint64_t b, uint64_t *r) { return !__builtin_mul_overflow(a, b, r) && *r <= WG_MAX; }
static bool wg_add(uint64_t a, uint64_t b, uint64_t *r) { return !__builtin_add_overflow(a, b, r) && *r <= WG_MAX; }
static bool wg_round(uint64_t v, uint64_t a, uint64_t *r) { uint64_t t; return a && wg_add(v, a - 1u, &t) && (*r = t / a * a, true); }
static uint64_t wg_ceil_pow2(uint64_t v) { uint64_t p = 1; while (p < v) p <<= 1; return p; }
static bool wg_size_of(const wg_info_t *in, const uint32_t *w, uint32_t t, const uint32_t (*spec)[2], size_t nspec, uint64_t *out, uint64_t *align, int depth) {
    if (t == 0 || t >= MAXID || depth > 8) return false;
    uint64_t al = 1;
    switch (in->kind[t]) {
    case 1: case 2: if (in->a[t] == 0 || in->a[t] > 64u || (in->a[t] & 7u)) return false; *out = in->a[t] / 8u; if (align) *align = *out; return true;
    case 3: { uint64_t e, ea; if (!wg_size_of(in, w, in->a[t], spec, nspec, &e, &ea, depth + 1)) return false; if (!wg_mul(e, in->b[t], out)) return false; if (align) *align = wg_ceil_pow2(*out); return true; }
    case 4: {
        uint64_t e, ea; if (!wg_size_of(in, w, in->a[t], spec, nspec, &e, &ea, depth + 1)) return false;
        uint32_t lid = in->b[t]; if (lid >= MAXID || !in->has_val[lid]) return false;
        uint32_t n = in->val[lid];
        if (in->is_spec[lid]) for (size_t i = 0; i < nspec; i++) if (spec[i][0] == in->spec_id[lid]) n = spec[i][1];
        if (n == 0) return false;                 /* a zero-length array is not a valid type */
        uint64_t stride; if (!wg_round(e, ea, &stride) || !wg_mul(stride, n, out)) return false;   /* the element stride is its size rounded up to its alignment (vec3 -> 16) */
        if (align) *align = ea; return true;
    }
    case 5: {
        uint64_t off = 0;
        for (uint32_t i = 0; i < in->nmembers[t]; i++) {
            uint64_t e, ea; if (!wg_size_of(in, w, w[in->members_at[t] + i], spec, nspec, &e, &ea, depth + 1)) return false;
            uint64_t o2; if (!wg_round(off, ea, &o2) || !wg_add(o2, e, &off)) return false; if (ea > al) al = ea;
        }
        if (!wg_round(off, al, out)) return false; if (align) *align = al; return true;
    }
    default: return false;
    }
}

/* One pass over the module: types, constants, spec constants, names, and the Workgroup variables (appended to vars[]). */
static bool wg_scan(const uint8_t *spv, size_t len, wg_info_t *in, uint32_t *vars, size_t *nvars, size_t vcap) {
    size_t nw; const uint32_t *w = wg_words(spv, len, &nw);
    if (!w) return false;
    *nvars = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (wc == 0 || i + wc > nw) return false;
        const uint32_t *o = &w[i + 1];
        switch (op) {
        case 5: if (wc >= 3 && o[0] < MAXID && !strncmp((const char *)&o[1], "lagfx.tgarg.", 12)) { strncpy(in->name[o[0]], (const char *)&o[1], sizeof in->name[0] - 1); } break;
        case 21: if (wc >= 4 && o[0] < MAXID) { in->kind[o[0]] = 1; in->a[o[0]] = o[1]; } break;
        case 22: if (wc >= 3 && o[0] < MAXID) { in->kind[o[0]] = 2; in->a[o[0]] = o[1]; } break;
        case 23: if (wc >= 4 && o[0] < MAXID) { in->kind[o[0]] = 3; in->a[o[0]] = o[1]; in->b[o[0]] = o[2]; } break;
        case 28: if (wc >= 4 && o[0] < MAXID) { in->kind[o[0]] = 4; in->a[o[0]] = o[1]; in->b[o[0]] = o[2]; } break;
        case 30: if (wc >= 2 && o[0] < MAXID) { in->kind[o[0]] = 5; in->members_at[o[0]] = (uint32_t)(i + 2); in->nmembers[o[0]] = wc - 2u; } break;
        case 32: if (wc >= 4 && o[0] < MAXID) { in->kind[o[0]] = 6; in->a[o[0]] = o[2]; in->b[o[0]] = o[1]; } break;
        case 43: if (wc >= 4 && o[1] < MAXID) { in->has_val[o[1]] = 1; in->val[o[1]] = o[2]; } break;
        case 50: if (wc >= 4 && o[1] < MAXID) { in->has_val[o[1]] = 1; in->val[o[1]] = o[2]; in->is_spec[o[1]] = 1; } break;
        case 71: if (wc >= 4 && o[0] < MAXID && o[1] == 1u /* SpecId */) in->spec_id[o[0]] = o[2]; break;
        case 59: if (wc >= 4 && o[0] < MAXID && o[2] == 4u /* Workgroup */ ) { if (*nvars >= vcap) return false; vars[(*nvars)++] = o[0]; } break;
        default: break;
        }
        i += wc;
    }
    return true;
}

bool lagfx_spv_reflect_workgroup_bytes(const uint8_t *spv, size_t spv_len, const uint32_t (*spec)[2], size_t nspec, uint64_t *bytes) {
    wg_info_t *in = calloc(1, sizeof *in); uint32_t vars[256]; size_t nv = 0;
    if (!in || !bytes) { free(in); return false; }
    size_t nw; const uint32_t *w = wg_words(spv, spv_len, &nw);
    bool ok = w && wg_scan(spv, spv_len, in, vars, &nv, 256);
    uint64_t total = 0;
    for (size_t k = 0; ok && k < nv; k++) {   /* vars[] holds the RESULT TYPE (pointer) ids: recorded as o[0] of OpVariable */
        uint64_t s; uint32_t pt = vars[k];
        if (pt >= MAXID || in->kind[pt] != 6 || !wg_size_of(in, w, in->a[pt], spec, nspec, &s, NULL, 0)) { ok = false; break; }
        if (!wg_add(total, s, &total)) { ok = false; break; }
    }
    free(in);
    if (ok) *bytes = total;
    return ok;
}

size_t lagfx_spv_reflect_threadgroup_args(const uint8_t *spv, size_t spv_len, lagfx_spv_tgarg_t *out, size_t cap) {
    wg_info_t *in = calloc(1, sizeof *in); uint32_t vars[256]; size_t nv = 0, total = 0;
    if (!in) return 0;
    if (wg_scan(spv, spv_len, in, vars, &nv, 256)) {
        for (uint32_t id = 1; id < MAXID; id++) {
            unsigned n, b;
            if (!in->name[id][0] || sscanf(in->name[id], "lagfx.tgarg.%u.%u", &n, &b) != 2) continue;
            if (total < cap && out) out[total] = (lagfx_spv_tgarg_t){ n, b, in->spec_id[id] };
            total++;
        }
        if (out) for (size_t i = 1; i < total && i < cap; i++) for (size_t j = i; j > 0 && out[j].spec_id < out[j - 1].spec_id; j--) { lagfx_spv_tgarg_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    }
    free(in);
    return total;
}

bool lagfx_spv_reflect_spec_default(const uint8_t *spv, size_t spv_len, uint32_t spec_id, uint32_t *value) {
    wg_info_t *in = calloc(1, sizeof *in); uint32_t vars[256]; size_t nv = 0; bool found = false;
    if (!in) return false;
    if (wg_scan(spv, spv_len, in, vars, &nv, 256) || nv >= 256u) {
        for (uint32_t id = 1; id < MAXID && !found; id++) if (in->is_spec[id] && in->spec_id[id] == spec_id && in->has_val[id]) { if (value) *value = in->val[id]; found = true; }
    }
    free(in);
    return found;
}
