/*
 * libapplegfx-vulkan — ARM64 deferred stamp completion (GPU work -> stamp cell + IRQ)
 * src/protocol/arm64_completion.h
 *
 * protocol_mode LAGFX_PROTO_ARM64_VMAPPLE only. x86 never touches this.
 *
 * Today the compute drain completes a command's stamp cell and raises the IRQ as soon as the command was dispatched. Once the Render/Blit
 * handlers record real GPU work, the guest may only see the stamp after that work finished. This header is the interface for that:
 *
 *   ctx = lagfx_arm64_submit_begin(p, slot, hdr);       drain loop, before dispatching a command (NULL when the feature is off)
 *   ...handler: lagfx_arm64_submit_current(p) -> ctx;   lagfx_arm64_submit_add_vk_fence(ctx, vk, fence, owns) / _add_work(ctx, &w)
 *   lagfx_arm64_submit_end(p, ctx);                     drain loop, after dispatching: completes now, or queues behind the GPU work
 *   lagfx_arm64_poll_completions(p);                    retires finished entries in per-channel submission order, one IRQ per batch
 *
 * Off by default: the queue exists only when env LAGFX_ARM64_EXEC=1 (or a test calls lagfx_arm64_cq_enable). With no queue every function is a
 * NULL-safe no-op and the drain completes the stamp itself exactly as before. With the queue on but no work attached and nothing pending on
 * the channel, end() still completes immediately (stamp cell + IRQ per command, as today).
 *
 * Semantics
 *  - Order: stamps of one channel (slot) are retired strictly in submission order. A command without work queued behind one with work waits.
 *  - Barriers: the command's `arg_count_8b` leading 8-byte slots are {channel, stamp} pairs (Henge pvg_lite.c). The command's arguments start at
 *    payload + arg_count_8b*8. A barrier makes the command's completion wait until channel's queued entries up to that stamp are retired (a
 *    barrier on a channel with nothing pending up to that stamp is satisfied). Barriers gate COMPLETION only; GPU execution order is the
 *    recorder's business (one graphics queue = submission order).
 *  - Failure: mark_failed() / a fence that reports an error / a watchdog timeout (LAGFX_ARM64_EXEC_TIMEOUT_MS, default 2000, also frees a barrier
 *    cycle) never withhold the stamp: failed entries complete like the others (and are counted), so the guest never hangs.
 *  - Batch: one retire pass writes, per channel, the highest retired stamp once, then raises the IRQ once.
 *  - Teardown / reset: lagfx_arm64_cq_shutdown waits (bounded) for outstanding work, releases it and writes NO stamps (the guest is going away).
 *
 * Threading (read before changing)
 *  - The queue is guarded by ONE recursive mutex (cq->lock). Every public function except begin, current, the add_ functions and mark_failed (which only touch the
 *    ctx owned by the one thread dispatching that command) takes it. Callbacks into the shell (write_memory, raise_interrupt) and the work's wait()
 *    are made WITH the lock held: retire order and stamp-cell monotonicity depend on that. The lock is a leaf with respect to the library: the
 *    code under it takes no other library lock (display rt_lock, ...), and nothing that holds rt_lock calls into it. A shell callback that re-enters
 *    the library (doorbell read -> poll, drain -> end) on the same thread is safe (recursive lock + in_retire guard); another thread waits.
 *  - No worker thread: shell callbacks such as raise_interrupt need the shell's own context (QEMU BQL). poll() is therefore called on the
 *    thread the shell already uses for the library: at the end of every compute drain, on guest reads of BAR0+0x1018 / 0x100c, and by the shell
 *    via lagfx_device_poll_completions() from a timer while lagfx_device_pending_completions() > 0. try-lock semantics: if another thread is
 *    inside the queue, poll returns 0 and the next tick retires.
 */
#ifndef LAGFX_PROTOCOL_ARM64_COMPLETION_H
#define LAGFX_PROTOCOL_ARM64_COMPLETION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"
#include "opcodes.h"

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

struct lagfx_vk_state;
typedef struct lagfx_arm64_submit_ctx lagfx_arm64_submit_ctx_t;

#define LAGFX_A64CQ_MAX_WORK     8u    /* GPU work items per command */
#define LAGFX_A64CQ_MAX_BARRIERS 4u    /* barrier slots honoured per command */

/* One piece of GPU work the completion waits for. */
typedef struct {
    void *opaque;
    /* Is the work finished? timeout_ns == 0 polls; >0 may block up to that long. Returns true when finished. *failed is set (optional out) if
     * the work finished abnormally (device lost...). Called only with the queue lock held. */
    bool (*wait)(void *opaque, uint64_t timeout_ns, bool *failed);
    /* Called exactly once, when the work is finished, abandoned or at teardown. May be NULL. */
    void (*release)(void *opaque);
} lagfx_arm64_gpu_work_t;

/* Enable the queue for this protocol (arm64 only; returns false otherwise). Normally done lazily from env LAGFX_ARM64_EXEC=1 by
 * lagfx_arm64_cq_ensure(); tests call this directly. */
bool lagfx_arm64_cq_enable(lagfx_protocol_t *p);
bool lagfx_arm64_cq_ensure(lagfx_protocol_t *p);   /* enable from env once; true if the queue exists */

/* NULL when the queue is off (every function below accepts NULL ctx and does nothing). Reads the barrier slots out of hdr. */
lagfx_arm64_submit_ctx_t *lagfx_arm64_submit_begin(lagfx_protocol_t *p, uint32_t slot, const lagfx_cmd_header_t *hdr);
/* The ctx of the command being dispatched right now (NULL outside begin..end or when off). */
lagfx_arm64_submit_ctx_t *lagfx_arm64_submit_current(lagfx_protocol_t *p);
/* Attach GPU work. Returns false (and releases the work immediately) if the ctx is NULL or full: the caller must then have waited itself. */
bool lagfx_arm64_submit_add_work(lagfx_arm64_submit_ctx_t *ctx, const lagfx_arm64_gpu_work_t *w);
void lagfx_arm64_submit_mark_failed(lagfx_arm64_submit_ctx_t *ctx);
#ifdef LAGFX_HAVE_VULKAN
/* Convenience: a VkFence submitted with the work (owns => destroyed on release) or a timeline semaphore reaching `value` (never destroyed). */
bool lagfx_arm64_submit_add_vk_fence(lagfx_arm64_submit_ctx_t *ctx, struct lagfx_vk_state *vk, VkFence fence, bool owns);
bool lagfx_arm64_submit_add_vk_timeline(lagfx_arm64_submit_ctx_t *ctx, struct lagfx_vk_state *vk, VkSemaphore sem, uint64_t value);
#endif
/* Ends the command. Returns true if its stamp was completed right now (cell written + IRQ raised, as before), false if queued (or p/ctx NULL
 * with nothing done: the caller then completes itself). ctx is consumed. */
bool lagfx_arm64_submit_end(lagfx_protocol_t *p, lagfx_arm64_submit_ctx_t *ctx);
/* Convenience for the drain: a command that was dropped before dispatch still completes its stamp (queued behind earlier entries). */
bool lagfx_arm64_submit_dropped(lagfx_protocol_t *p, uint32_t slot, const lagfx_cmd_header_t *hdr);

/* Retire every finished entry in per-channel order. Returns the number of stamps retired (>=0); at most one IRQ is raised for the batch. */
size_t lagfx_arm64_poll_completions(lagfx_protocol_t *p);
size_t lagfx_arm64_cq_pending(lagfx_protocol_t *p);
/* Counters (tests / diagnostics): retired, failed (incl. timed out), timed_out. */
void lagfx_arm64_cq_stats(lagfx_protocol_t *p, uint64_t *retired, uint64_t *failed, uint64_t *timed_out);
/* Wait up to timeout_ms for outstanding work, release everything, free the queue. No stamps are written. Safe on NULL / queue off. */
void lagfx_arm64_cq_shutdown(lagfx_protocol_t *p, uint32_t timeout_ms);

#endif
