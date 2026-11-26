/* SPDX-License-Identifier: MIT */
/* src/vulkan/spv_iface.c — see spv_iface.h */
#include "spv_iface.h"

#include <stdlib.h>
#include <string.h>

enum { OP_NAME = 5, OP_MEMBER_NAME = 6, OP_ENTRY_POINT = 15, OP_CAPABILITY = 17, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_MATRIX = 24, OP_TYPE_IMAGE = 25,
       OP_TYPE_ARRAY = 28, OP_TYPE_RTARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_VARIABLE = 59, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72,
       OP_TYPE_BOOL = 20 };
enum { D_RELAXED = 0, D_BLOCK = 2, D_FLAT = 14, D_NOPERSP = 13, D_CENTROID = 16, D_SAMPLE = 17, D_PATCH = 15, D_BUILTIN = 11, D_LOCATION = 30, D_COMPONENT = 31, D_BINDING = 33, D_SET = 34, D_IAI = 43 };

typedef struct {
    uint16_t op;          /* defining opcode of the id (type / variable / const) */
    uint32_t a, b, c;     /* type: int{width,sign}, float{width}, vector{comp type,count}, matrix{col type,count}, array{elem,length id}, pointer{storage class, pointee}; var{type, storage class}; const{value} */
    uint32_t loc, comp, builtin, binding, set, iai;
    uint8_t  flags;       /* 1 has loc, 2 has comp, 4 has builtin, 8 has iai, 16 flat, 32 noperspective, 64 centroid, 128 sample */
    uint8_t  flags2;      /* 1 patch, 2 relaxed, 4 has binding, 8 has set */
    int32_t  name;        /* word offset of the OpName string, -1 none */
} idinfo_t;

static void cpy_name(char *dst, size_t cap, const uint32_t *w, size_t nw, int32_t at) {
    dst[0] = 0;
    if (at < 0) return;
    size_t n = 0;
    for (size_t i = (size_t)at; i < nw && n + 1 < cap; i++)
        for (int k = 0; k < 4 && n + 1 < cap; k++) { char ch = (char)(w[i] >> (8 * k)); if (!ch) { dst[n] = 0; return; } dst[n++] = ch; }
    dst[n] = 0;
}

/* scalar kind / width / vector size / locations of the (array / matrix / vector / scalar) type `t` */
static void type_shape(const idinfo_t *ids, uint32_t bound, uint32_t t, lagfx_spv_io_t *io, uint32_t mult) {
    uint32_t depth = 0, nloc = 1, cols = 1, ncomp = 1;
    while (t < bound && depth++ < 8) {
        const idinfo_t *x = &ids[t];
        if (x->op == OP_TYPE_ARRAY) { uint32_t len = x->b < bound && ids[x->b].op == OP_CONSTANT ? ids[x->b].a : 1; nloc *= len ? len : 1; t = x->a; continue; }
        if (x->op == OP_TYPE_MATRIX) { cols = x->b; if (cols < 1) cols = 1; t = x->a; continue; }
        if (x->op == OP_TYPE_VECTOR) { ncomp = x->b; t = x->a; continue; }
        if (x->op == OP_TYPE_FLOAT) { io->kind = 0, io->width = x->a; break; }
        if (x->op == OP_TYPE_INT) { io->kind = x->b ? 1 : 2, io->width = x->a; break; }
        io->kind = 3, io->width = x->op == OP_TYPE_BOOL ? 32 : 0; break;
    }
    if (io->width == 0) io->width = 32;
    uint32_t per_loc = (io->width == 64 && ncomp > 2) ? 2u : 1u;   /* a 64-bit vec3/vec4 takes two locations */
    io->ncomp = ncomp;
    io->nloc = nloc * cols * per_loc * mult;
    io->ncomp_total = nloc * cols * ncomp * (io->width == 64 ? 2u : 1u) * mult;
}

bool lagfx_spv_iface_reflect(const uint32_t *w, size_t nw, uint32_t model, lagfx_spv_iface_t *o) {
    memset(o, 0, sizeof *o);
    if (nw < 5 || w[0] != 0x07230203u) return false;
    uint32_t bound = w[3];
    if (bound == 0 || bound > 4000000u) return false;
    idinfo_t *ids = calloc(bound, sizeof *ids);
    if (!ids) return false;
    for (uint32_t i = 0; i < bound; i++) ids[i].name = -1;
    o->version = w[1];
    size_t i = 5;
    uint32_t ep_id = 0, ep_n = 0, iface[512];
    bool found = false;
    /* pass 1: everything except per-variable shapes */
    while (i < nw) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > nw) { free(ids); return false; }
        const uint32_t *p = &w[i + 1];
        uint32_t id;
        switch (op) {
        case OP_CAPABILITY: if (p[0] == 40) o->cap_input_attachment = true; break;
        case OP_ENTRY_POINT:
            if (!found && p[0] == model) {
                found = true, ep_id = p[1];
                size_t nm = 2; for (; nm < len - 1; nm++) { uint32_t x = p[nm]; if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x >> 24)) { nm++; break; } }
                cpy_name(o->entry, sizeof o->entry, p, len - 1, 2);
                ep_n = 0;
                for (size_t k = nm; k < len - 1 && ep_n < 512; k++) iface[ep_n++] = p[k];
            }
            break;
        case OP_NAME: id = p[0]; if (id < bound) ids[id].name = (int32_t)(i + 2); break;
        case OP_TYPE_BOOL: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op; break;
        case OP_TYPE_INT: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[1], ids[id].b = p[2]; break;
        case OP_TYPE_FLOAT: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[1]; break;
        case OP_TYPE_VECTOR: case OP_TYPE_MATRIX: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[1], ids[id].b = p[2]; break;
        case OP_TYPE_ARRAY: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[1], ids[id].b = p[2]; break;
        case OP_TYPE_RTARRAY: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[1]; break;
        case OP_TYPE_STRUCT: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op; break;
        case OP_TYPE_IMAGE: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[2]; /* Dim */ break;
        case OP_TYPE_POINTER: id = p[0]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[1], ids[id].b = p[2]; break;
        case OP_CONSTANT: id = p[1]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = len > 3 ? p[2] : 0; break;
        case OP_VARIABLE: id = p[1]; if (id < bound) ids[id].op = (uint16_t)op, ids[id].a = p[0], ids[id].b = p[2]; break;
        case OP_DECORATE:
            id = p[0];
            if (id < bound) {
                idinfo_t *x = &ids[id];
                switch (p[1]) {
                case D_LOCATION: x->loc = p[2], x->flags |= 1; break;
                case D_COMPONENT: x->comp = p[2], x->flags |= 2; break;
                case D_BUILTIN: x->builtin = p[2], x->flags |= 4; break;
                case D_IAI: x->iai = p[2], x->flags |= 8; break;
                case D_FLAT: x->flags |= 16; break;
                case D_NOPERSP: x->flags |= 32; break;
                case D_CENTROID: x->flags |= 64; break;
                case D_SAMPLE: x->flags |= 128; break;
                case D_PATCH: x->flags2 |= 1; break;
                case D_RELAXED: x->flags2 |= 2; break;
                case D_BINDING: x->binding = p[2], x->flags2 |= 4; break;
                case D_SET: x->set = p[2], x->flags2 |= 8; break;
                default: break;
                }
            }
            break;
        default: break;
        }
        i += len;
    }
    if (!found) { free(ids); return false; }
    (void)ep_id;
    o->exec_model = model;
    /* pass 2: members of Block structs (gl_PerVertex etc.) carry BuiltIn / Location per member: record the BuiltIn members of an interface struct variable as builtins */
    typedef struct { uint32_t st, member, builtin, loc; bool has_b, has_l; } mem_t;
    mem_t *mem = NULL; size_t nmem = 0, capm = 0;
    for (i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (op == OP_MEMBER_DECORATE && len >= 4 && (w[i + 3] == D_BUILTIN || w[i + 3] == D_LOCATION)) {
            if (nmem == capm) { capm = capm ? capm * 2 : 32; mem = realloc(mem, capm * sizeof *mem); if (!mem) { free(ids); return false; } }
            mem_t m = { .st = w[i + 1], .member = w[i + 2] };
            if (w[i + 3] == D_BUILTIN) m.builtin = w[i + 4], m.has_b = true; else m.loc = w[i + 4], m.has_l = true;
            mem[nmem++] = m;
        }
        i += len;
    }
    for (uint32_t k = 0; k < ep_n; k++) {
        uint32_t v = iface[k];
        if (v >= bound || ids[v].op != OP_VARIABLE) continue;
        uint32_t sc = ids[v].b, pt = ids[v].a;
        if (pt >= bound || ids[pt].op != OP_TYPE_POINTER) continue;
        uint32_t base = ids[pt].b;
        if (sc != 1 && sc != 3) continue;   /* Input / Output; the input attachments (UniformConstant) are listed below */
        lagfx_spv_io_t io;
        memset(&io, 0, sizeof io);
        io.id = v;
        cpy_name(io.name, sizeof io.name, w, nw, ids[v].name);
        io.flat = ids[v].flags & 16, io.noperspective = ids[v].flags & 32, io.centroid = ids[v].flags & 64, io.sample = ids[v].flags & 128, io.patch = ids[v].flags2 & 1, io.relaxed = ids[v].flags2 & 2;
        /* arrays of Block structs (per-vertex arrays) are not used by VS / FS; an array of a scalar type multiplies locations */
        uint32_t t = base;
        if (ids[t].op == OP_TYPE_STRUCT) {
            /* record builtin members as builtin entries, other members as one unknown-location entry */
            bool any = false;
            for (size_t m = 0; m < nmem; m++) if (mem[m].st == t && mem[m].has_b) {
                lagfx_spv_io_t b = io; b.has_builtin = true, b.builtin = mem[m].builtin; b.kind = 3, b.width = 32, b.ncomp = 1, b.nloc = 0;
                lagfx_spv_io_t *dst = sc == 1 ? o->in : o->out; uint32_t *n = sc == 1 ? &o->n_in : &o->n_out;
                if (*n < LAGFX_SPV_IFACE_MAX_IO) dst[(*n)++] = b; else o->truncated = true;
                any = true;
            }
            (void)any;
            continue;
        }
        if (ids[v].flags & 4) io.has_builtin = true, io.builtin = ids[v].builtin;
        if (ids[v].flags & 1) io.has_loc = true, io.loc = ids[v].loc;
        io.comp = (ids[v].flags & 2) ? ids[v].comp : 0;
        type_shape(ids, bound, t, &io, 1);
        if (io.has_builtin) io.nloc = 0;
        lagfx_spv_io_t *dst = sc == 1 ? o->in : o->out; uint32_t *n = sc == 1 ? &o->n_in : &o->n_out;
        if (*n < LAGFX_SPV_IFACE_MAX_IO) dst[(*n)++] = io; else o->truncated = true;
    }
    /* input attachments: every UniformConstant variable whose (array-stripped) type is an OpTypeImage with Dim SubpassData (6) */
    for (uint32_t v = 1; v < bound; v++) {
        if (ids[v].op != OP_VARIABLE || ids[v].b != 0) continue;
        uint32_t pt = ids[v].a, t = pt < bound && ids[pt].op == OP_TYPE_POINTER ? ids[pt].b : 0;
        for (int d = 0; d < 4 && t < bound && (ids[t].op == OP_TYPE_ARRAY || ids[t].op == OP_TYPE_RTARRAY); d++) t = ids[t].a;
        if (t >= bound || ids[t].op != OP_TYPE_IMAGE || ids[t].a != 6) continue;
        if (o->n_ia >= LAGFX_SPV_IFACE_MAX_IA) { o->truncated = true; break; }
        lagfx_spv_ia_t *a = &o->ia[o->n_ia++];
        a->id = v, a->set = ids[v].set, a->binding = ids[v].binding, a->has_index = ids[v].flags & 8, a->index = ids[v].iai;
        for (uint32_t k = 0; k < ep_n; k++) if (iface[k] == v) a->in_interface = true;
        cpy_name(a->name, sizeof a->name, w, nw, ids[v].name);
    }
    free(mem);
    free(ids);
    o->ok = true;
    return true;
}
