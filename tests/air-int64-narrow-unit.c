/* Int64 narrowing sweep (devices without shaderInt64): every AIR fixture is translated with narrowing OFF and ON.
 *   - a narrowed module never needs the Int64 capability
 *   - a module that translated without narrowing is either narrowed or REFUSED (never silently dropped)
 *   - when spirv-val is installed, a narrowed module must validate whenever the un-narrowed one does
 * Host-only: it checks the translation, not the computed values (no GPU run of the narrowed module). */
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"
#include "vulkan/caps.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_int64_cap(const uint8_t *b, size_t n) {
    const uint32_t *w = (const uint32_t *)(const void *)b;
    for (size_t i = 5; i < n / 4;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu; if (!wc) return 0; if (op == 17 && w[i + 1] == 11u) return 1; i += wc; }
    return 0;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(l > 0 ? (size_t)l : 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static int validates(const uint8_t *spv, size_t n, const char *tmp, const char *env) {
    FILE *f = fopen(tmp, "wb"); if (!f) return -1; fwrite(spv, 1, n, f); fclose(f);
    char cmd[600]; snprintf(cmd, sizeof cmd, "spirv-val --target-env %s %s >/dev/null 2>&1", env, tmp);
    return system(cmd) == 0;
}

static void sweep(int strict_atomics, int have_val) {
    const char *dir = SRCDIR "/tests/fixtures";
    DIR *d = opendir(dir); CHECK(d, "fixture dir %s", dir); if (!d) return;
    char tmp[64]; snprintf(tmp, sizeof tmp, "/tmp/air-int64-narrow-%d.spv", (int)getpid());
    int total = 0, need64 = 0, narrowed = 0, refused = 0, vok = 0, vbase_bad = 0, vnarrow_only = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name); if (l < 8 || strcmp(e->d_name + l - 7, ".air.bc")) continue;
        char path[600]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t an = 0; uint8_t *air = slurp(path, &an); if (!air) continue;
        lagfx_air_module_t *m = NULL;
        if (lagfx_air_module_open(air, an, &m) != LAGFX_OK) { free(air); continue; }
        uint8_t *s0 = NULL, *s1 = NULL; size_t n0 = 0, n1 = 0;
        lagfx_air2spv_set_atomic_lenient(strict_atomics ? 0 : 1);
        lagfx_air2spv_set_narrow_int64(0); int r0 = lagfx_air2spv_translate_module(m, &s0, &n0);
        lagfx_air2spv_set_narrow_int64(1); int r1 = lagfx_air2spv_translate_module(m, &s1, &n1);
        lagfx_air2spv_set_narrow_int64(0); lagfx_air2spv_set_atomic_lenient(-1);
        if (r0 == 0 && s0) {
            total++;
            uint32_t n_off = 0, n_on = 0;
            CHECK(lagfx_spv_required_caps_ex(s0, n0, &n_off), "%s: caps scan", e->d_name);
            if (n_off & (1u << LAGFX_VK_CAP_INT64)) need64++;
            if (r1 == 0 && s1) {
                narrowed++;
                CHECK(lagfx_spv_required_caps_ex(s1, n1, &n_on), "%s: caps scan (narrowed)", e->d_name);
                CHECK(!(n_on & (1u << LAGFX_VK_CAP_INT64)), "%s: narrowed module still needs Int64 (0x%x)", e->d_name, n_on);
                if (have_val && (n_off & (1u << LAGFX_VK_CAP_INT64))) {
                    static const char *const envs[] = { "vulkan1.2", "vulkan1.1spv1.4" };   /* S26-class and the S8 target (Vulkan 1.1 + VK_KHR_spirv_1_4) */
                    for (unsigned k = 0; k < 2u; k++) {
                        int v0 = validates(s0, n0, tmp, envs[k]), v1 = validates(s1, n1, tmp, envs[k]);
                        if (v0 && !v1) { vnarrow_only++; CHECK(0, "%s: narrowing breaks spirv-val (%s)", e->d_name, envs[k]); }
                        else if (!v0) vbase_bad++; else vok++;
                    }
                }
            } else {
                refused++;
                CHECK(r1 != 0 && lagfx_air2spv_last_narrow_refusal()[0], "%s: refused without a narrowing reason (status %d)", e->d_name, r1);
            }
        }
        free(s0); free(s1); lagfx_air_module_free(m); free(air);
    }
    closedir(d); remove(tmp);
    printf("INFO [%s atomics] fixtures translated=%d needing-Int64=%d narrowed=%d refused=%d spirv-val(narrowed ok=%d, base already invalid=%d, narrowing-only failures=%d, validator %s)\n",
           strict_atomics ? "strict" : "lenient", total, need64, narrowed, refused, vok, vbase_bad, vnarrow_only, have_val ? "present" : "absent");
    CHECK(total > 100, "fixture sweep too small (%d)", total);
    {   /* a fragment/vertex function refused for 64-bit data must FAIL the translation: it used to fall back to the flat-colour reference stub (a red fragment) and look 'narrowed' */
        size_t bn = 0; uint8_t *bc = slurp(SRCDIR "/tests/fixtures/rt_bits32_fragment.air.bc", &bn); lagfx_air_module_t *mm = NULL; uint8_t *sp = NULL; size_t sn = 0;
        CHECK(bc && lagfx_air_module_open(bc, bn, &mm) == LAGFX_OK, "rt_bits32 fixture");
        if (mm) {
            lagfx_air2spv_set_narrow_int64(0); int r0 = lagfx_air2spv_translate_module(mm, &sp, &sn); uint8_t *s1 = NULL; size_t n1 = 0;
            lagfx_air2spv_set_narrow_int64(1); int rr = lagfx_air2spv_translate_module(mm, &s1, &n1); lagfx_air2spv_set_narrow_int64(0);
            /* either refused, or translated by the Int64 LOWERING: no Int64 capability and at least as large as the unnarrowed translation (a reference stub is far smaller) */
            CHECK(rr != 0 ? !s1 : (r0 == 0 && s1 && !has_int64_cap(s1, n1) && n1 >= sn), "refused 64-bit fragment fell back to a stub (status %d, %zu vs %zu bytes)", rr, n1, sn);
            free(sp); free(s1); lagfx_air_module_free(mm); }
        free(bc);
    }
    {   /* overflow past 32 bits that is OBSERVED (compare / float convert / equality) must be refused; the same IR translates without narrowing */
        static const struct { const char *f, *why; } ov[] = { { "rt_i64ovf_cmp_fragment.air.bc", "unsigned compare" }, { "rt_i64ovf_float_fragment.air.bc", "convert to float" },
            { "rt_i64ovf_eq_fragment.air.bc", "equality" }, { "rt_i64ovf_signed_fragment.air.bc", "signed compare" } };
        for (unsigned k = 0; k < 4u; k++) {
            char path[600]; snprintf(path, sizeof path, SRCDIR "/tests/fixtures/%s", ov[k].f);
            size_t bn = 0; uint8_t *bc = slurp(path, &bn); lagfx_air_module_t *mm = NULL; uint8_t *s0 = NULL, *s1 = NULL; size_t n0 = 0, n1 = 0;
            CHECK(bc && lagfx_air_module_open(bc, bn, &mm) == LAGFX_OK, "%s: fixture", ov[k].f);
            if (mm) {
                lagfx_air2spv_set_narrow_int64(0); CHECK(lagfx_air2spv_translate_module(mm, &s0, &n0) == 0 && s0, "%s: must translate without narrowing", ov[k].f);
                lagfx_air2spv_set_narrow_int64(1); int rr = lagfx_air2spv_translate_module(mm, &s1, &n1); lagfx_air2spv_set_narrow_int64(0);
                /* narrowing cannot represent it: refused, or lowered to word pairs (no Int64 capability, not smaller than the wide translation); a float conversion is still refused */
                CHECK(rr != 0 ? !s1 : (k != 1u && s1 && !has_int64_cap(s1, n1) && n1 >= n0), "%s: overflow %s must be refused or lowered when narrowed (status %d)", ov[k].f, ov[k].why, rr);
                free(s0); free(s1); lagfx_air_module_free(mm);
            }
            free(bc);
        }
    }
    CHECK(refused >= 3, "the 64-bit atomic fixtures must be refused (refused=%d)", refused);
}

int main(void) {
    int have_val = system("spirv-val --version >/dev/null 2>&1") == 0;
    const char *req = getenv("LAGFX_REQUIRE_SPIRV_VAL");
    if (req && req[0] == '1') CHECK(have_val, "LAGFX_REQUIRE_SPIRV_VAL=1 but spirv-val is not installed");
    sweep(0, have_val);   /* lenient atomics: also exercises the float-atomic fixtures */
    sweep(1, have_val);   /* product behaviour */
    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
