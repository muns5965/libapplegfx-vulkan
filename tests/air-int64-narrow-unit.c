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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(l > 0 ? (size_t)l : 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
static int validates(const uint8_t *spv, size_t n, const char *tmp) {
    FILE *f = fopen(tmp, "wb"); if (!f) return -1; fwrite(spv, 1, n, f); fclose(f);
    char cmd[600]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.2 %s >/dev/null 2>&1", tmp);
    return system(cmd) == 0;
}

int main(void) {
    const char *dir = SRCDIR "/tests/fixtures";
    DIR *d = opendir(dir); CHECK(d, "fixture dir %s", dir); if (!d) return 1;
    int have_val = system("spirv-val --version >/dev/null 2>&1") == 0;
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
        lagfx_air2spv_set_atomic_lenient(1);
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
                    int v0 = validates(s0, n0, tmp), v1 = validates(s1, n1, tmp);
                    if (v0 && !v1) { vnarrow_only++; CHECK(0, "%s: narrowing breaks spirv-val", e->d_name); }
                    else if (!v0) vbase_bad++; else vok++;
                }
            } else {
                refused++;
                CHECK(lagfx_air2spv_last_narrow_refusal()[0] || r1 != 0, "%s: refused without a reason", e->d_name);
            }
        }
        free(s0); free(s1); lagfx_air_module_free(m); free(air);
    }
    closedir(d); remove(tmp);
    printf("INFO fixtures translated=%d needing-Int64=%d narrowed=%d refused=%d spirv-val(narrowed ok=%d, base already invalid=%d, narrowing-only failures=%d, validator %s)\n",
           total, need64, narrowed, refused, vok, vbase_bad, vnarrow_only, have_val ? "present" : "absent");
    CHECK(total > 100, "fixture sweep too small (%d)", total);
    CHECK(refused >= 3, "the 64-bit atomic fixtures must be refused (refused=%d)", refused);
    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
