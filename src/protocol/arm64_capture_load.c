/*
 * libapplegfx-vulkan — ARM64 render replay capture: loader (no Vulkan). See arm64_capture.h for the format.
 */
#include "arm64_capture.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXKV 96u
typedef struct { const char *k, *v; } kv_t;

static unsigned split(char *line, kv_t *kv) {
    unsigned n = 0;
    char *s = strtok(line, " \t\r\n");   /* the cap_* kind */
    while ((s = strtok(NULL, " \t\r\n")) && n < MAXKV) {
        char *e = strchr(s, '=');
        if (!e) continue;
        *e = 0, kv[n].k = s, kv[n].v = e + 1, n++;
    }
    return n;
}
static const char *get(const kv_t *kv, unsigned n, const char *k) {
    for (unsigned i = 0; i < n; i++) if (!strcmp(kv[i].k, k)) return kv[i].v;
    return NULL;
}
static uint64_t u(const kv_t *kv, unsigned n, const char *k) { const char *v = get(kv, n, k); return v ? strtoull(v, NULL, 10) : 0; }
static int64_t sgn(const kv_t *kv, unsigned n, const char *k) { const char *v = get(kv, n, k); return v ? strtoll(v, NULL, 10) : 0; }
static uint64_t hx(const kv_t *kv, unsigned n, const char *k) { const char *v = get(kv, n, k); return v ? strtoull(v, NULL, 16) : 0; }
static void str(const kv_t *kv, unsigned n, const char *k, char *dst, size_t cap) { const char *v = get(kv, n, k); snprintf(dst, cap, "%s", v ? v : ""); }
/* comma list of floats / uints into a fixed array; returns the count */
static unsigned flist(const char *v, float *dst, unsigned max) {
    unsigned c = 0;
    while (v && *v && c < max) { char *e; dst[c++] = strtof(v, &e); if (*e != ',') break; v = e + 1; }
    return c;
}
static unsigned ulist(const char *v, uint32_t *dst, unsigned max, unsigned stride_skip, uint32_t *dst2) {
    unsigned c = 0;
    while (v && *v && c < max) {
        char *e;
        dst[c] = (uint32_t)strtoul(v, &e, 10);
        if (dst2 && *e == ':') { dst2[c] = (uint32_t)strtoul(e + 1, &e, 10); }
        c++;
        if (*e != ',') break;
        v = e + 1;
    }
    (void)stride_skip;
    return c;
}

#define PUSH(arr, cnt, cap_, T) ({ if ((cnt) == (cap_)) { size_t nc_ = (cap_) ? (cap_) * 2 : 64; T *t_ = realloc((arr), nc_ * sizeof(T)); if (!t_) goto oom; (arr) = t_; (cap_) = nc_; } memset(&(arr)[(cnt)], 0, sizeof(T)); &(arr)[(cnt)++]; })

void lagfx_cap_free(lagfx_cap_t *c) {
    if (!c) return;
    free(c->blobs), free(c->passes), free(c->atts), free(c->imgs), free(c->bufs), free(c->samps), free(c->draws), free(c->binds);
    free(c);
}

lagfx_cap_t *lagfx_cap_load(const char *dir, const char **err) {
    static const char *e0 = "";
    if (err) *err = e0;
    if (!dir) { if (err) *err = "no dir"; return NULL; }
    char path[640];
    snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { if (err) *err = "cannot open index.txt"; return NULL; }
    lagfx_cap_t *c = calloc(1, sizeof *c);
    if (!c) { fclose(f); if (err) *err = "out of memory"; return NULL; }
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    size_t cb = 0, cp = 0, ca = 0, ci = 0, cu = 0, cs = 0, cd = 0, cn = 0;
    bool have_header = false;
    char *line = malloc(1 << 16);
    if (!line) goto oom;
    while (fgets(line, 1 << 16, f)) {
        if (strncmp(line, "cap_", 4)) continue;
        char kind[32];
        { size_t k = 0; while (line[k] && line[k] != ' ' && line[k] != '\n' && k < sizeof kind - 1) kind[k] = line[k], k++; kind[k] = 0; }
        kv_t kv[MAXKV];
        unsigned n = split(line, kv);
        if (!strcmp(kind, "cap_header")) {   /* a new run: drop everything parsed so far */
            free(c->blobs), free(c->passes), free(c->atts), free(c->imgs), free(c->bufs), free(c->samps), free(c->draws), free(c->binds);
            char d[512]; memcpy(d, c->dir, sizeof d);
            memset(c, 0, sizeof *c); memcpy(c->dir, d, sizeof d);
            cb = cp = ca = ci = cu = cs = cd = cn = 0;
            c->version = (uint32_t)u(kv, n, "version");
            have_header = true;
            continue;
        }
        if (!have_header) continue;
        if (!strcmp(kind, "cap_blob")) {
            lagfx_cap_blob_t *b = PUSH(c->blobs, c->nblobs, cb, lagfx_cap_blob_t);
            b->id = (uint32_t)u(kv, n, "id"), b->bytes = u(kv, n, "bytes"), b->full = u(kv, n, "full"), b->trunc = u(kv, n, "trunc") != 0, b->hash = hx(kv, n, "hash");
            str(kv, n, "file", b->file, sizeof b->file);
        } else if (!strcmp(kind, "cap_pass")) {
            lagfx_cap_pass_t *q = PUSH(c->passes, c->npasses, cp, lagfx_cap_pass_t);
            q->id = (uint32_t)u(kv, n, "id"), q->seq = (uint32_t)u(kv, n, "seq"), q->exec = (uint32_t)u(kv, n, "exec"), q->task = (uint32_t)u(kv, n, "task");
            q->w = (uint32_t)u(kv, n, "w"), q->h = (uint32_t)u(kv, n, "h"), q->ncolor = (uint32_t)u(kv, n, "ncolor");
            q->depth = u(kv, n, "depth") != 0, q->stencil = u(kv, n, "stencil") != 0, q->depth_ref = (uint32_t)u(kv, n, "depth_ref"), q->depth_vkfmt = (uint32_t)u(kv, n, "depth_vkfmt");
            q->depth_load = (uint32_t)u(kv, n, "depth_load"), q->stencil_load = (uint32_t)u(kv, n, "stencil_load"), q->stencil_clear = (uint32_t)u(kv, n, "stencil_clear");
            const char *dc = get(kv, n, "depth_clear"); q->depth_clear = dc ? strtof(dc, NULL) : 0.0f;
        } else if (!strcmp(kind, "cap_att")) {
            lagfx_cap_att_t *a = PUSH(c->atts, c->natts, ca, lagfx_cap_att_t);
            a->pass = (uint32_t)u(kv, n, "pass"), a->slot = (uint32_t)u(kv, n, "slot"), a->ref = (uint32_t)u(kv, n, "ref"), a->gen = u(kv, n, "gen");
            a->vkfmt = (uint32_t)u(kv, n, "vkfmt"), a->mtlfmt = (uint32_t)u(kv, n, "mtlfmt"), a->w = (uint32_t)u(kv, n, "w"), a->h = (uint32_t)u(kv, n, "h"), a->bpp = (uint32_t)u(kv, n, "bpp");
            a->load = (uint32_t)u(kv, n, "load"), a->pre = (uint32_t)u(kv, n, "pre"), a->post = (uint32_t)u(kv, n, "post");
            flist(get(kv, n, "clear"), a->clear, 4);
            str(kv, n, "post_png", a->post_png, sizeof a->post_png);
        } else if (!strcmp(kind, "cap_img")) {
            lagfx_cap_img_t *m = PUSH(c->imgs, c->nimgs, ci, lagfx_cap_img_t);
            m->id = (uint32_t)u(kv, n, "id"), m->task = (uint32_t)u(kv, n, "task"), m->ref = (uint32_t)u(kv, n, "ref"), m->gen = u(kv, n, "gen");
            m->vkfmt = (uint32_t)u(kv, n, "vkfmt"), m->mtlfmt = (uint32_t)u(kv, n, "mtlfmt"), m->w = (uint32_t)u(kv, n, "w"), m->h = (uint32_t)u(kv, n, "h"), m->d = (uint32_t)u(kv, n, "d");
            m->mips = (uint32_t)u(kv, n, "mips"), m->layers = (uint32_t)u(kv, n, "layers"), m->bpp = (uint32_t)u(kv, n, "bpp"), m->row = u(kv, n, "row");
            const char *lo = get(kv, n, "layout"); m->mirror = lo && !strcmp(lo, "mirror");
            m->tex_base = u(kv, n, "tex_base"), m->tex_stride = u(kv, n, "tex_stride"), m->block_h = (uint32_t)u(kv, n, "block_h");
            m->blob = (uint32_t)u(kv, n, "blob"), m->len = u(kv, n, "len"), m->pass = (uint32_t)u(kv, n, "pass");
            str(kv, n, "use", m->use, sizeof m->use);
            const char *mt = get(kv, n, "miptab");
            while (mt && *mt && m->nmip_tab < 16) {   /* off:row:w:h:d, ... */
                char *e; unsigned i = m->nmip_tab;
                m->mip[i].off = strtoull(mt, &e, 10); if (*e != ':') break;
                m->mip[i].row = strtoull(e + 1, &e, 10); if (*e != ':') break;
                m->mip[i].w = (uint32_t)strtoul(e + 1, &e, 10); if (*e != ':') break;
                m->mip[i].h = (uint32_t)strtoul(e + 1, &e, 10); if (*e != ':') break;
                m->mip[i].d = (uint32_t)strtoul(e + 1, &e, 10);
                m->nmip_tab++;
                if (*e != ',') break;
                mt = e + 1;
            }
        } else if (!strcmp(kind, "cap_buf")) {
            lagfx_cap_buf_t *b = PUSH(c->bufs, c->nbufs, cu, lagfx_cap_buf_t);
            b->id = (uint32_t)u(kv, n, "id"), b->task = (uint32_t)u(kv, n, "task"), b->ref = (uint32_t)u(kv, n, "ref"), b->gen = u(kv, n, "gen"), b->length = u(kv, n, "length");
            b->off = u(kv, n, "off"), b->len = u(kv, n, "len"), b->blob = (uint32_t)u(kv, n, "blob"), b->pass = (uint32_t)u(kv, n, "pass");
        } else if (!strcmp(kind, "cap_samp")) {
            lagfx_cap_samp_t *s = PUSH(c->samps, c->nsamps, cs, lagfx_cap_samp_t);
            s->id = (uint32_t)u(kv, n, "id"), s->ref = (uint32_t)u(kv, n, "ref"), s->gen = u(kv, n, "gen");
            s->min_filter = (uint32_t)u(kv, n, "min"), s->mag_filter = (uint32_t)u(kv, n, "mag"), s->mip_filter = (uint32_t)u(kv, n, "mip"), s->mip_enabled = (uint32_t)u(kv, n, "mip_enabled");
            s->addr_u = (uint32_t)u(kv, n, "addr_u"), s->addr_v = (uint32_t)u(kv, n, "addr_v"), s->addr_w = (uint32_t)u(kv, n, "addr_w"), s->aniso = (uint32_t)u(kv, n, "aniso");
            s->compare_enable = (uint32_t)u(kv, n, "compare_enable"), s->compare_op = (uint32_t)u(kv, n, "compare_op"), s->border = (uint32_t)u(kv, n, "border"), s->unnorm = (uint32_t)u(kv, n, "unnorm");
            const char *v;
            s->lod_min = (v = get(kv, n, "lod_min")) ? strtof(v, NULL) : 0.0f, s->lod_max = (v = get(kv, n, "lod_max")) ? strtof(v, NULL) : 0.0f, s->lod_bias = (v = get(kv, n, "lod_bias")) ? strtof(v, NULL) : 0.0f;
        } else if (!strcmp(kind, "cap_draw")) {
            lagfx_cap_draw_t *d = PUSH(c->draws, c->ndraws, cd, lagfx_cap_draw_t);
            d->n = (uint32_t)u(kv, n, "n"), d->seq = (uint32_t)u(kv, n, "seq"), d->pass = (uint32_t)u(kv, n, "pass"), d->task = (uint32_t)u(kv, n, "task"), d->tag = (uint32_t)u(kv, n, "tag");
            d->prim = (uint32_t)u(kv, n, "prim"), d->indexed = (uint32_t)u(kv, n, "indexed"), d->count = u(kv, n, "count"), d->instances = u(kv, n, "instances");
            d->first_vertex = u(kv, n, "first_vertex"), d->base_instance = u(kv, n, "base_instance"), d->base_vertex = sgn(kv, n, "base_vertex");
            d->index_start = (uint32_t)u(kv, n, "index_start"), d->index_type = (uint32_t)u(kv, n, "index_type"), d->index_off = u(kv, n, "index_off"), d->index_buf = (uint32_t)u(kv, n, "index_buf");
            d->pipe_ref = (uint32_t)u(kv, n, "pipe_ref"), d->pipe_gen = u(kv, n, "pipe_gen"), d->pipe_hash = hx(kv, n, "pipe_hash"), d->vs_spv = (uint32_t)u(kv, n, "vs_spv"), d->fs_spv = (uint32_t)u(kv, n, "fs_spv");
            d->vtx_buf_slot = (uint32_t)u(kv, n, "vtx_buf_slot"), d->vtx_buf = (uint32_t)u(kv, n, "vtx_buf"), d->vtx_off = u(kv, n, "vtx_off"), d->vtx_stride = (uint32_t)u(kv, n, "vtx_stride");
            d->n_vtx_in = ulist(get(kv, n, "vtx_in"), d->vtx_in_loc, 8, 0, d->vtx_in_comp);
            d->n_vtx_attr = ulist(get(kv, n, "vtx_attrs"), d->vtx_attr_fmt, 8, 0, d->vtx_attr_off);
            flist(get(kv, n, "vp"), d->vp, 6);
            { float t[4]; flist(get(kv, n, "sc"), t, 4); for (int i = 0; i < 4; i++) d->sc[i] = (uint32_t)t[i]; }
            d->cull = (uint32_t)u(kv, n, "cull"), d->winding = (uint32_t)u(kv, n, "winding"); flist(get(kv, n, "bias"), d->bias, 3);
            d->blend_set = (uint32_t)u(kv, n, "blend_set"), d->blend = (uint32_t)u(kv, n, "blend"), d->write_mask = (uint32_t)u(kv, n, "write_mask"), d->ncolor_fmts = (uint32_t)u(kv, n, "ncolor_fmts");
            { uint32_t t[3] = {0}; ulist(get(kv, n, "blend_rgb"), t, 3, 0, NULL); memcpy(d->blend_rgb, t, sizeof t); memset(t, 0, sizeof t); ulist(get(kv, n, "blend_a"), t, 3, 0, NULL); memcpy(d->blend_a, t, sizeof t); }
            ulist(get(kv, n, "color_fmts"), d->color_fmts, 4, 0, NULL);
            d->depth_test = (uint32_t)u(kv, n, "depth_test"), d->depth_write = (uint32_t)u(kv, n, "depth_write"), d->depth_cmp = (uint32_t)u(kv, n, "depth_cmp"), d->stencil_test = (uint32_t)u(kv, n, "stencil_test");
            ulist(get(kv, n, "sref"), d->sref, 2, 0, NULL);
            const char *sk = get(kv, n, "skip"); snprintf(d->skip, sizeof d->skip, "%s", sk && strcmp(sk, "-") ? sk : "");
            d->nbind = 0, d->first_bind = (uint32_t)c->nbinds;
        } else if (!strcmp(kind, "cap_bind")) {
            lagfx_cap_bind_t *b = PUSH(c->binds, c->nbinds, cn, lagfx_cap_bind_t);
            b->draw = (uint32_t)u(kv, n, "draw"), b->binding = (uint32_t)u(kv, n, "binding"), b->idx = (uint32_t)u(kv, n, "idx"), b->writable = (uint32_t)u(kv, n, "writable");
            b->buf = (uint32_t)u(kv, n, "buf"), b->img = (uint32_t)u(kv, n, "img"), b->samp = (uint32_t)u(kv, n, "samp"), b->off = u(kv, n, "off"), b->range = u(kv, n, "range");
            str(kv, n, "kind", b->kind, sizeof b->kind), str(kv, n, "stage", b->stage, sizeof b->stage);
            if (c->ndraws && c->draws[c->ndraws - 1].n == b->draw) c->draws[c->ndraws - 1].nbind++;   /* binds follow their draw line */
        } else c->lines_unknown++;
    }
    free(line);
    fclose(f);
    if (!have_header) { lagfx_cap_free(c); if (err) *err = "no cap_header in index.txt"; return NULL; }
    return c;
oom:
    free(line);
    fclose(f);
    lagfx_cap_free(c);
    if (err) *err = "out of memory";
    return NULL;
}

#define FIND(fn, T, arr, cnt) const T *fn(const lagfx_cap_t *c, uint32_t id) { if (!c || !id) return NULL; for (size_t i = 0; i < c->cnt; i++) if (c->arr[i].id == id) return &c->arr[i]; return NULL; }
FIND(lagfx_cap_find_blob, lagfx_cap_blob_t, blobs, nblobs)
FIND(lagfx_cap_find_img, lagfx_cap_img_t, imgs, nimgs)
FIND(lagfx_cap_find_buf, lagfx_cap_buf_t, bufs, nbufs)
FIND(lagfx_cap_find_samp, lagfx_cap_samp_t, samps, nsamps)
FIND(lagfx_cap_find_pass, lagfx_cap_pass_t, passes, npasses)

uint8_t *lagfx_cap_read_blob(const lagfx_cap_t *c, uint32_t id, size_t *len) {
    const lagfx_cap_blob_t *b = lagfx_cap_find_blob(c, id);
    if (!b || strchr(b->file, '/')) return NULL;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", c->dir, b->file);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = malloc(b->bytes ? b->bytes : 1);
    size_t got = buf ? fread(buf, 1, b->bytes, f) : 0;
    fclose(f);
    if (!buf || got != b->bytes) { free(buf); return NULL; }
    if (len) *len = got;
    return buf;
}
