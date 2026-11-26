/*
 * libapplegfx-vulkan — ARM64 bounded capture of REAL guest pipeline / library / binding data (LAGFX_ARM64_DUMP=1).
 * src/protocol/arm64_dump.h
 *
 * Independent of LAGFX_ARM64_EXEC (decode-only runs work, no Vulkan needed; with both on, both happen). On the first use of each pipeline-state object
 * (type 7) and function object (type 6) it logs one bounded record (type, ref, size, FNV-1a 64 hash, first 128 bytes, decoded function refs / vertex
 * layout / library header, names and stages) and, for the first 16 draws, the index->ref maps of the vertex/fragment buffer, texture and sampler tables
 * (tags 125/126/110/111/114/112) plus the pipeline ref.
 * If LAGFX_ARM64_DUMP_DIR names a directory, the FULL payloads are written there: pso_<task>_<ref>_<hash>.bin, lib_<hash>.bin and an appended index.txt
 * (ref -> file, full_size, saved_size, truncated, hash; draw binding maps). Type 11 (mapped texture) objects of the resource list get an index line
 * `mapped task= ref= mapping= tag= desc_size= surface=WxH fourcc= row= first= first128=<hex>` (surface geometry from the shell's get_surface_desc, 'unknown' without one). Files are created exclusively (O_EXCL|O_NOFOLLOW) relative to the opened
 * directory, never overwritten; the directory itself is opened with O_NOFOLLOW. Caps: LAGFX_ARM64_DUMP_MAX_FILE (default 4 MiB) per file and
 * LAGFX_ARM64_DUMP_MAX_TOTAL (default 32 MiB) over all files. A file over its cap is written as far as it fits with the suffix .TRUNCATED; once the total
 * is used no more files are written but the index still lists every blob (saved=0). Counters: p->diag2.exec.pipe.dump_*.
 * State (seen set, descriptor of the directory, byte total) is per process.
 */
#ifndef LAGFX_PROTOCOL_ARM64_DUMP_H
#define LAGFX_PROTOCOL_ARM64_DUMP_H

#include <stdbool.h>
#include <stdint.h>
#include "state.h"
#include "arm64_exec_decode.h"

bool lagfx_a64_dump_enabled(void);   /* LAGFX_ARM64_DUMP == "1", read at every call */
void lagfx_a64_dump_reset(void);     /* forgets the seen set, closes the directory (tests; a new run) */

/* Called by the decoder once per exec for the exec's resource list ({ref, flags} pairs) and then per buffer segment. */
void lagfx_a64_dump_resources(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint8_t *res, uint32_t nres);
/* Called when the resolver / object table first READS a pipeline-state (type 7: pipelines, samplers) or function (type 6) object that the exec's resource list did not
 * name (tag 116 / 110 / 114 / 112 ... reach them by ref): same capture as for the resource list (seen set, caps, per-run directory, index lines obj / pso / fn;
 * the functions a pipeline names are captured with it). Other types are ignored. No-op unless LAGFX_ARM64_DUMP=1. `e` is the 12-byte object-table entry. */
void lagfx_a64_dump_touch_object(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t e[3]);
/* A pipeline whose vkCreateGraphicsPipelines failed: with LAGFX_ARM64_DUMP=1 and an open LAGFX_ARM64_DUMP_DIR the two SPIR-V modules are written as
 * spv_<fnv64 of the module>_vs.spv / spv_<hash>_fs.spv (exclusive create: a repeat of the same module is not rewritten; same caps as every blob) plus an index
 * line `spv vs=<file> fs=<file> vs_bytes= fs_bytes=`, so the failure can be reproduced offline with spirv-val / lavapipe. No-op otherwise. */
void lagfx_a64_dump_spirv(lagfx_protocol_t *p, const void *vs, size_t vs_bytes, const void *fs, size_t fs_bytes);
void lagfx_a64_dump_segment(lagfx_protocol_t *p, const lagfx_task_entry_t *task, const uint8_t *buf, uint32_t avail, const lagfx_exec_seg_t *seg);

#endif
