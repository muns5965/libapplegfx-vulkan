/* SPDX-License-Identifier: MIT */
/* spv_msfetch.c (multisampled framebuffer-fetch SPIR-V rewrite): modules with 1 / 2 / 16 / 32 / 64 reads are built here word by word (no tool needed to BUILD them), rewritten,
 * and the result is checked for (a) the EXACT output size (the buffer is sized from what is inserted: a regression of the old "3 words per read" allocation shows as a size mismatch
 * and, under AddressSanitizer, as a heap-buffer-overflow), (b) the structure (every read carries a Sample operand loaded from ONE SampleId Input variable, nothing declared twice,
 * only the Fragment entry point gets the variable) and (c) spirv-val. A missing spirv-val is a SKIP of the whole test (exit 77), never a pass. */
#include "air2spv/spv_reflect.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

typedef struct { uint32_t *w; size_t n, cap; } buf_t;
static void put(buf_t *b, uint32_t x) { if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->w = realloc(b->w, b->cap * 4); } b->w[b->n++] = x; }
static void ins(buf_t *b, uint32_t op, size_t nops, const uint32_t *ops) { put(b, ((uint32_t)(nops + 1) << 16) | op); for (size_t i = 0; i < nops; i++) put(b, ops[i]); }
#define INS(b, op, ...) do { const uint32_t o_[] = { __VA_ARGS__ }; ins(b, op, sizeof o_ / 4, o_); } while (0)
static uint32_t str_word(const char *s, size_t k) { uint32_t x = 0; for (int j = 0; j < 4; j++) { const unsigned char c = (unsigned char)s[k * 4 + j]; x |= (uint32_t)c << (8 * j); if (!c) break; } return x; }

enum { V_FRESH, V_NOINT, V_EXISTING, V_VERTEX };   /* what the module already declares */

/* a fragment shader reading input attachment 0 `n` times (and writing the last value), SPIR-V 1.3 */
static void build(buf_t *b, int variant, unsigned n) {
    const int noint = variant == V_NOINT, existing = variant == V_EXISTING, vertex = variant == V_VERTEX;
    enum { VOID = 1, FN, FLT, V4, INT, V2, PTR_OUT, IMG, PTR_IMG, VAR_IMG, VAR_OUT, C0, COORD, MAIN, LBL, PTR_SID, VAR_SID, VMAIN, VLBL, FIRST_FREE };
    const uint32_t bound = FIRST_FREE + 2 * n + 2;
    put(b, 0x07230203u); put(b, 0x00010300u); put(b, 0); put(b, bound); put(b, 0);
    INS(b, 17, 1u /* Shader */); INS(b, 17, 40u /* InputAttachment */);
    if (noint) INS(b, 17, 22u /* Int16 */);
    if (existing) INS(b, 17, 35u /* SampleRateShading */);
    INS(b, 14, 0u, 1u);   /* MemoryModel Logical GLSL450 */
    { uint32_t o[16]; size_t k = 0; o[k++] = 4u; o[k++] = MAIN; o[k++] = str_word("main\0\0\0\0", 0); o[k++] = 0; o[k++] = VAR_OUT; if (existing) o[k++] = VAR_SID; ins(b, 15, k, o); }   /* "main" = 2 words incl. terminator */
    if (vertex) { uint32_t o[8] = { 0u, VMAIN, str_word("vmain\0\0\0", 0), str_word("vmain\0\0\0", 1) }; ins(b, 15, 4, o); }
    INS(b, 16, MAIN, 7u);   /* OriginUpperLeft */
    INS(b, 71, VAR_OUT, 30u, 0u);          /* Location 0 */
    INS(b, 71, VAR_IMG, 34u, 0u);          /* DescriptorSet 0 */
    INS(b, 71, VAR_IMG, 33u, 0u);          /* Binding 0 */
    INS(b, 71, VAR_IMG, 43u, 0u);          /* InputAttachmentIndex 0 */
    if (existing) { INS(b, 71, VAR_SID, 11u, 18u); INS(b, 71, VAR_SID, 14u); }
    INS(b, 19, VOID);
    INS(b, 33, FN, VOID);
    INS(b, 22, FLT, 32u);
    INS(b, 23, V4, FLT, 4u);
    if (noint) INS(b, 21, INT, 16u, 1u); else INS(b, 21, INT, 32u, 1u);
    INS(b, 23, V2, INT, 2u);
    INS(b, 32, PTR_OUT, 3u, V4);
    INS(b, 25, IMG, FLT, 6u /* SubpassData */, 0u, 0u, 0u, 2u, 0u);
    INS(b, 32, PTR_IMG, 0u /* UniformConstant */, IMG);
    INS(b, 59, PTR_IMG, VAR_IMG, 0u);
    INS(b, 59, PTR_OUT, VAR_OUT, 3u);
    INS(b, 43, INT, C0, 0u);
    INS(b, 44, V2, COORD, C0, C0);
    if (existing) { INS(b, 32, PTR_SID, 1u, INT); INS(b, 59, PTR_SID, VAR_SID, 1u); }
    INS(b, 54, VOID, MAIN, 0u, FN); INS(b, 248, LBL);
    uint32_t id = FIRST_FREE, last = 0;
    for (unsigned k = 0; k < n; k++) { const uint32_t l = id++, r = id++; INS(b, 61, IMG, l, VAR_IMG); INS(b, 98, V4, r, l, COORD); last = r; }
    INS(b, 62, VAR_OUT, last);
    INS(b, 253); INS(b, 56);
    if (vertex) { INS(b, 54, VOID, VMAIN, 0u, FN); INS(b, 248, VLBL); INS(b, 253); INS(b, 56); }
}

static int have_val(void) { return system("spirv-val --version >/dev/null 2>&1") == 0; }
static int val(const uint8_t *spv, size_t sz, const char *what) {
    char path[64] = "/tmp/lagfx-msfu-XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0, "mkstemp");
    FILE *f = fdopen(fd, "wb"); fwrite(spv, 1, sz, f); fclose(f);
    char cmd[200]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.1 %s 2>&1", path);
    FILE *p = popen(cmd, "r"); char line[256]; int any = 0; while (p && fgets(line, sizeof line, p)) { fputs(line, stderr); any = 1; }
    const int rc = p ? pclose(p) : -1; remove(path);
    CHECK(rc == 0 && !any, "%s: spirv-val rejects the rewritten module", what);
    return 0;
}

/* structure of the rewritten module: counts what the rewrite promises */
typedef struct { unsigned sample_reads, plain_reads, srs_caps, sid_vars, int_ptrs, loads_of_sid, frag_ep_iface_has_sid, vert_ep_wc, ms_images; uint32_t sid; } shape_t;
static void shape(const uint32_t *w, size_t nw, shape_t *s) {
    memset(s, 0, sizeof *s);
    for (size_t i = 5; i < nw;) { const uint32_t wc = w[i] >> 16; if ((w[i] & 0xFFFFu) == 71 && wc == 4 && w[i + 2] == 11u && w[i + 3] == 18u) s->sid = w[i + 1]; i += wc; }   /* the SampleId variable first (decorations come after the entry points) */
    for (size_t i = 5; i < nw;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (op == 17 && w[i + 1] == 35u) s->srs_caps++;
        if (op == 71 && wc == 4 && w[i + 2] == 11u && w[i + 3] == 18u) s->sid_vars++;
        if (op == 25 && w[i + 3] == 6u && w[i + 6] == 1u) s->ms_images++;
        if (op == 15 && w[i + 1] == 0u) s->vert_ep_wc = wc;
        if (op == 15 && w[i + 1] == 4u) for (size_t j = i + 5; j < i + wc; j++) if (w[j] == s->sid) s->frag_ep_iface_has_sid = 1;
        if (op == 98) { if (wc == 7 && w[i + 5] == 0x40u) s->sample_reads++; else s->plain_reads++; }
        if (op == 61 && wc == 4 && w[i + 3] == s->sid && s->sid) s->loads_of_sid++;
        i += wc;
    }
}

static int one(int variant, unsigned n, size_t want_extra, const char *name) {
    buf_t b = { 0 }; build(&b, variant, n);
    uint8_t *out = NULL; size_t osz = 0; uint32_t nr = 0;
    const int rc = lagfx_spv_msfetch_rewrite((const uint8_t *)b.w, b.n * 4u, &out, &osz, &nr);
    CHECK(rc == 1 && out && nr == n, "%s n=%u: rewrite rc=%d nreads=%u", name, n, rc, nr);
    CHECK(osz == b.n * 4u + want_extra * 4u, "%s n=%u: output %zu bytes, expected exactly %zu (input %zu + %zu words)", name, n, osz, b.n * 4u + want_extra * 4u, b.n * 4u, want_extra);
    shape_t s; shape((const uint32_t *)(const void *)out, osz / 4u, &s);
    CHECK(s.sample_reads == n && s.plain_reads == 0 && s.loads_of_sid == n, "%s n=%u: reads with Sample %u plain %u loads of SampleId %u", name, n, s.sample_reads, s.plain_reads, s.loads_of_sid);
    CHECK(s.srs_caps == 1 && s.sid_vars == 1 && s.ms_images == 1 && s.frag_ep_iface_has_sid, "%s n=%u: caps %u sid vars %u ms images %u iface %u (each exactly once)", name, n, s.srs_caps, s.sid_vars, s.ms_images, s.frag_ep_iface_has_sid);
    if (variant == V_VERTEX) { shape_t t; shape((const uint32_t *)(const void *)b.w, b.n, &t); CHECK(s.vert_ep_wc == t.vert_ep_wc && s.vert_ep_wc != 0, "vertex entry point must be untouched (wc %u -> %u)", t.vert_ep_wc, s.vert_ep_wc); }
    if (val(out, osz, name)) return 1;
    free(out); free(b.w);
    return 0;
}

int main(void) {
    if (!have_val()) { puts("SKIP: spirv-val not installed (the rewrite is only meaningful when its output is validated)"); return 77; }
    static const unsigned ns[] = { 1, 2, 16, 32, 64 };
    for (size_t k = 0; k < sizeof ns / sizeof *ns; k++) {
        const unsigned n = ns[k];
        if (one(V_FRESH, n, 18u + 6u * n, "fresh (int32 present; no pointer / variable / capability)")) return 1;     /* cap 2 + decor 7 + ptr 4 + var 4 + iface 1 + 6/read */
        if (one(V_NOINT, n, 22u + 6u * n, "no 32-bit int in the module (new int type too)")) return 1;                /* + int type 4 */
        if (one(V_EXISTING, n, 6u * n, "SampleId variable, Input pointer and capability already declared")) return 1;   /* nothing declared twice, nothing added to the interface */
        if (one(V_VERTEX, n, 18u + 6u * n, "vertex + fragment entry points")) return 1;                              /* the vertex entry point gets no interface word */
    }
    puts("PASS msfetch rewrite: 1/2/16/32/64 reads x 4 module shapes: exact output size, one Sample read each from one SampleId variable, nothing declared twice, vertex EP untouched, spirv-val clean");
    /* malformed input must never read or write out of bounds (ASan / UBSan builds enforce this) */
    {   buf_t b = { 0 }; build(&b, V_FRESH, 3);
        const uint32_t vals[] = { 0u, 1u, 2u, 5u, b.w[3] - 1u, b.w[3], b.w[3] + 1u, 0x00010000u, 0xFFFFFFF0u, 0xFFFFFFFFu };
        for (size_t k = 5; k < b.n; k++) for (size_t v = 0; v < sizeof vals / sizeof *vals; v++) {   /* every word set to each value in turn */
            uint32_t *c = malloc(b.n * 4u); memcpy(c, b.w, b.n * 4u); c[k] = vals[v];
            uint8_t *o = NULL; size_t osz = 0; uint32_t nr = 0; const int rc = lagfx_spv_msfetch_rewrite((const uint8_t *)c, b.n * 4u, &o, &osz, &nr);
            if (rc == 1) free(o);
            free(c);
        }
        free(b.w);
        puts("PASS malformed words (each word replaced by 0 / 1 / 2 / 5 / bound-1..+1 / 0x10000 / 0xFFFFFFF0 / 0xFFFFFFFF in turn): no out-of-bounds access"); }
    return 0;
}
