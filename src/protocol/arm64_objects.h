/*
 * libapplegfx-vulkan — ARM64 per-task object table, keyed by (task, ref).
 * src/protocol/arm64_objects.h
 *
 * Reference: Apple's devices/pvg_metal.m `object:task:` / `get:task:`. The guest keeps a list of 12-byte entries per task
 * ({u32 type | size << 8, u32 addr lo, u32 addr hi}, at task->objects_addr + ref * 12). The host creates its object the first time a
 * command references the ref, re-creates it when the 12-byte entry (or the task's root page) changes, and drops everything of a task
 * when the task is torn down. This module owns only the keying and the lifetime; what an object IS (VkBuffer, image, pipeline...) is the
 * caller's, through the ops. It has no Vulkan dependency, so the lifetime rules are unit-testable without a device.
 */
#ifndef LAGFX_PROTOCOL_ARM64_OBJECTS_H
#define LAGFX_PROTOCOL_ARM64_OBJECTS_H

#include <stdbool.h>
#include <stdint.h>
#include "state.h"

#define LAGFX_A64_OBJ_BUFFER      1u
#define LAGFX_A64_OBJ_TEXTURE     2u
#define LAGFX_A64_OBJ_FUNCTION    6u
#define LAGFX_A64_OBJ_SERIALIZED  7u   /* pipelines, samplers, depth/stencil states, fences */
#define LAGFX_A64_OBJ_LINEAR_TEX  8u
#define LAGFX_A64_OBJ_BUFFER_V2   19u  /* "macOS 27 buffer", same entry as type 1 */

typedef struct lagfx_a64_obj_ops {
    void *ctx;
    /* Build the object for entry[3] of (task, ref). May return NULL (unsupported / failed): that is remembered, so it is not retried until
     * the entry changes. entry[0] & 0xff is the type, entry[0] >> 8 the size. Never called for an empty slot (type 0). */
    void *(*create)(void *ctx, lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, const uint32_t entry[3]);
    /* Release what create returned (never called with NULL). */
    void (*destroy)(void *ctx, void *obj);
} lagfx_a64_obj_ops_t;

typedef struct lagfx_a64_objects lagfx_a64_objects_t;

typedef struct {
    uint64_t created, destroyed, recreated, failed, hits, read_failed;
    uint32_t live;                 /* nodes currently held (including remembered failures) */
} lagfx_a64_obj_stats_t;

lagfx_a64_objects_t *lagfx_a64_objects_new(const lagfx_a64_obj_ops_t *ops);
/* Destroys every object. Safe on NULL. */
void lagfx_a64_objects_free(lagfx_a64_objects_t *t);

/* The object for (task, ref), creating it on first reference. NULL for ref 0, an unreadable list, an empty slot, or an object that could
 * not be built. If out_entry is non-NULL it receives the 12-byte entry whenever the entry could be read. */
void *lagfx_a64_objects_get(lagfx_a64_objects_t *t, lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, uint32_t out_entry[3]);

/* Task teardown: destroys every object of the task. Returns how many nodes were dropped. */
uint32_t lagfx_a64_objects_drop_task(lagfx_a64_objects_t *t, uint32_t task_id);

void lagfx_a64_objects_stats(const lagfx_a64_objects_t *t, lagfx_a64_obj_stats_t *out);

#endif
