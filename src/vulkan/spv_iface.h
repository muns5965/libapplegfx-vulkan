/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — read-only SPIR-V interface reflection for the pipeline repro (src/vulkan/pipeline_repro.c): stage inputs / outputs (Location, Component, scalar
 * type and width, interpolation decorations, BuiltIn) and input attachments (DescriptorSet, Binding, InputAttachmentIndex are three DIFFERENT numbers). Pure C, no Vulkan
 * headers, never used by the normal pipeline path.
 */
#ifndef LIBAPPLEGFX_VULKAN_SPV_IFACE_H
#define LIBAPPLEGFX_VULKAN_SPV_IFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_SPV_IFACE_MAX_IO 160
#define LAGFX_SPV_IFACE_MAX_IA 16

typedef struct {
    uint32_t id, loc, comp;        /* Location / Component (valid when has_loc) */
    uint32_t builtin;              /* SPIR-V BuiltIn enum (valid when has_builtin) */
    bool     has_loc, has_builtin;
    uint8_t  kind;                 /* 0 float, 1 signed int, 2 unsigned int, 3 other (bool / struct without a scalar base) */
    uint32_t width;                /* scalar bit width */
    uint32_t ncomp;                /* vector size per location slot (1 for scalars, matrix: rows) */
    uint32_t nloc;                 /* Locations consumed (arrays, matrix columns, 64-bit 3/4-vectors count) */
    uint32_t ncomp_total;          /* 32-bit components consumed in all */
    bool     flat, noperspective, centroid, sample, patch, relaxed;
    char     name[40];
} lagfx_spv_io_t;

typedef struct {
    uint32_t id, set, binding, index;   /* index = InputAttachmentIndex */
    bool     has_index, in_interface;
    char     name[40];
} lagfx_spv_ia_t;

typedef struct {
    bool     ok;                   /* parsed; the entry point of the model was found */
    uint32_t version;              /* SPIR-V version word */
    uint32_t exec_model;
    char     entry[40];
    uint32_t n_in, n_out, n_ia;
    bool     truncated;
    bool     cap_input_attachment, raster_order_exec_mode;   /* OpCapability InputAttachment / (reserved) */
    lagfx_spv_io_t in[LAGFX_SPV_IFACE_MAX_IO], out[LAGFX_SPV_IFACE_MAX_IO];
    lagfx_spv_ia_t ia[LAGFX_SPV_IFACE_MAX_IA];
} lagfx_spv_iface_t;

/* exec_model: 0 Vertex, 4 Fragment. Fills `o` (caller zeroes nothing). Returns false on a malformed module or no entry point of that model. */
bool lagfx_spv_iface_reflect(const uint32_t *words, size_t nwords, uint32_t exec_model, lagfx_spv_iface_t *o);

#endif
