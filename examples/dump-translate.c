/* Re-translates every function of the metallib blobs (lib_*.bin) of an arm64 dump with the CURRENT air2spv translator, so that translator fixes can be checked with
 * spirv-val against the guest's real shaders (no device needed). usage: dump-translate <dump dir> <out dir>
 * Writes <out>/<lib hash>_<function>_<vs|fs>.spv. SPDX-License-Identifier: MIT */
#include "air/bitcode_reader.h"
#include "air2spirv/metallib_extract.h"
#include "air2spv/translate.h"
#include "libapplegfx-vulkan.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1u); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <dump dir> <out dir>\n", argv[0]); return 2; }
    DIR *d = opendir(argv[1]); if (!d) { perror("opendir"); return 1; }
    struct dirent *e; int ok = 0, bad = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "lib_", 4) != 0) continue;
        char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[1], e->d_name);
        size_t n = 0; uint8_t *lib = slurp(path, &n); if (!lib) continue;
        lagfx_metallib_function_t fn[8]; size_t nf = 0;
        if (lagfx_metallib_extract_functions(lib, n, fn, 8, &nf) != LAGFX_OK) { free(lib); continue; }
        for (size_t k = 0; k < nf && k < 8; k++) {
            if (!fn[k].bitcode) continue;
            const char *st = fn[k].stage == LAGFX_METALLIB_STAGE_VERTEX ? "vs" : fn[k].stage == LAGFX_METALLIB_STAGE_FRAGMENT ? "fs" : "x";
            lagfx_air_module_t *m = NULL; uint8_t *spv = NULL; size_t sz = 0;
            if (lagfx_air_module_open(fn[k].bitcode, fn[k].bitcode_len, &m) != LAGFX_OK || !m) { fprintf(stderr, "%s %s: module_open failed\n", e->d_name, fn[k].name); bad++; continue; }
            lagfx_status_t s = lagfx_air2spv_translate_module(m, &spv, &sz);
            lagfx_air_module_free(m);
            if (s != LAGFX_OK || !spv) { fprintf(stderr, "%s %s: translate failed\n", e->d_name, fn[k].name); bad++; continue; }
            char out[1400]; snprintf(out, sizeof out, "%s/%.*s_%s_%s.spv", argv[2], (int)(strlen(e->d_name) - 8), e->d_name + 4, fn[k].name, st);
            for (char *c = out + strlen(argv[2]) + 1; *c; c++) if (*c == '.' && c[1] != 's') *c = '_';
            FILE *o = fopen(out, "wb"); if (o) { fwrite(spv, 1, sz, o); fclose(o); ok++; }
            free(spv);
        }
        free(lib);
    }
    closedir(d); printf("translated %d functions, %d failed\n", ok, bad); return 0;
}
