/*
 * libapplegfx-vulkan — ARM64 render replay capture: writer. Format and sync assumptions: arm64_capture.h. Recorder interface: arm64_capture_rec.h.
 */
#include "arm64_capture.h"
#include "arm64_dump.h"
#include "arm64_pipeline.h"
#include "../common/log.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool lagfx_a64_capture_enabled(void) {
    const char *e = getenv("LAGFX_ARM64_DUMP_DATA");
    return lagfx_a64_dump_enabled() && e && e[0] == '1' && !e[1];
}

#ifndef LAGFX_HAVE_VULKAN
void lagfx_a64_capture_reset(void) {}
#else

#include "arm64_capture_rec.h"

#define BLOB_TAB 4096u

static struct {
    bool header;
    uint32_t next_blob, next_img, next_buf, next_samp, next_pass;
    uint32_t next_n;            /* process-wide draw number (survives header; reset only by lagfx_a64_capture_reset) */
    uint32_t recorded;          /* draws recorded (cap MAX_DRAWS) */
    uint32_t execs;
    uint64_t total;             /* new blob bytes written */
    struct { uint64_t hash, bytes, full; uint32_t id; } blobs[BLOB_TAB];
    uint32_t nblobs;
} C;

void lagfx_a64_capture_reset(void) { memset(&C, 0, sizeof C); }

static uint64_t env_u64(const char *name, uint64_t def) {
    const char *e = getenv(name);
    if (!e || !e[0]) return def;
    char *end = NULL;
    unsigned long long v = strtoull(e, &end, 0);
    return end && !*end && v ? v : def;
}
static uint64_t max_obj(void) { return env_u64("LAGFX_ARM64_DUMP_DATA_MAX_OBJ", 8ull << 20); }
static uint64_t max_total(void) { return env_u64("LAGFX_ARM64_DUMP_DATA_MAX_TOTAL", 128ull << 20); }
static uint64_t max_exec(void) { return env_u64("LAGFX_ARM64_DUMP_DATA_MAX_EXEC", 64ull << 20); }
static uint64_t max_draws(void) { return env_u64("LAGFX_ARM64_DUMP_DATA_MAX_DRAWS", 256); }

/* draw number filter: "a-b", "a-", "a" ; false = no filter */
static bool draw_range(uint64_t *lo, uint64_t *hi) {
    const char *e = getenv("LAGFX_ARM64_DUMP_DATA_DRAWS");
    *lo = 0, *hi = UINT64_MAX;
    if (!e || !e[0]) return false;
    char *end;
    *lo = strtoull(e, &end, 10);
    if (*end == '-') { end++; if (*end) *hi = strtoull(end, NULL, 10); }
    else *hi = *lo;
    return true;
}
static bool pipe_filter_ok(uint32_t ref) {
    const char *e = getenv("LAGFX_ARM64_DUMP_DATA_PIPE");
    if (!e || !e[0]) return true;
    while (*e) {
        char *end;
        if ((uint32_t)strtoul(e, &end, 10) == ref) return true;
        if (end == e) break;
        e = *end == ',' ? end + 1 : end;
        if (end == e && *e) break;
    }
    return false;
}
/* 0 executed only, 1 executed + skipped, 2 skipped only */
static int fail_mode(void) {
    const char *e = getenv("LAGFX_ARM64_DUMP_DATA_FAIL");
    if (!e || !e[0]) return 0;
    return !strcmp(e, "only") ? 2 : strcmp(e, "0") ? 1 : 0;
}

/* ---------------------------------------------------------------- in-memory record ---- */

enum { M_BUF = 0, M_TEX_MIRROR = 1, M_TEX_TIGHT = 2 };
typedef struct {
    lagfx_a64_obj_t *o; uint64_t gen; uint32_t ref, task; int mode; uint32_t pass;   /* pass index (0-based within this cap), UINT32_MAX = pre snapshot not yet owned */
    uint64_t minoff; bool used;
    bool done, ok; lagfx_a64_snap_t s;
    uint32_t out_id;                                                                    /* id written to the index (0 = not written yet) */
} csnap_t;

typedef struct { lagfx_a64_cap_att_in_t in; int pre, post; } catt_t;
typedef struct {
    uint32_t seq; lagfx_a64_cap_pass_in_t in; catt_t att[4]; uint32_t natt;
    uint32_t ndraw, first_draw;
    uint32_t id;
} cpass_t;
typedef struct {
    uint32_t pass, n, seq; char skip[24];
    lagfx_a64_cap_draw_in_t in;
    int vs_spv, fs_spv, vb_snap, ib_snap, bind_snap[LAGFX_A64_MAX_BINDINGS];
} cdraw_t;
typedef struct { uint64_t hash; uint32_t *w; size_t bytes; } cspv_t;

struct lagfx_a64_cap {
    lagfx_protocol_t *p; lagfx_a64_rexec_t *x; uint32_t task, exec, seq;
    uint64_t mem;
    csnap_t *snaps; uint32_t nsnaps, capsnaps;
    cpass_t *passes; uint32_t npasses, cappasses; bool pass_open;
    cdraw_t *draws; uint32_t ndraws, capdraws;
    cspv_t *spv; uint32_t nspv, capspv;
    struct { uint32_t ref; uint64_t gen; float lo, hi; uint32_t id; } samp[64]; uint32_t nsamp;
};

#define GROW(arr, cnt, cap_) do { if ((cnt) == (cap_)) { uint32_t nc_ = (cap_) ? (cap_) * 2u : 16u; void *t_ = realloc((arr), nc_ * sizeof *(arr)); if (!t_) return -1; (arr) = t_; (cap_) = nc_; } } while (0)

static void cap_free(lagfx_a64_cap_t *c) {
    if (!c) return;
    for (uint32_t i = 0; i < c->nspv; i++) free(c->spv[i].w);
    free(c->spv), free(c->snaps), free(c->passes), free(c->draws), free(c);
}

/* ---------------------------------------------------------------- index / blob output ---- */

static void iline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void iline(const char *fmt, ...) {
    int fd = lagfx_a64_dump_index_fd();
    if (fd < 0) return;
    char l[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(l, sizeof l - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n > sizeof l - 2) n = (int)sizeof l - 2;
    l[n++] = '\n';
    if (write(fd, l, (size_t)n) < 0) { /* best effort */ }
}

static bool write_file(const char *name, const void *data, uint64_t bytes, bool *existed) {
    int dfd = lagfx_a64_dump_dir_fd();
    *existed = false;
    if (dfd < 0) return false;
    int fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { if (errno == EEXIST) { *existed = true; return true; } return false; }
    uint64_t done = 0;
    while (done < bytes) {
        ssize_t w = write(fd, (const uint8_t *)data + done, (size_t)(bytes - done));
        if (w <= 0) break;
        done += (uint64_t)w;
    }
    close(fd);
    return done == bytes;
}

/* 0 = no data (empty, cap reached or write failed). Identical content (hash + sizes) is written once per run and reuses its blob id. */
static uint32_t put_blob(lagfx_protocol_t *p, const void *data, uint64_t bytes, uint64_t full) {
    if (!data || !bytes) return 0;
    uint64_t h = lagfx_a64_hash64(data, (size_t)bytes);
    for (uint32_t i = 0; i < C.nblobs; i++)
        if (C.blobs[i].hash == h && C.blobs[i].bytes == bytes && C.blobs[i].full == full) { p->diag2.exec.pipe.cap_blobs_dedup++; return C.blobs[i].id; }
    if (C.total + bytes > max_total()) { p->diag2.exec.pipe.cap_skipped_cap++; return 0; }
    char name[64];
    snprintf(name, sizeof name, "cap_%016llx.bin", (unsigned long long)h);
    bool existed;
    if (!write_file(name, data, bytes, &existed)) { p->diag2.exec.pipe.cap_failed++; return 0; }
    if (!existed) C.total += bytes, p->diag2.exec.pipe.cap_bytes += bytes;
    uint32_t id = ++C.next_blob;
    if (C.nblobs < BLOB_TAB) C.blobs[C.nblobs].hash = h, C.blobs[C.nblobs].bytes = bytes, C.blobs[C.nblobs].full = full, C.blobs[C.nblobs].id = id, C.nblobs++;
    iline("cap_blob id=%u file=%s bytes=%llu full=%llu trunc=%d hash=%016llx", id, name, (unsigned long long)bytes, (unsigned long long)full, bytes < full, (unsigned long long)h);
    if (bytes < full) p->diag2.exec.pipe.cap_truncated++;
    return id;
}

/* ---------------------------------------------------------------- PNG (optional, stored deflate) ---- */

static uint32_t crc_tab[256]; static bool crc_init;
static uint32_t crc32_upd(uint32_t crc, const uint8_t *d, size_t n) {
    if (!crc_init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; } crc_init = true; }
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *d++) & 255u] ^ (crc >> 8);
    return ~crc;
}
static void be32(uint8_t *d, uint32_t v) { d[0] = (uint8_t)(v >> 24), d[1] = (uint8_t)(v >> 16), d[2] = (uint8_t)(v >> 8), d[3] = (uint8_t)v; }
static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *data, uint32_t n) {
    be32(out, n); memcpy(out + 4, type, 4); if (n) memcpy(out + 8, data, n);
    be32(out + 8 + n, crc32_upd(0, out + 4, 4 + (size_t)n));
    return 12u + n;
}
static float h2f(uint16_t h) {
    uint32_t s = h >> 15, e = (h >> 10) & 31u, m = h & 1023u;
    float f = !e ? ldexpf((float)m, -24) : e == 31u ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024u), (int)e - 25);
    return s ? -f : f;
}
static uint8_t q8(float v) { return v != v || v <= 0.0f ? 0 : v >= 1.0f ? 255 : (uint8_t)(v * 255.0f + 0.5f); }

/* RGBA8 PNG of a tight-layout image; false if the format is not convertible. */
static bool write_png(lagfx_protocol_t *p, uint32_t vkfmt, uint32_t w, uint32_t h, const uint8_t *src, uint64_t have, char *name, size_t cap) {
    uint32_t bpp = vkfmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : (vkfmt == VK_FORMAT_B8G8R8A8_UNORM || vkfmt == VK_FORMAT_B8G8R8A8_SRGB || vkfmt == VK_FORMAT_R8G8B8A8_UNORM || vkfmt == VK_FORMAT_R8G8B8A8_SRGB) ? 4u : 0u;
    if (!bpp || !w || !h || w > 16384 || h > 16384 || have < (uint64_t)w * h * bpp) return false;
    size_t rowb = (size_t)w * 4u + 1u, raw = rowb * h;
    uint8_t *rawb = malloc(raw);
    if (!rawb) return false;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *o = rawb + y * rowb;
        *o++ = 0;
        const uint8_t *s = src + (size_t)y * w * bpp;
        for (uint32_t x = 0; x < w; x++, s += bpp, o += 4) {
            if (bpp == 8) { for (int c = 0; c < 4; c++) o[c] = q8(h2f((uint16_t)(s[2 * c] | s[2 * c + 1] << 8))); }
            else if (vkfmt == VK_FORMAT_B8G8R8A8_UNORM || vkfmt == VK_FORMAT_B8G8R8A8_SRGB) o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = s[3];
            else memcpy(o, s, 4);
        }
    }
    size_t nblk = (raw + 65534) / 65535, zl = 2 + raw + 5 * nblk + 4;
    uint8_t *z = malloc(zl), *png = malloc(8 + 25 + 12 + zl + 12);
    if (!z || !png) { free(rawb), free(z), free(png); return false; }
    size_t zo = 0;
    z[zo++] = 0x78, z[zo++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw; off += 65535) {
        size_t n = raw - off < 65535 ? raw - off : 65535;
        z[zo++] = off + n >= raw, z[zo++] = (uint8_t)n, z[zo++] = (uint8_t)(n >> 8), z[zo++] = (uint8_t)~n, z[zo++] = (uint8_t)(~n >> 8);
        memcpy(z + zo, rawb + off, n), zo += n;
        for (size_t i = 0; i < n; i++) a = (a + rawb[off + i]) % 65521u, b = (b + a) % 65521u;
    }
    be32(z + zo, b << 16 | a), zo += 4;
    size_t po = 0;
    memcpy(png, "\x89PNG\r\n\x1a\n", 8), po = 8;
    uint8_t ihdr[13]; be32(ihdr, w), be32(ihdr + 4, h), ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    po += png_chunk(png + po, "IHDR", ihdr, 13);
    po += png_chunk(png + po, "IDAT", z, (uint32_t)zo);
    po += png_chunk(png + po, "IEND", NULL, 0);
    uint64_t hh = lagfx_a64_hash64(png, po);
    snprintf(name, cap, "cap_%016llx.png", (unsigned long long)hh);
    bool existed, ok = C.total + po <= max_total() && write_file(name, png, po, &existed);
    if (ok && !existed) C.total += po, p->diag2.exec.pipe.cap_bytes += po;
    free(rawb), free(z), free(png);
    return ok;
}

/* ---------------------------------------------------------------- recording ---- */

static void on_finish(void *ctx, bool done);

lagfx_a64_cap_t *lagfx_a64_cap_begin(lagfx_protocol_t *p, lagfx_a64_rexec_t *x, uint32_t task, uint32_t exec_seq) {
    if (!p || !x || !lagfx_a64_capture_enabled()) return NULL;
    if (lagfx_a64_dump_dir_fd() < 0 || lagfx_a64_dump_index_fd() < 0) return NULL;   /* no directory: nothing to write to */
    lagfx_a64_cap_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->p = p, c->x = x, c->task = task, c->exec = exec_seq ? exec_seq : ++C.execs;
    lagfx_a64_res_set_finish_hook(x, on_finish, c);
    return c;
}

bool lagfx_a64_cap_pass_wanted(lagfx_a64_cap_t *c) {
    if (!c) return false;
    for (uint32_t i = 0; i < c->nsnaps; i++) if (c->snaps[i].pass == UINT32_MAX) c->snaps[i].pass = UINT32_MAX - 1u;   /* pre snapshots of an earlier pass that never opened: orphans */
    uint64_t lo, hi;
    draw_range(&lo, &hi);
    if (C.next_n > hi || C.recorded >= max_draws()) return false;
    return true;
}

static int snap_find(lagfx_a64_cap_t *c, lagfx_a64_obj_t *o, int mode, uint32_t pass) {
    for (uint32_t i = 0; i < c->nsnaps; i++)
        if (c->snaps[i].o == o && c->snaps[i].gen == o->gen && c->snaps[i].mode == mode && c->snaps[i].pass == pass) return (int)i;
    return -1;
}
static int snap_new(lagfx_a64_cap_t *c, lagfx_a64_obj_t *o, int mode, uint32_t pass) {
    GROW(c->snaps, c->nsnaps, c->capsnaps);
    csnap_t *s = &c->snaps[c->nsnaps];
    memset(s, 0, sizeof *s);
    s->o = o, s->gen = o->gen, s->ref = o->ref, s->task = o->task, s->mode = mode, s->pass = pass, s->minoff = UINT64_MAX;
    return (int)c->nsnaps++;
}

/* record the GPU copy now (outside a pass) if the per-exec memory allows */
static void snap_take(lagfx_a64_cap_t *c, csnap_t *s, uint64_t off) {
    uint64_t want = s->mode == M_BUF ? (s->o->length - off < max_obj() ? s->o->length - off : max_obj()) : 0;
    if (s->mode != M_BUF) {
        uint64_t full = s->mode == M_TEX_TIGHT ? (uint64_t)s->o->width * s->o->height * (s->o->img_type == VK_IMAGE_TYPE_3D ? s->o->depth : 1u) * s->o->bpp : s->o->length;
        want = full < max_obj() ? full : max_obj();
    }
    s->done = true;
    if (!want || c->mem + want > max_exec()) { c->p->diag2.exec.pipe.cap_skipped_cap++; return; }
    bool ok = s->mode == M_BUF ? lagfx_a64_res_snap_buffer(c->x, s->o, off, max_obj(), &s->s)
                               : lagfx_a64_res_snap_texture(c->x, s->o, s->mode == M_TEX_TIGHT, max_obj(), &s->s);
    if (!ok) { c->p->diag2.exec.pipe.cap_failed++; return; }
    c->mem += s->s.bytes;
    s->ok = true;
    c->p->diag2.exec.pipe.cap_snaps++;
}

void lagfx_a64_cap_pre(lagfx_a64_cap_t *c, lagfx_a64_obj_t *o) {
    if (!c || !o || o->kind != LAGFX_A64_TEXTURE || o->ds_aspects) return;
    if (snap_find(c, o, M_TEX_TIGHT, UINT32_MAX) >= 0) return;
    int i = snap_new(c, o, M_TEX_TIGHT, UINT32_MAX);
    if (i >= 0) snap_take(c, &c->snaps[i], 0);
}

void lagfx_a64_cap_pass_open(lagfx_a64_cap_t *c, const lagfx_a64_cap_pass_in_t *pi, const lagfx_a64_cap_att_in_t *atts, uint32_t natt) {
    if (!c || !pi) return;
    if (c->npasses == c->cappasses) {
        uint32_t nc = c->cappasses ? c->cappasses * 2u : 8u;
        void *t = realloc(c->passes, nc * sizeof *c->passes);
        if (!t) return;
        c->passes = t, c->cappasses = nc;
    }
    cpass_t *q = &c->passes[c->npasses];
    memset(q, 0, sizeof *q);
    q->seq = ++c->seq, q->in = *pi, q->first_draw = c->ndraws;
    for (uint32_t i = 0; i < natt && i < 4u; i++) {
        q->att[i].in = atts[i], q->att[i].post = -1;
        int k = snap_find(c, atts[i].o, M_TEX_TIGHT, UINT32_MAX);
        q->att[i].pre = k;
        if (k >= 0) c->snaps[k].pass = c->npasses;   /* owned by this pass now (a later pass snapshots again) */
    }
    q->natt = natt < 4u ? natt : 4u;
    c->npasses++;
    c->pass_open = true;
    c->p->diag2.exec.pipe.cap_passes++;
}

uint32_t lagfx_a64_cap_draw_enter(lagfx_a64_cap_t *c, uint32_t pipe_ref, bool *selected) {
    uint32_t n = C.next_n++;
    uint64_t lo, hi;
    draw_range(&lo, &hi);
    bool sel = c && c->pass_open && n >= lo && n <= hi && pipe_filter_ok(pipe_ref) && C.recorded < max_draws();
    if (c && !sel) c->p->diag2.exec.pipe.cap_skipped_filter++;
    *selected = sel;
    return n;
}

static int spv_get(lagfx_a64_cap_t *c, const uint32_t *w, size_t bytes) {
    if (!w || !bytes) return -1;
    uint64_t h = lagfx_a64_hash64(w, bytes);
    for (uint32_t i = 0; i < c->nspv; i++) if (c->spv[i].hash == h && c->spv[i].bytes == bytes) return (int)i;
    GROW(c->spv, c->nspv, c->capspv);
    uint32_t *cp = malloc(bytes);
    if (!cp) return -1;
    memcpy(cp, w, bytes);
    c->spv[c->nspv] = (cspv_t){ h, cp, bytes };
    return (int)c->nspv++;
}

void lagfx_a64_cap_draw(lagfx_a64_cap_t *c, uint32_t n, const char *skip, const lagfx_a64_cap_draw_in_t *d) {
    if (!c || !d || !c->pass_open) return;
    int fm = fail_mode();
    if ((skip && fm == 0) || (!skip && fm == 2)) { c->p->diag2.exec.pipe.cap_skipped_filter++; return; }
    if (C.recorded >= max_draws()) { c->p->diag2.exec.pipe.cap_skipped_cap++; return; }
    if (c->ndraws == c->capdraws) {
        uint32_t nc = c->capdraws ? c->capdraws * 2u : 16u;
        void *t = realloc(c->draws, nc * sizeof *c->draws);
        if (!t) return;
        c->draws = t, c->capdraws = nc;
    }
    cdraw_t *q = &c->draws[c->ndraws];
    memset(q, 0, sizeof *q);
    q->pass = c->npasses - 1u, q->n = n, q->seq = ++c->seq, q->in = *d;
    snprintf(q->skip, sizeof q->skip, "%s", skip ? skip : "-");
    if (skip) q->in.nbind = 0;   /* skipped draw: no data (its bindings may be unresolved) */
    q->vs_spv = spv_get(c, d->vs, d->vs_bytes), q->fs_spv = spv_get(c, d->fs, d->fs_bytes);
    q->vb_snap = q->ib_snap = -1;
    for (uint32_t i = 0; i < LAGFX_A64_MAX_BINDINGS; i++) q->bind_snap[i] = -1;
    c->passes[c->npasses - 1u].ndraw++;
    c->ndraws++;
    C.recorded++;
    c->p->diag2.exec.pipe.cap_draws++;
}

/* register use of an object by a draw of pass `pi`: returns the csnap index */
static int use_obj(lagfx_a64_cap_t *c, lagfx_a64_obj_t *o, int mode, uint32_t pi, uint64_t off) {
    if (!o) return -1;
    int k = snap_find(c, o, mode, pi);
    if (k < 0) k = snap_new(c, o, mode, pi);
    if (k < 0) return -1;
    c->snaps[k].used = true;
    if (off < c->snaps[k].minoff) c->snaps[k].minoff = off;
    return k;
}

void lagfx_a64_cap_pass_close(lagfx_a64_cap_t *c) {
    if (!c || !c->pass_open) return;
    c->pass_open = false;
    uint32_t pi = c->npasses - 1u;
    cpass_t *q = &c->passes[pi];
    if (!q->ndraw) return;
    /* what the pass's draws bind, grouped per object */
    for (uint32_t di = q->first_draw; di < c->ndraws; di++) {
        cdraw_t *d = &c->draws[di];
        if (d->pass != pi) continue;
        d->vb_snap = use_obj(c, d->in.vb, M_BUF, pi, d->in.vb_off);
        d->ib_snap = use_obj(c, d->in.ib, M_BUF, pi, d->in.index_off);
        for (uint32_t b = 0; b < d->in.nbind; b++) {
            const lagfx_a64_cap_bind_in_t *bi = &d->in.binds[b];
            if (!bi->obj) continue;
            if (bi->kind == LAGFX_A64_BIND_BUFFER) d->bind_snap[b] = use_obj(c, bi->obj, M_BUF, pi, bi->off);
            else if (bi->kind == LAGFX_A64_BIND_IMAGE) d->bind_snap[b] = use_obj(c, bi->obj, bi->obj->has_pages ? M_TEX_MIRROR : M_TEX_TIGHT, pi, 0);
        }
    }
    for (uint32_t k = 0; k < c->nsnaps; k++) {
        csnap_t *s = &c->snaps[k];
        if (s->pass != pi || !s->used || s->done) continue;
        uint64_t off = s->mode == M_BUF ? (s->o->length <= max_obj() ? 0 : (s->minoff < s->o->length ? s->minoff & ~15ull : 0)) : 0;
        snap_take(c, s, off);
    }
    for (uint32_t i = 0; i < q->natt; i++) {   /* post: tight copy of every attachment of the pass */
        int k = snap_new(c, q->att[i].in.o, M_TEX_TIGHT, pi);
        if (k < 0) continue;
        snap_take(c, &c->snaps[k], 0);
        q->att[i].post = k;
    }
}

/* ---------------------------------------------------------------- writing (fence signalled) ---- */

static void floats(char *dst, size_t cap, const float *v, unsigned n) {
    size_t o = 0;
    for (unsigned i = 0; i < n && o + 20 < cap; i++) o += (size_t)snprintf(dst + o, cap - o, "%s%.9g", i ? "," : "", (double)v[i]);
    if (!n && cap) dst[0] = 0;
}

static uint32_t emit_img(lagfx_a64_cap_t *c, csnap_t *s, const char *use, uint32_t pass_id) {
    if (s->out_id) return s->out_id;
    if (!s->ok) return 0;
    const lagfx_a64_obj_t *o = s->o;
    uint32_t blob = put_blob(c->p, s->s.data, s->s.bytes, s->s.full);
    uint32_t id = ++C.next_img;
    char mt[LAGFX_A64_MAX_MIPS * 48 + 8];
    mt[0] = 0;
    bool mirror = s->mode == M_TEX_MIRROR;
    uint32_t nm = o->mips < o->tex_mip_count ? o->mips : o->tex_mip_count;
    if (mirror) {
        size_t po = 0;
        for (uint32_t l = 0; l < nm && l < LAGFX_A64_MAX_MIPS && po + 48 < sizeof mt; l++)
            po += (size_t)snprintf(mt + po, sizeof mt - po, "%s%llu:%llu:%u:%u:%u", l ? "," : "", (unsigned long long)o->mip[l].off, (unsigned long long)o->mip[l].row, o->mip[l].w, o->mip[l].h, o->mip[l].d);
    }
    bool is3d = o->img_type == VK_IMAGE_TYPE_3D;
    iline("cap_img id=%u task=%u ref=%u gen=%llu vkfmt=%u mtlfmt=%u w=%u h=%u d=%u mips=%u layers=%u bpp=%u layout=%s row=%llu tex_base=%llu tex_stride=%llu block_h=%u miptab=%s blob=%u len=%llu pass=%u use=%s",
          id, s->task, s->ref, (unsigned long long)s->gen, (unsigned)o->format, o->mtl_format, o->width, o->height, mirror || is3d ? o->depth : 1u, mirror ? nm : 1u, mirror ? (is3d ? 1u : o->layers) : 1u, o->bpp,
          mirror ? "mirror" : "tight", (unsigned long long)(mirror && o->tex_mip_count ? o->mip[0].row : (uint64_t)o->width * o->bpp),
          (unsigned long long)(mirror ? o->tex_base : 0), (unsigned long long)(mirror ? o->tex_stride : 0), mirror ? o->tex_block_h : 0u, mirror ? mt : "-", blob, (unsigned long long)s->s.bytes, pass_id, use);
    s->out_id = id;
    return id;
}

static uint32_t emit_buf(lagfx_a64_cap_t *c, csnap_t *s, uint32_t pass_id) {
    if (s->out_id) return s->out_id;
    if (!s->ok) return 0;
    uint32_t blob = put_blob(c->p, s->s.data, s->s.bytes, s->s.full);
    uint32_t id = ++C.next_buf;
    iline("cap_buf id=%u task=%u ref=%u gen=%llu length=%llu off=%llu len=%llu blob=%u pass=%u", id, s->task, s->ref, (unsigned long long)s->gen, (unsigned long long)s->o->length,
          (unsigned long long)s->s.off, (unsigned long long)s->s.bytes, blob, pass_id);
    s->out_id = id;
    return id;
}

static uint32_t emit_samp(lagfx_a64_cap_t *c, const lagfx_a64_cap_samp_in_t *a) {
    for (uint32_t i = 0; i < c->nsamp; i++)
        if (c->samp[i].ref == a->ref && c->samp[i].gen == a->gen && c->samp[i].lo == a->lod_lo && c->samp[i].hi == a->lod_hi) return c->samp[i].id;
    uint32_t id = ++C.next_samp;
    if (c->nsamp < 64) c->samp[c->nsamp].ref = a->ref, c->samp[c->nsamp].gen = a->gen, c->samp[c->nsamp].lo = a->lod_lo, c->samp[c->nsamp].hi = a->lod_hi, c->samp[c->nsamp].id = id, c->nsamp++;
    const VkSamplerCreateInfo *ci = &a->ci;
    iline("cap_samp id=%u ref=%u gen=%llu min=%u mag=%u mip=%u mip_enabled=%d addr_u=%u addr_v=%u addr_w=%u lod_min=%.9g lod_max=%.9g lod_bias=%.9g aniso=%d compare_enable=%d compare_op=%u border=%u unnorm=%d",
          id, a->ref, (unsigned long long)a->gen, (unsigned)ci->minFilter, (unsigned)ci->magFilter, (unsigned)ci->mipmapMode, a->mip, (unsigned)ci->addressModeU, (unsigned)ci->addressModeV, (unsigned)ci->addressModeW,
          (double)a->lod_lo, (double)a->lod_hi, (double)ci->mipLodBias, ci->anisotropyEnable ? (int)ci->maxAnisotropy : 0, (int)ci->compareEnable, (unsigned)ci->compareOp, (unsigned)ci->borderColor, (int)ci->unnormalizedCoordinates);
    return id;
}

static const char *kind_name(uint32_t k) {
    switch (k) { case LAGFX_A64_BIND_IMAGE: return "image"; case LAGFX_A64_BIND_SAMPLER: return "sampler"; case LAGFX_A64_BIND_BUFFER: return "buffer";
                 case LAGFX_A64_BIND_INPUT: return "input"; case LAGFX_A64_BIND_STORAGE_IMAGE: return "storage_image"; default: return "unknown"; }
}

static void write_all(lagfx_a64_cap_t *c) {
    if (!C.header) {
        C.header = true;
        C.next_blob = C.next_img = C.next_buf = C.next_samp = C.next_pass = 0, C.nblobs = 0;
        iline("cap_header version=%d byte_order=le", LAGFX_CAP_VERSION);
    }
    bool png = getenv("LAGFX_ARM64_DUMP_DATA_PNG") && getenv("LAGFX_ARM64_DUMP_DATA_PNG")[0] == '1';
    for (uint32_t pi = 0; pi < c->npasses; pi++) {
        cpass_t *q = &c->passes[pi];
        if (!q->ndraw) continue;
        q->id = ++C.next_pass;
        iline("cap_pass id=%u seq=%u exec=%u task=%u w=%u h=%u ncolor=%u depth=%d depth_ref=%u depth_vkfmt=%u depth_load=%u depth_clear=%.9g stencil=%d stencil_load=%u stencil_clear=%u",
              q->id, q->seq, c->exec, c->task, q->in.w, q->in.h, q->natt, q->in.depth, q->in.depth_ref, q->in.depth_vkfmt, q->in.depth_load, (double)q->in.depth_clear, q->in.stencil, q->in.stencil_load, q->in.stencil_clear);
        for (uint32_t i = 0; i < q->natt; i++) {
            catt_t *a = &q->att[i];
            const lagfx_a64_obj_t *o = a->in.o;
            uint32_t pre = a->pre >= 0 ? emit_img(c, &c->snaps[a->pre], "pre", q->id) : 0;
            uint32_t post = a->post >= 0 ? emit_img(c, &c->snaps[a->post], "post", q->id) : 0;
            char pn[96], cl[160];
            snprintf(pn, sizeof pn, "-");
            if (png && a->post >= 0 && c->snaps[a->post].ok) { char t[96]; if (write_png(c->p, o->format, o->width, o->height, c->snaps[a->post].s.data, c->snaps[a->post].s.bytes, t, sizeof t)) snprintf(pn, sizeof pn, "%s", t); }
            floats(cl, sizeof cl, a->in.clear, 4);
            iline("cap_att pass=%u slot=%u ref=%u gen=%llu vkfmt=%u mtlfmt=%u w=%u h=%u bpp=%u load=%u clear=%s pre=%u post=%u post_png=%s", q->id, a->in.slot, o->ref, (unsigned long long)o->gen, (unsigned)o->format,
                  o->mtl_format, o->width, o->height, o->bpp, a->in.load, cl, pre, post, pn);
        }
        for (uint32_t di = q->first_draw; di < c->ndraws; di++) {
            cdraw_t *d = &c->draws[di];
            if (d->pass != pi) continue;
            const lagfx_a64_cap_draw_in_t *in = &d->in;
            uint32_t vb = d->vb_snap >= 0 ? emit_buf(c, &c->snaps[d->vb_snap], q->id) : 0, ib = d->ib_snap >= 0 ? emit_buf(c, &c->snaps[d->ib_snap], q->id) : 0;
            uint32_t bid[LAGFX_A64_MAX_BINDINGS], sid[LAGFX_A64_MAX_BINDINGS];
            for (uint32_t b = 0; b < in->nbind; b++) {
                bid[b] = sid[b] = 0;
                if (d->bind_snap[b] >= 0) { csnap_t *s = &c->snaps[d->bind_snap[b]]; bid[b] = s->mode == M_BUF ? emit_buf(c, s, q->id) : emit_img(c, s, "tex", q->id); }
                if (in->binds[b].has_samp) sid[b] = emit_samp(c, &in->binds[b].samp);
            }
            uint32_t vs = d->vs_spv >= 0 ? put_blob(c->p, c->spv[d->vs_spv].w, c->spv[d->vs_spv].bytes, c->spv[d->vs_spv].bytes) : 0;
            uint32_t fs = d->fs_spv >= 0 ? put_blob(c->p, c->spv[d->fs_spv].w, c->spv[d->fs_spv].bytes, c->spv[d->fs_spv].bytes) : 0;
            char vi[160], at[160], vp[200], bias[100], cf[80];
            size_t o1 = 0, o2 = 0;
            vi[0] = at[0] = 0;
            for (uint32_t i = 0; i < in->n_vtx_in; i++) o1 += (size_t)snprintf(vi + o1, sizeof vi - o1, "%s%u:%u", i ? "," : "", in->vtx_in_loc[i], in->vtx_in_comp[i]);
            for (uint32_t i = 0; i < in->n_attr; i++) o2 += (size_t)snprintf(at + o2, sizeof at - o2, "%s%u:%u", i ? "," : "", in->attr_fmt[i], in->attr_off[i]);
            floats(vp, sizeof vp, in->vp, 6), floats(bias, sizeof bias, in->bias, 3);
            snprintf(cf, sizeof cf, "%u,%u,%u,%u", in->color_fmts[0], in->color_fmts[1], in->color_fmts[2], in->color_fmts[3]);
            iline("cap_draw n=%u seq=%u skip=%s pass=%u task=%u tag=%u prim=%u indexed=%u count=%llu instances=%llu first_vertex=%llu base_vertex=%lld base_instance=%llu index_start=%u index_type=%u index_off=%llu index_buf=%u "
                  "pipe_ref=%u pipe_gen=%llu pipe_hash=%016llx vs_spv=%u fs_spv=%u vtx_buf_slot=%u vtx_buf=%u vtx_off=%llu vtx_stride=%u vtx_in=%s vtx_attrs=%s vp=%s sc=%u,%u,%u,%u cull=%u winding=%u bias=%s "
                  "blend_set=%u blend=%u blend_rgb=%u,%u,%u blend_a=%u,%u,%u write_mask=%u ncolor_fmts=%u color_fmts=%s depth_test=%u depth_write=%u depth_cmp=%u stencil_test=%u sref=%u,%u nbind=%u",
                  d->n, d->seq, d->skip, q->id, c->task, in->tag, in->prim, in->indexed, (unsigned long long)in->count, (unsigned long long)in->instances, (unsigned long long)in->first_vertex, (long long)in->base_vertex,
                  (unsigned long long)in->base_instance, in->index_start, in->index_type, (unsigned long long)in->index_off, ib, in->pipe_ref, (unsigned long long)in->pipe_gen, (unsigned long long)in->pipe_hash, vs, fs,
                  in->vtx_slot, vb, (unsigned long long)in->vb_off, in->vtx_stride, vi, at, vp, in->sc[0], in->sc[1], in->sc[2], in->sc[3], in->cull, in->winding, bias, in->blend_set, in->blend, in->blend_rgb[0], in->blend_rgb[1],
                  in->blend_rgb[2], in->blend_a[0], in->blend_a[1], in->blend_a[2], in->write_mask, in->ncolor_fmts, cf, in->depth_test, in->depth_write, in->depth_cmp, in->stencil_test, in->sref[0], in->sref[1], in->nbind);
            for (uint32_t b = 0; b < in->nbind; b++) {
                const lagfx_a64_cap_bind_in_t *bi = &in->binds[b];
                bool buf = bi->kind == LAGFX_A64_BIND_BUFFER;
                iline("cap_bind draw=%u binding=%u kind=%s stage=%s idx=%u writable=%u buf=%u off=%llu range=%llu img=%u samp=%u", d->n, bi->binding, kind_name(bi->kind), bi->stage ? "fs" : "vs", bi->idx, bi->writable,
                      buf ? bid[b] : 0, (unsigned long long)bi->off, (unsigned long long)bi->range, bi->kind == LAGFX_A64_BIND_IMAGE ? bid[b] : 0, sid[b]);
            }
        }
    }
}

static void on_finish(void *ctx, bool done) {
    lagfx_a64_cap_t *c = ctx;
    if (done) write_all(c);
    else c->p->diag2.exec.pipe.cap_failed++;
    cap_free(c);
}

#endif /* LAGFX_HAVE_VULKAN */
