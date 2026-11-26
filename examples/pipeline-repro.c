/* Rebuilds a failed pipeline from a failure bundle (pipefail_<hash>/, written when LAGFX_PIPEFAIL_BUNDLE_DIR / the arm64 dump dir is set) on this machine's Vulkan device and
 * prints the report. usage: pipeline-repro <bundle dir> [--matrix] [--m13] [--reduce [--no-isolate] [--only id,id] [--from id] [--list-rows] [--timeout ms]]. --matrix adds the DIAGNOSTIC variant matrix; --m13 the -13 matrix (rows M0..M10, LAGFX_PIPELINE_MATRIX_DIR=<dir> for M8). Exit: 0 = the recorded failure reproduced, 1 = it did not
 * (builds, or fails differently), 2 = usage / bundle / Vulkan error. SPDX-License-Identifier: MIT */
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <bundle dir> [--matrix] [--m13]\n", argv[0]); return 2; }
    lagfx_pipeline_repro_opts_t o = { .struct_size = sizeof o, .flags = 0 };
    int noiso = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--no-isolate")) noiso = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--matrix")) o.flags |= LAGFX_PIPELINE_REPRO_MATRIX;
        else if (!strcmp(argv[i], "--m13")) o.flags |= LAGFX_PIPELINE_REPRO_M13;
        else if (!strcmp(argv[i], "--reduce")) o.flags |= LAGFX_PIPELINE_REPRO_REDUCE | (noiso ? 0u : LAGFX_PIPELINE_REPRO_ISOLATE);
        else if (!strcmp(argv[i], "--no-isolate")) noiso = 1, o.flags &= ~LAGFX_PIPELINE_REPRO_ISOLATE;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) o.reduce_only = argv[++i];
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) o.reduce_from = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) o.reduce_timeout_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list-rows")) o.reduce_list_only = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    lagfx_pipeline_repro_result_t r;
    char *rep = NULL;
    lagfx_status_t st = lagfx_pipeline_repro_run(argv[1], &o, &r, &rep);
    if (rep) fputs(rep, stdout);
    lagfx_pipeline_repro_free_report(rep);
    if (st != LAGFX_OK) { fprintf(stderr, "repro could not run: status %d\n", (int)st); return 2; }
    return r.reproduced ? 0 : 1;
}
