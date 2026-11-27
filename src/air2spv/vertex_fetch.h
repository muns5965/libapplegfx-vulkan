/* Source layout of the stage-in values of a driver-generated vertex fetch wrapper. SPDX-License-Identifier: MIT
 *
 * Metal compiles a render pipeline whose vertex function has a MTLVertexDescriptor into a wrapper "air.vertexFetchFunction": it carries no air.vertex_input
 * metadata, it loads every attribute from the vertex buffer argument at a constant byte offset (GEP over the per-vertex struct, constant byte index) and passes the
 * loaded values POSITIONALLY to the real vertex function. The translator translates the real function (its arguments become the stage-in Locations 0,1,..); this
 * helper reads the wrapper and tells, for each argument of the real function, which buffer byte offset its value was loaded from.
 * Only what the wrapper's instructions state is reported; anything not a plain "constant-offset load from a buffer argument" is unknown (-1). */
#ifndef LAGFX_AIR2SPV_VERTEX_FETCH_H
#define LAGFX_AIR2SPV_VERTEX_FETCH_H
#include "air/bitcode_reader.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t offset;   /* byte offset inside the per-vertex record; -1 = unknown (0 is a valid offset) */
    int32_t bytes;    /* store size of the loaded value; -1 = unknown */
    int32_t buffer;   /* Metal buffer slot of the buffer argument (its air.location_index); -1 = unknown */
    int32_t unfed;    /* 1 = the wrapper passes `undef` for this argument: the real function receives an undefined value, nothing is read from the buffer for it */
} lagfx_vfetch_in_t;

/* For argument k of function `fn_idx` (the real vertex function) fill out[k] (k < cap) from the module's air.vertexFetchFunction wrapper that calls it. Returns the number
 * of arguments described (the callee's argument count, capped), or 0 when the module has no such wrapper / the call is not found (out[] is then untouched). */
size_t lagfx_air_vertex_fetch_inputs(const lagfx_air_module_t *m, uint32_t fn_idx, lagfx_vfetch_in_t *out, size_t cap);

/* OpName given to a stage-in Input variable whose source is known: "lagfx.vin.o<offset>[.s<bytes>][.b<buffer>]" (decimal), or "lagfx.vin.u" when the wrapper passes undef for it. */
#define LAGFX_VIN_NAME_UNFED "lagfx.vin.u"
#define LAGFX_VIN_NAME_PREFIX "lagfx.vin.o"
#endif
