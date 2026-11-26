/* Writes every row of the pipeline-reduce ladder for a fragment module to a directory (DRY run: nothing is built, every row counts as FAIL so the adaptive rows follow the worst
 * case) plus manifest.tsv (fs hash, row id, sha256[0..16], bytes, instructions, meaning, what). Offline helper: the files are what the device builds, the manifest keys the
 * device log lines (sha=) to the offline spirv-val results. usage: reduce-rows <fs.spv> <outdir> [<dir with *.red.*.fs.spv files>]. SPDX-License-Identifier: MIT */
#include "vulkan/pipeline_reduce.h"
#include <stdio.h>
#include <stdlib.h>

static void emit(void *u, const char *l) { (void)u; puts(l); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <fs.spv> <outdir> [filedir]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n);
    if (!w || fread(w, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    lagfx_reduce_cfg_t cfg = { .fs = w, .fs_bytes = (size_t)n, .dump_dir = argv[2], .dry = 1, .emit = emit, .file_dir = argc > 3 ? argv[3] : NULL };
    lagfx_pipeline_reduce_cfg_env(&cfg);
    lagfx_pipeline_reduce_run(&cfg, NULL);
    return 0;
}
