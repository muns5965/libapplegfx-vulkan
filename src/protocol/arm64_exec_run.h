/*
 * libapplegfx-vulkan — ARM64 GPU exec (0x43 / 0x45): execution of Render segments on Vulkan.
 * src/protocol/arm64_exec_run.h
 *
 * Reference: Apple's devices/pvg_metal.m exec (~1847-1984), beginPass (1109), render (1197), object:task:.
 *
 * OFF unless protocol_mode == LAGFX_PROTO_ARM64_VMAPPLE AND the environment variable LAGFX_ARM64_EXEC is "1". Called by
 * lagfx_arm64_exec_decode (arm64_exec_decode.c), which keeps decoding, counting and logging exactly as before; this module only adds the
 * Vulkan recording. It never completes a stamp or raises an interrupt (the drain loop does, as for every command), never aborts an exec
 * and never crashes on a malformed record: every tag it cannot execute is counted in p->diag2.exec.run (per tag) and skipped.
 *
 * What a pass does (one exec = one command buffer, submitted and waited for in lagfx_a64_exec_end):
 *   26 begin pass  -> vkCmdBeginRendering on colour attachment 0 (load action DontCare/Load/Clear, clear colour from the record)
 *   116 pipeline   -> a VkPipeline built (and cached per colour format) from what the pipeline resolver returns for the object
 *   125 vertex bufs / 126 vertex buffer offset -> vkCmdBindVertexBuffers at draw time (one stream, see lagfx_a64_pipeline_src_t)
 *   130 viewport / 117 scissor -> the draw region of lagfx_vk_cmd_set_region
 *   1 draw arrays, 7 draw indexed (triangle lists only; the pipeline builder has a fixed topology)
 *   110/111/114/112 fragment buffers/offset/textures/samplers -> recorded and counted as "partial": not bound (the pipeline layouts the
 *       existing builder produces for stage-in pipelines have no descriptor sets)
 *   30,34,35,36 pre-pass descriptors -> counted as partial; 32,33 and every other tag -> counted as unsupported.
 * Pipelines come from the (task, ref) table of arm64_objects.h; buffers and textures (render targets included) are the resource mirror objects of arm64_resources.h, shared with Blit.
 */
#ifndef LAGFX_PROTOCOL_ARM64_EXEC_RUN_H
#define LAGFX_PROTOCOL_ARM64_EXEC_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"
#include "arm64_exec_decode.h"
#include "../vulkan/pipeline_build.h"

/* True iff arm64 mode and LAGFX_ARM64_EXEC=1 (read at every call). */
bool lagfx_a64_exec_enabled(const lagfx_protocol_t *p);

/* What a pipeline resolver hands back for a pipeline object (guest object type 7, serialized render pipeline). The Apple serialized
 * descriptor is not decoded by this module: the resolver turns it into ready shader modules + layout. desc.color_format is overwritten
 * with the pass's format when the VkPipeline is built. */
typedef struct {
    lagfx_pipeline_desc_t desc;
    uint32_t vertex_buffer_index;   /* the Metal vertex buffer slot that feeds Vulkan binding 0 (lagfx_pipeline_build has one stream) */
    void (*release)(void *user);    /* optional, called when the pipeline object dies (shader modules etc. belong to the resolver) */
    void *user;
} lagfx_a64_pipeline_src_t;

typedef bool (*lagfx_a64_pipeline_resolver_t)(void *ctx, lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref,
                                              const uint32_t entry[3], lagfx_a64_pipeline_src_t *out);

/* Installs the resolver (creates the exec state if needed). NULL = every pipeline is unresolved: draws are skipped and counted. */
void lagfx_a64_exec_set_pipeline_resolver(lagfx_protocol_t *p, lagfx_a64_pipeline_resolver_t fn, void *ctx);

/* Per-exec bracket, called by the decoder. ONE exec context and ONE command buffer per exec, owned by the resource mirror (arm64_resources.h):
 * begin: applies the resource-flag rules through the mirror (upload only for flag 0x01 / first use, never over a stale GPU result); returns false when nothing
 *   executes (and, if the GPU is still on the previous exec, marks the command's stamp failed).
 * segment: one buffer's segment: Render (type 0) and Blit (type 2) both record into that command buffer; the render target / textures / buffers are the
 *   mirror objects the blits use, with layout + memory barriers between a Render pass and a following Blit and back.
 * end: ends an open pass, submits ONCE, waits bounded. DONE -> flag-0x04 write-backs to the guest pages are done. PENDING -> the fence has not signalled:
 *   NOT failed, every object stays alive, and the command's stamp is queued behind it in the completion queue (arm64_completion.h) so it completes only
 *   after lagfx_a64_res_poll reported DONE. FAILED (before submit) -> the stamp completes as failed. */
bool lagfx_a64_exec_begin(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *res, uint32_t nres);
void lagfx_a64_exec_segment(lagfx_protocol_t *p, lagfx_task_entry_t *task, const uint8_t *buf, uint32_t avail, uint64_t len, const lagfx_exec_seg_t *seg);
typedef enum {
    LAGFX_A64_EXEC_NONE = 0,   /* nothing was recorded (no Render pass with a target) */
    LAGFX_A64_EXEC_DONE,       /* submitted and the fence signalled within LAGFX_GPU_WAIT_MS */
    LAGFX_A64_EXEC_PENDING,    /* submitted, fence NOT yet signalled: still running, not failed. Everything it uses stays alive until the fence signalled (lagfx_a64_exec_poll / the completion queue) */
    LAGFX_A64_EXEC_FAILED      /* failed before the work reached the queue (record / submit error) */
} lagfx_a64_exec_status_t;
lagfx_a64_exec_status_t lagfx_a64_exec_end(lagfx_protocol_t *p);
/* Reaps pending submissions whose fence has signalled; returns how many are still outstanding. Never blocks. */
uint32_t lagfx_a64_exec_poll(lagfx_protocol_t *p);

/* Hook for src/handlers/compute/render_inner_ops.c (the existing inner-op table, same tag numbers): true iff the ARM64 executor is recording an
 * exec and has taken the op; false immediately in every other situation, so the x86 handlers are unchanged. */
bool lagfx_a64_exec_render_op(lagfx_protocol_t *p, uint32_t tag, const uint8_t *payload, size_t len);

/* Lifetime: task teardown (CmdDeleteTask), protocol reset / free. All safe when nothing was ever created. */
void lagfx_a64_exec_drop_task(lagfx_protocol_t *p, uint32_t task_id);
void lagfx_a64_exec_free(lagfx_protocol_t *p);

/* Number of objects the table holds / has created / has destroyed (for tests and diagnostics). */
void lagfx_a64_exec_object_counts(const lagfx_protocol_t *p, uint32_t *live, uint64_t *created, uint64_t *destroyed);

/* Reads a texture object's pixels back (linear, width*4 bytes per row for the 4-byte formats). Test and debug aid. */
bool lagfx_a64_exec_readback_texture(lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, void *dst, size_t dst_size,
                                     uint32_t *width, uint32_t *height);

#endif
