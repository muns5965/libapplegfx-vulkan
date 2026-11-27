/*
 * libapplegfx-vulkan — the descriptor binding allocator / table (unit, no Vulkan)
 * tests/binding-alloc-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Covers air2spv/binding_alloc.h: sequential allocation, sharing of one binding by compatible descriptors (byte-buffer views, the same type in both stages),
 * detection of every incompatibility (descriptor type, input-attachment index, image dimensionality), growth (200 descriptors; overflow is counted, never truncated),
 * the table key (changes with ANY binding / type / stage / role change), the reflected-module path (translated fixtures: the fragment fixture of the device defect has no
 * collision, the same fragment module WITHOUT the stage offset collides with the vertex module's buffer and is reported), the stage base, the report text.
 */
#include "air/bitcode_reader.h"
#include "air2spv/binding_alloc.h"
#include "air2spv/translate.h"
#include "libapplegfx-vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

#define SAMP LAGFX_SPV_BINDING_SAMPLER
#define IMG LAGFX_SPV_BINDING_SAMPLED_IMAGE
#define BUF LAGFX_SPV_BINDING_STORAGE_BUFFER
#define IA LAGFX_SPV_BINDING_INPUT_ATTACHMENT
#define F LAGFX_BA_STAGE_FRAGMENT
#define V LAGFX_BA_STAGE_VERTEX
#define NR LAGFX_SPV_NO_ROLE

static lagfx_ba_t *mk(void) { lagfx_ba_t *a = malloc(sizeof *a); lagfx_ba_init(a); return a; }

static void test_alloc_and_claims(void) {
    lagfx_ba_t *a = mk();
    uint32_t b = 99;
    for (uint32_t i = 0; i < 24; i++) { CHECK(lagfx_ba_alloc(a, 0, IMG, F, 1, NR, &b) == LAGFX_BA_OK && b == i); }          /* 24 textures: 0..23 */
    CHECK(lagfx_ba_alloc(a, 0, SAMP, F, 0, NR, &b) == LAGFX_BA_OK && b == 24);                                              /* the default sampler */
    CHECK(lagfx_ba_alloc(a, 0, SAMP, F, 0, NR, &b) == LAGFX_BA_OK && b == 25);                                              /* constexpr samplers */
    CHECK(lagfx_ba_alloc(a, 0, SAMP, F, 0, NR, &b) == LAGFX_BA_OK && b == 26);
    CHECK(lagfx_ba_alloc(a, 0, IA, F, 0, 0, &b) == LAGFX_BA_OK && b == 27);                                                 /* input attachments come AFTER, wherever that is (was: 24 + n) */
    CHECK(lagfx_ba_alloc(a, 0, IA, F, 0, 1, &b) == LAGFX_BA_OK && b == 28);
    CHECK(lagfx_ba_ok(a) && a->n == 29);
    /* the old fixed placement: an input attachment CLAIMED on binding 24 is refused (the default sampler holds it) */
    lagfx_ba_entry_t old_ia = { .set = 0, .binding = 24, .kind = IA, .stages = F, .role_index = 0 };
    CHECK(lagfx_ba_claim(a, &old_ia) == LAGFX_BA_CONFLICT && a->nconflict == 1 && a->conflict[0].reason == LAGFX_BA_REASON_KIND && a->conflict[0].binding == 24);
    CHECK(!lagfx_ba_ok(a));
    char txt[4096];
    lagfx_ba_describe(a, txt, sizeof txt);
    CHECK(strstr(txt, "CONFLICT set 0 binding 24: SAMPLER vs INPUT_ATTACHMENT"));
    CHECK((strstr(txt, "set 0 binding 27  INPUT_ATTACHMENT") && strstr(txt, "stages=F role=0")));
    free(a);

    /* sharing: byte-buffer views (same kind, same stage) and one type used by both stages are ONE entry */
    a = mk();
    lagfx_ba_entry_t e = { .set = 0, .binding = 3, .kind = BUF, .stages = V, .role_index = NR };
    CHECK(lagfx_ba_claim(a, &e) == LAGFX_BA_OK);
    CHECK(lagfx_ba_claim(a, &e) == LAGFX_BA_SHARED);                      /* a view of the same buffer */
    e.stages = F; e.may_write = 1;
    CHECK(lagfx_ba_claim(a, &e) == LAGFX_BA_SHARED && a->n == 1 && a->e[0].stages == (V | F) && a->e[0].may_write == 1);
    CHECK(lagfx_ba_ok(a));
    /* other sets are independent */
    e.set = 1;
    CHECK(lagfx_ba_claim(a, &e) == LAGFX_BA_OK && a->n == 2);
    /* the VS/FS case that must NOT be merged: different types */
    lagfx_ba_entry_t s = { .set = 0, .binding = 3, .kind = SAMP, .stages = F, .role_index = NR };
    CHECK(lagfx_ba_claim(a, &s) == LAGFX_BA_CONFLICT && !lagfx_ba_ok(a));
    free(a);

    /* input attachments of different index / sampled images of different dimensionality on one binding */
    a = mk();
    lagfx_ba_entry_t i0 = { .set = 0, .binding = 5, .kind = IA, .stages = F, .role_index = 0 }, i1 = i0;
    i1.role_index = 1;
    CHECK(lagfx_ba_claim(a, &i0) == LAGFX_BA_OK && lagfx_ba_claim(a, &i0) == LAGFX_BA_SHARED && lagfx_ba_claim(a, &i1) == LAGFX_BA_CONFLICT && a->conflict[0].reason == LAGFX_BA_REASON_ROLE);
    lagfx_ba_entry_t t2 = { .set = 0, .binding = 6, .kind = IMG, .stages = F, .image_dim = 1, .role_index = NR }, t3 = t2;
    t3.image_dim = 2;
    CHECK(lagfx_ba_claim(a, &t2) == LAGFX_BA_OK && lagfx_ba_claim(a, &t3) == LAGFX_BA_CONFLICT && a->conflict[1].reason == LAGFX_BA_REASON_DIM);
    free(a);
}

static void test_growth(void) {
    lagfx_ba_t *a = mk();
    uint32_t b = 0;
    for (uint32_t i = 0; i < 200; i++) CHECK(lagfx_ba_alloc(a, 0, (lagfx_spv_binding_kind_t)(1 + i % 3), i & 1 ? F : V, 0, NR, &b) == LAGFX_BA_OK && b == i);
    CHECK(a->n == 200 && lagfx_ba_ok(a));
    for (uint32_t i = 200; i < LAGFX_BA_MAX; i++) CHECK(lagfx_ba_alloc(a, 0, BUF, F, 0, NR, &b) == LAGFX_BA_OK);
    CHECK(lagfx_ba_ok(a) && a->n == LAGFX_BA_MAX);
    CHECK(lagfx_ba_alloc(a, 0, BUF, F, 0, NR, &b) == LAGFX_BA_FULL && a->noverflow == 1 && !lagfx_ba_ok(a));   /* counted, not dropped silently */
    char txt[64 * 1024];
    size_t need = lagfx_ba_describe(a, txt, sizeof txt);
    CHECK(need < sizeof txt && strstr(txt, "OVERFLOW: 1 descriptor(s)"));
    free(a);
}

static void test_key(void) {
    lagfx_ba_t *a = mk(), *c = mk();
    uint32_t b;
    lagfx_ba_alloc(a, 0, IMG, F, 1, NR, &b); lagfx_ba_alloc(a, 0, SAMP, F, 0, NR, &b); lagfx_ba_alloc(a, 0, IA, F, 0, 0, &b);
    lagfx_ba_alloc(c, 0, IMG, F, 1, NR, &b); lagfx_ba_alloc(c, 0, SAMP, F, 0, NR, &b); lagfx_ba_alloc(c, 0, IA, F, 0, 0, &b);
    CHECK(lagfx_ba_key(a) == lagfx_ba_key(c));                     /* same table, same key */
    lagfx_ba_entry_t e = { .set = 0, .binding = 3, .kind = BUF, .stages = F, .role_index = NR };
    lagfx_ba_claim(c, &e);
    CHECK(lagfx_ba_key(a) != lagfx_ba_key(c));                     /* a binding appears */
    lagfx_ba_t *d = mk();
    lagfx_ba_alloc(d, 0, IMG, F, 1, NR, &b); lagfx_ba_alloc(d, 0, IA, F, 0, 0, &b); lagfx_ba_alloc(d, 0, SAMP, F, 0, NR, &b);   /* the same descriptors at other bindings */
    CHECK(lagfx_ba_key(a) != lagfx_ba_key(d));
    lagfx_ba_t *f = mk();
    lagfx_ba_alloc(f, 0, IMG, F, 1, NR, &b); lagfx_ba_alloc(f, 0, SAMP, F, 0, NR, &b); lagfx_ba_alloc(f, 0, IA, F, 0, 1, &b);   /* another input-attachment index */
    CHECK(lagfx_ba_key(a) != lagfx_ba_key(f));
    lagfx_ba_t *g = mk();
    lagfx_ba_alloc(g, 0, IMG, F | V, 1, NR, &b); lagfx_ba_alloc(g, 0, SAMP, F, 0, NR, &b); lagfx_ba_alloc(g, 0, IA, F, 0, 0, &b);   /* another stage set */
    CHECK(lagfx_ba_key(a) != lagfx_ba_key(g));
    lagfx_ba_t *h = mk();
    lagfx_ba_alloc(h, 0, IMG, F, 1, NR, &b); lagfx_ba_alloc(h, 0, SAMP, F, 0, NR, &b);
    lagfx_ba_entry_t bad = { .set = 0, .binding = 1, .kind = IA, .stages = F, .role_index = 0 };
    lagfx_ba_claim(h, &bad);                                       /* a refused claim changes the key too (an invalid table never equals a valid one) */
    lagfx_ba_t *k = mk();
    lagfx_ba_alloc(k, 0, IMG, F, 1, NR, &b); lagfx_ba_alloc(k, 0, SAMP, F, 0, NR, &b);
    CHECK(lagfx_ba_key(h) != lagfx_ba_key(k));
    /* the entry order of claims does not matter */
    lagfx_ba_t *m = mk();
    lagfx_ba_entry_t x0 = { .set = 0, .binding = 2, .kind = BUF, .stages = V, .role_index = NR }, x1 = { .set = 0, .binding = 0, .kind = SAMP, .stages = F, .role_index = NR };
    lagfx_ba_claim(m, &x0); lagfx_ba_claim(m, &x1);
    lagfx_ba_t *n = mk();
    lagfx_ba_claim(n, &x1); lagfx_ba_claim(n, &x0);
    CHECK(lagfx_ba_key(m) == lagfx_ba_key(n));
    free(a); free(c); free(d); free(f); free(g); free(h); free(k); free(m); free(n);
}

/* ---- reflected translator output ---- */
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}
static uint8_t *fixture(const char *name, size_t *n) {
    char path[512];
    const char *dirs[] = { SRCDIR "/fixtures/", "tests/fixtures/", "../tests/fixtures/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) { snprintf(path, sizeof path, "%s%s", dirs[d], name); uint8_t *b = slurp(path, n); if (b) return b; }
    return NULL;
}

static void test_modules(void) {
    size_t vn = 0, fn = 0, vsn = 0, fsn = 0;
    uint8_t *vbc = fixture("login_ubercomposite_vert.air.bc", &vn), *fbc = fixture("bind_many_fs.air.bc", &fn), *vs = NULL, *fs = NULL;
    lagfx_air_module_t *vm = NULL, *fm = NULL;
    lagfx_stage_keys_t k;
    memset(&k, 0, sizeof k);
    CHECK(vbc && fbc && lagfx_air_module_open(vbc, vn, &vm) == LAGFX_OK && lagfx_air_module_open(fbc, fn, &fm) == LAGFX_OK);
    if (g_fail) return;
    if (!lagfx_air_stage_keys(vm, true, &k)) memset(&k, 0, sizeof k);
    CHECK(lagfx_air2spv_translate_module_ex(vm, NULL, &vs, &vsn) == LAGFX_OK && lagfx_air2spv_translate_module_ex(fm, &k, &fs, &fsn) == LAGFX_OK);
    const uint8_t *bl[2] = { vs, fs };
    size_t ln[2] = { vsn, fsn };

    /* WITHOUT the stage offset the fragment module's texture 0 sits on the vertex module's buffer 0: the merge reports it */
    lagfx_ba_t *a = mk();
    CHECK(!lagfx_ba_merge_modules(a, bl, ln, 2) && a->nconflict >= 1);
    char txt[16384];
    lagfx_ba_describe(a, txt, sizeof txt);
    CHECK(strstr(txt, "CONFLICT set 0 binding 0: STORAGE_BUFFER vs SAMPLED_IMAGE") != NULL);
    free(a);

    /* the vertex module alone, and the fragment module alone: valid; the stage base of the vertex module clears its range */
    a = mk();
    CHECK(lagfx_ba_merge_modules(a, bl, ln, 1) && a->n == 1);                       /* the UberComposite vertex shader: one buffer (binding 0) */
    free(a);
    uint32_t base = lagfx_ba_stage_base(vs, vsn, 16);
    CHECK(base == 16);                                                                 /* binding 0 < 16: the historical base */
    CHECK(lagfx_ba_stage_base(vs, vsn, 0) == 1);                                       /* a vertex stage larger than the minimum moves the base behind itself */
    CHECK(lagfx_ba_stage_base(NULL, 0, 16) == 16);
    lagfx_spv_offset_bindings(fs, fsn, base);
    a = mk();
    CHECK(lagfx_ba_merge_modules(a, bl, ln, 2) && lagfx_ba_ok(a));
    uint32_t nia = 0, nsamp = 0, nimg = 0, lastres = 0, firstia = ~0u;
    for (uint32_t i = 0; i < a->n; i++) {
        const lagfx_ba_entry_t *e = &a->e[i];
        if (e->kind == IA) { nia++; CHECK(e->stages == F && e->role_index < 2); if (e->binding < firstia) firstia = e->binding; }
        else if (e->kind == SAMP) { nsamp++; if (e->binding > lastres) lastres = e->binding; }
        else if (e->kind == IMG) { nimg++; if (e->binding > lastres) lastres = e->binding; }
        if (e->stages == V) CHECK(e->binding < 16);
        if (e->stages == F) CHECK(e->binding >= 16);
    }
    CHECK(nia == 2 && nimg == 24 && nsamp == 3);
    CHECK(firstia > lastres);                                                          /* attachments sit behind every sampler / texture (they did not: 24 + n) */
    uint64_t key = lagfx_ba_key(a);
    free(a);
    /* another offset = another layout = another key */
    uint8_t *fs2 = malloc(fsn);
    memcpy(fs2, fs, fsn);
    lagfx_spv_offset_bindings(fs2, fsn, 1);
    const uint8_t *bl2[2] = { vs, fs2 };
    a = mk();
    lagfx_ba_merge_modules(a, bl2, ln, 2);
    CHECK(lagfx_ba_key(a) != key);
    free(a); free(fs2);
    free(vs); free(fs); lagfx_air_module_free(vm); lagfx_air_module_free(fm); free(vbc); free(fbc);
}

int main(void) {
    test_alloc_and_claims();
    test_growth();
    test_key();
    test_modules();
    if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    printf("binding alloc: ok\n");
    return 0;
}
