/*
 * libapplegfx-vulkan — pipeline-layout-level descriptor check (offline, no Vulkan device)
 * tests/pipeline-layout-check-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * spirv-val checks a module on its own. It cannot see the PIPELINE LAYOUT the module is built against, so it cannot see that one (set, binding) is declared as a
 * sampler by the module and as an input attachment by the layout (VUID-VkGraphicsPipelineCreateInfo-layout-07990). This test checks exactly that, with a SPIR-V
 * walker of its own (independent of lagfx_spv_reflect_bindings, so a reflection bug cannot hide the same bug in the check):
 *   - inside one module and between the stages of one pipeline: variables of DIFFERENT descriptor kinds on one (set, binding) (buffer views of one kind may share);
 *   - against a layout (the set-0 layout lagfx_layout_bindings_from_spv derives, or the `layout.binding` records of a pipefail bundle): the binding exists, its
 *     VkDescriptorType is one the variable's kind may use, descriptorCount covers an array, its stageFlags include the module's stage, the set is set 0.
 *
 * Modes:
 *   pipeline-layout-check-unit                    self test on hand-written modules (always runs; includes the negative tests: a clean module passes, every defect
 *                                                 class is found with the exact number of findings). Exit 0 / 1.
 *   pipeline-layout-check-unit <bundle|dir>...    check pipefail bundles (pipefail_<hash>/{state,vs.spv,fs.spv}); <dir> holds pipefail_* subdirectories. Prints every
 *                                                 finding; exit 1 when ANY bundle has one. (The device dump /tmp/vm2/dump/pipefail_18d04777d220e041 FAILS here:
 *                                                 constexpr sampler vs input attachment on binding 40.)
 *   LAGFX_PIPEFAIL_CHECK_DIR=<dir>                the same for the meson test (skip, exit 77, when unset).
 *
 * What a PASS means: the module and layout are consistent in descriptor kind / count / stage / set. It does NOT mean an Adreno driver builds the pipeline.
 */
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LAGFX_HAVE_VULKAN
int main(void) { return 77; }
#else
#include "vulkan/descriptor_layout.h"
#include <vulkan/vulkan.h>
#include <dirent.h>
#include <sys/stat.h>
#include "pipeline-layout-check-modules.h"

enum { K_NONE = 0, K_SAMPLER, K_SAMPLED_IMAGE, K_COMBINED, K_STORAGE_IMAGE, K_INPUT_ATT, K_STORAGE_BUFFER, K_UNIFORM_BUFFER, K_TEXEL_BUFFER };
static const char *kname(int k) {
    static const char *n[] = { "none", "sampler", "sampled-image", "combined-image-sampler", "storage-image", "input-attachment", "storage-buffer", "uniform-buffer", "texel-buffer" };
    return n[k];
}
typedef struct { uint32_t set, binding, count; int kind; } var_t;
#define MAXV 256
#define MAXF 64
typedef struct { char msg[MAXF][220]; int n; } findings_t;
static void fnd(findings_t *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fnd(findings_t *f, const char *fmt, ...) {
    if (f->n >= MAXF) return;
    __builtin_va_list ap; __builtin_va_start(ap, fmt); vsnprintf(f->msg[f->n++], sizeof f->msg[0], fmt, ap); __builtin_va_end(ap);
}

/* ---- minimal SPIR-V walker: descriptor variables only ---- */
typedef struct { uint16_t op; uint32_t a, b, c, d, e, f; } tinfo_t;   /* per result id: opcode + the operands the walk needs */
static int collect(const uint32_t *w, size_t nw, var_t *out, int cap, int *nout) {
    *nout = 0;
    if (nw < 5 || w[0] != 0x07230203u) return -1;
    uint32_t bound = w[3];
    if (bound == 0 || bound > (1u << 22)) return -1;
    tinfo_t *t = calloc(bound, sizeof *t);
    uint32_t *dset = calloc(bound, 4), *dbind = calloc(bound, 4), *hasset = calloc(bound, 4), *hasbind = calloc(bound, 4), *blk = calloc(bound, 4), *bblk = calloc(bound, 4);
    uint32_t *cval = calloc(bound, 4);
    int rc = 0;
    for (int pass = 0; pass < 2 && !rc; pass++) {
        for (size_t i = 5; i < nw;) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
            if (!wc || i + wc > nw) { rc = -1; break; }
            const uint32_t *p = w + i;
            if (pass == 0) {
                switch (op) {
                case 71: /* OpDecorate */ if (wc >= 3 && p[1] < bound) {
                        if (p[2] == 34) { dset[p[1]] = p[3]; hasset[p[1]] = 1; }
                        else if (p[2] == 33) { dbind[p[1]] = p[3]; hasbind[p[1]] = 1; }
                        else if (p[2] == 2) blk[p[1]] = 1;
                        else if (p[2] == 3) bblk[p[1]] = 1; } break;
                case 25: /* OpTypeImage: id, sampled type, dim, depth, arrayed, ms, sampled, format */
                    if (wc >= 9 && p[1] < bound) { t[p[1]].op = op; t[p[1]].a = p[3]; t[p[1]].b = p[7]; } break;
                case 26: case 27: /* OpTypeSampler, OpTypeSampledImage */ if (p[1] < bound) t[p[1]].op = op; break;
                case 28: case 29: /* OpTypeArray (id, elem, length id), OpTypeRuntimeArray */ if (p[1] < bound) { t[p[1]].op = op; t[p[1]].a = p[2]; t[p[1]].b = op == 28 ? p[3] : 0; } break;
                case 30: /* OpTypeStruct */ if (p[1] < bound) t[p[1]].op = op; break;
                case 32: /* OpTypePointer (id, storage class, type) */ if (wc >= 4 && p[1] < bound) { t[p[1]].op = op; t[p[1]].a = p[2]; t[p[1]].b = p[3]; } break;
                case 43: /* OpConstant (type, id, value) */ if (wc >= 4 && p[2] < bound) cval[p[2]] = p[3]; break;
                }
            } else if (op == 59 && wc >= 4) { /* OpVariable: type, id, storage class */
                uint32_t id = p[2], sc = p[3];
                if ((sc == 0 || sc == 2 || sc == 12) && id < bound && p[1] < bound && t[p[1]].op == 32) {
                    uint32_t ty = t[p[1]].b, count = 1;
                    while (ty < bound && (t[ty].op == 28 || t[ty].op == 29)) { count *= t[ty].op == 28 ? (t[ty].b < bound ? cval[t[ty].b] : 1) : 1; ty = t[ty].a; }
                    int kind = K_NONE;
                    if (ty < bound) switch (t[ty].op) {
                        case 26: kind = K_SAMPLER; break;
                        case 27: kind = K_COMBINED; break;
                        case 25: kind = t[ty].a == 6 ? K_INPUT_ATT : t[ty].a == 5 ? K_TEXEL_BUFFER : t[ty].b == 2 ? K_STORAGE_IMAGE : K_SAMPLED_IMAGE; break;
                        case 30: kind = sc == 12 ? K_STORAGE_BUFFER : bblk[ty] ? K_STORAGE_BUFFER : blk[ty] ? K_UNIFORM_BUFFER : K_NONE; break;
                    }
                    if (kind && hasset[id] && hasbind[id] && *nout < cap) out[(*nout)++] = (var_t){ dset[id], dbind[id], count, kind };
                }
            }
            i += wc;
        }
    }
    free(t); free(dset); free(dbind); free(hasset); free(hasbind); free(blk); free(bblk); free(cval);
    return rc;
}

/* ---- the check ---- */
typedef struct { uint32_t binding; uint32_t type; uint32_t count; uint32_t stages; } lay_t;
static int compatible(int kind, uint32_t vt) {
    switch (kind) {
    case K_SAMPLER:        return vt == VK_DESCRIPTOR_TYPE_SAMPLER || vt == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case K_SAMPLED_IMAGE:  return vt == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || vt == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case K_COMBINED:       return vt == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case K_STORAGE_IMAGE:  return vt == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case K_INPUT_ATT:      return vt == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    case K_STORAGE_BUFFER: return vt == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || vt == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    case K_UNIFORM_BUFFER: return vt == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || vt == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    case K_TEXEL_BUFFER:   return vt == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || vt == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    }
    return 0;
}
typedef struct { const uint32_t *w; size_t nw; uint32_t stage; const char *name; } stage_t;

/* Returns the number of findings appended. */
static int check_pipeline(const stage_t *st, int nst, const lay_t *lay, int nlay, findings_t *f) {
    int before = f->n;
    var_t v[8][MAXV]; int nv[8];
    for (int s = 0; s < nst && s < 8; s++) {
        if (collect(st[s].w, st[s].nw, v[s], MAXV, &nv[s]) != 0) { fnd(f, "%s: not a readable SPIR-V module", st[s].name); nv[s] = 0; continue; }
        /* (1) one module: different kinds on one (set, binding) */
        for (int i = 0; i < nv[s]; i++) for (int j = i + 1; j < nv[s]; j++)
            if (v[s][i].set == v[s][j].set && v[s][i].binding == v[s][j].binding && v[s][i].kind != v[s][j].kind)
                fnd(f, "%s: set %u binding %u is declared as %s AND as %s (one binding has one descriptor type)", st[s].name, v[s][i].set, v[s][i].binding, kname(v[s][i].kind), kname(v[s][j].kind));
    }
    /* (2) between the stages of the pipeline */
    for (int s = 0; s < nst && s < 8; s++) for (int s2 = s + 1; s2 < nst && s2 < 8; s2++)
        for (int i = 0; i < nv[s]; i++) for (int j = 0; j < nv[s2]; j++)
            if (v[s][i].set == v[s2][j].set && v[s][i].binding == v[s2][j].binding && v[s][i].kind != v[s2][j].kind)
                fnd(f, "%s vs %s: set %u binding %u is %s in one stage and %s in the other", st[s].name, st[s2].name, v[s][i].set, v[s][i].binding, kname(v[s][i].kind), kname(v[s2][j].kind));
    /* (3) against the layout */
    if (lay) for (int s = 0; s < nst && s < 8; s++) for (int i = 0; i < nv[s]; i++) {
        const var_t *x = &v[s][i];
        if (x->set != 0) { fnd(f, "%s: set %u binding %u: the layout has only set 0", st[s].name, x->set, x->binding); continue; }
        const lay_t *l = NULL;
        for (int k = 0; k < nlay; k++) if (lay[k].binding == x->binding) { l = &lay[k]; break; }
        if (!l) { fnd(f, "%s: set 0 binding %u (%s) is not in the layout", st[s].name, x->binding, kname(x->kind)); continue; }
        if (!compatible(x->kind, l->type)) fnd(f, "%s: set 0 binding %u is %s in the module but VkDescriptorType %u in the layout", st[s].name, x->binding, kname(x->kind), l->type);
        else if (x->count > l->count) fnd(f, "%s: set 0 binding %u: the module needs %u descriptors, the layout has %u", st[s].name, x->binding, x->count, l->count);
        if (!(l->stages & st[s].stage)) fnd(f, "%s: set 0 binding %u: layout stageFlags 0x%x do not include this stage (0x%x)", st[s].name, x->binding, l->stages, st[s].stage);
    }
    return f->n - before;
}

static int derived_layout(const stage_t *st, int nst, lay_t *out) {
    const uint8_t *b[8]; size_t l[8];
    for (int s = 0; s < nst; s++) { b[s] = (const uint8_t *)st[s].w; l[s] = st[s].nw * 4; }
    VkDescriptorSetLayoutBinding vb[32];
    uint32_t n = lagfx_layout_bindings_from_spv(b, l, (uint32_t)nst, vb);
    for (uint32_t i = 0; i < n; i++) out[i] = (lay_t){ vb[i].binding, (uint32_t)vb[i].descriptorType, vb[i].descriptorCount, (uint32_t)vb[i].stageFlags };
    return (int)n;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define ARR(m) m, sizeof m / 4
static void dump(const findings_t *f) { for (int i = 0; i < f->n; i++) printf("    finding: %s\n", f->msg[i]); }

static int selftest(void) {
    const uint32_t FS = VK_SHADER_STAGE_FRAGMENT_BIT, VS = VK_SHADER_STAGE_VERTEX_BIT, ALL = FS | VS;
    findings_t f;
    lay_t lay[40];
    /* 1. clean module: no findings against its own derived layout, nor against a hand-written one */
    { stage_t s[1] = { { ARR(k_lm_clean_fs), FS, "fs" } };
      memset(&f, 0, sizeof f);
      int n = derived_layout(s, 1, lay);
      CHECK(n == 4, "derived layout of the clean module has %d bindings", n);
      CHECK(check_pipeline(s, 1, lay, n, &f) == 0, "clean module vs derived layout: findings");
      lay_t hand[4] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, ALL }, { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, ALL }, { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, FS }, { 3, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, FS } };
      CHECK(check_pipeline(s, 1, hand, 4, &f) == 0, "clean module vs hand layout: findings");
      /* a COMBINED_IMAGE_SAMPLER may stand in for the sampled image / the sampler (legal in Vulkan) */
      hand[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; hand[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      CHECK(check_pipeline(s, 1, hand, 4, &f) == 0, "combined image sampler accepted");
      puts("PASS: clean module: no finding against the derived layout, a hand-written one, and combined-image-sampler substitutes"); }
    /* 2. the shape of pipefail_18d04777d220e041: sampler + input attachment on binding 40 -> one collision in the module; the DERIVED layout (first wins) cannot satisfy both */
    { stage_t s[1] = { { ARR(k_lm_collide_fs), FS, "fs" } };
      memset(&f, 0, sizeof f);
      int n = derived_layout(s, 1, lay);
      int c = check_pipeline(s, 1, lay, n, &f);
      dump(&f);
      CHECK(c == 2, "collision module vs derived layout: %d findings, expected 2 (the intra-module collision + the layout type mismatch on binding 40)", c);
      CHECK(strstr(f.msg[0], "binding 40") && strstr(f.msg[0], "sampler") && strstr(f.msg[0], "input-attachment"), "collision finding names binding 40 / sampler / input attachment: %s", f.msg[0]);
      CHECK(strstr(f.msg[1], "binding 40") && strstr(f.msg[1], "layout"), "layout finding names binding 40: %s", f.msg[1]);
      /* the same, with the layout the bundle recorded (40/41 = input attachments, 39 sampler) */
      lay_t rec[4] = { { 16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, FS | VS }, { 39, VK_DESCRIPTOR_TYPE_SAMPLER, 1, FS | VS }, { 40, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, FS }, { 41, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, FS } };
      memset(&f, 0, sizeof f);
      CHECK(check_pipeline(s, 1, rec, 4, &f) == 2, "collision module vs recorded-style layout: %d findings, expected 2", f.n);
      puts("PASS: sampler vs input attachment on one binding (the pipefail_18d04777d220e041 shape) is found, in the module and against the layout"); }
    /* 3. layout defects on a one-buffer module: type, stage, count, missing binding, set */
    { stage_t s[1] = { { ARR(k_lm_buf_fs), FS, "fs" } };
      lay_t ok = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, FS };
      memset(&f, 0, sizeof f); CHECK(check_pipeline(s, 1, &ok, 1, &f) == 0, "baseline");
      lay_t bad_type = ok; bad_type.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      memset(&f, 0, sizeof f); CHECK(check_pipeline(s, 1, &bad_type, 1, &f) == 1 && strstr(f.msg[0], "storage-buffer"), "type mismatch: %d", f.n);
      lay_t bad_stage = ok; bad_stage.stages = VS;
      memset(&f, 0, sizeof f); CHECK(check_pipeline(s, 1, &bad_stage, 1, &f) == 1 && strstr(f.msg[0], "stageFlags"), "stage mismatch: %d", f.n);
      lay_t other = ok; other.binding = 7;
      memset(&f, 0, sizeof f); CHECK(check_pipeline(s, 1, &other, 1, &f) == 1 && strstr(f.msg[0], "not in the layout"), "missing binding: %d", f.n);
      memset(&f, 0, sizeof f); CHECK(check_pipeline(s, 1, NULL, 0, &f) == 0, "no layout given: only the module-level checks run");
      stage_t s1[1] = { { ARR(k_lm_set1_fs), FS, "fs" } };
      memset(&f, 0, sizeof f); CHECK(check_pipeline(s1, 1, &ok, 1, &f) == 1 && strstr(f.msg[0], "only set 0"), "set 1: %d", f.n);
      stage_t sa[1] = { { ARR(k_lm_arr_fs), FS, "fs" } };
      lay_t one = { 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, FS }, two = { 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2, FS };
      memset(&f, 0, sizeof f); CHECK(check_pipeline(sa, 1, &one, 1, &f) == 1 && strstr(f.msg[0], "needs 2"), "array count: %d", f.n);
      memset(&f, 0, sizeof f); CHECK(check_pipeline(sa, 1, &two, 1, &f) == 0, "array count 2 accepted");
      puts("PASS: layout defects are found one by one (descriptor type, stage flags, array count, missing binding, set 1); a correct layout passes"); }
    /* 4. VS and FS declare different kinds on one binding: found; the derived layout (first wins) additionally fails the second stage */
    { stage_t s[2] = { { ARR(k_lm_buf_vs), VS, "vs" }, { ARR(k_lm_smp_fs), FS, "fs" } };
      memset(&f, 0, sizeof f);
      int n = derived_layout(s, 2, lay);
      int c = check_pipeline(s, 2, lay, n, &f);
      dump(&f);
      CHECK(c == 2 && strstr(f.msg[0], "vs vs fs"), "cross-stage collision: %d findings", c);
      puts("PASS: VS / FS kind clash on one binding is found"); }
    return 0;
}

/* ---- bundle mode ---- */
static uint32_t *slurp32(const char *path, size_t *nw) {
    FILE *fp = fopen(path, "rb"); if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long l = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (l < 20 || (l & 3)) { fclose(fp); return NULL; }
    uint32_t *b = malloc((size_t)l);
    if (!b || fread(b, 1, (size_t)l, fp) != (size_t)l) { fclose(fp); free(b); return NULL; }
    fclose(fp); *nw = (size_t)l / 4; return b;
}
/* returns 0 = clean, 1 = findings, 2 = not a bundle */
static int check_bundle(const char *dir) {
    char p[1400];
    size_t nv, nf;
    snprintf(p, sizeof p, "%s/vs.spv", dir); uint32_t *vs = slurp32(p, &nv);
    snprintf(p, sizeof p, "%s/fs.spv", dir); uint32_t *fs = slurp32(p, &nf);
    if (!vs || !fs) { free(vs); free(fs); printf("%s: no vs.spv / fs.spv\n", dir); return 2; }
    stage_t st[2] = { { vs, nv, VK_SHADER_STAGE_VERTEX_BIT, "vs" }, { fs, nf, VK_SHADER_STAGE_FRAGMENT_BIT, "fs" } };
    lay_t rec[128]; int nrec = 0;
    snprintf(p, sizeof p, "%s/state", dir);
    FILE *sf = fopen(p, "r"); char line[512];
    while (sf && fgets(line, sizeof line, sf)) {
        unsigned b, t, c, s;
        if (sscanf(line, "layout.binding %u %u %u %u", &b, &t, &c, &s) == 4 && nrec < 128) rec[nrec++] = (lay_t){ b, t, c, s };
    }
    if (sf) fclose(sf);
    findings_t f; memset(&f, 0, sizeof f);
    int c1 = nrec ? check_pipeline(st, 2, rec, nrec, &f) : 0;
    int c1f = f.n;
    lay_t der[40]; int nd = derived_layout(st, 2, der);
    findings_t g; memset(&g, 0, sizeof g);
    int c2 = check_pipeline(st, 2, der, nd, &g);
    printf("%s: recorded layout %d bindings -> %d finding(s); layout derived now (lagfx_layout_bindings_from_spv) %d bindings -> %d finding(s)\n", dir, nrec, c1, nd, c2);
    for (int i = 0; i < c1f; i++) printf("    recorded: %s\n", f.msg[i]);
    for (int i = 0; i < g.n; i++) printf("    derived:  %s\n", g.msg[i]);
    free(vs); free(fs);
    return (c1 || c2) ? 1 : 0;
}
static int is_bundle(const char *d) { char p[1400]; struct stat s; snprintf(p, sizeof p, "%s/state", d); return stat(p, &s) == 0; }
static int check_path(const char *path, int *nb) {
    if (is_bundle(path)) { (*nb)++; return check_bundle(path); }
    DIR *d = opendir(path); struct dirent *e; int bad = 0;
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, "pipefail_", 9)) {
        char p[1400]; snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (is_bundle(p)) { (*nb)++; if (check_bundle(p) == 1) bad = 1; }
    }
    if (d) closedir(d);
    return bad;
}

int main(int argc, char **argv) {
    if (argc <= 1 && !getenv("LAGFX_PIPEFAIL_CHECK_DIR")) return selftest();
    int rc = 0, nb = 0;
    if (argc <= 1) { if (selftest()) return 1; if (check_path(getenv("LAGFX_PIPEFAIL_CHECK_DIR"), &nb) == 1) rc = 1; if (!nb) { puts("no pipefail bundle found: skip"); return 77; } }
    else for (int i = 1; i < argc; i++) if (check_path(argv[i], &nb) == 1) rc = 1;
    printf("%d bundle(s) checked: %s\n", nb, rc ? "FINDINGS (exit 1)" : "clean");
    return rc;
}
#endif
