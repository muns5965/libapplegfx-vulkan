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
 *  - Barriers (processing AND completion; see lagfx_arm64_barrier_gate below): the command's `arg_count_8b` leading 8-byte slots are {channel, stamp} pairs (Henge pvg_lite.c). The command's arguments start at
 *    payload + arg_count_8b*8. A barrier makes the command's completion wait until channel's queued entries up to that stamp are retired (a
 *    barrier on a channel with nothing pending up to that stamp is satisfied). Barriers gate COMPLETION only; GPU execution order is the
 *    recorder's business (one graphics queue = submission order).
 *  - Timeouts are NOT failures. After LAGFX_VK_WAIT_MS (default 2000, an initial test value) without the fence signalling, the entry is counted
 *    "slow" (logged once) and stays PENDING: no stamp is written, because the GPU may still be using memory the guest would then free or reuse.
 *    Nothing blocks on it; polls just re-check. Optional hard limit LAGFX_GPU_HARD_TIMEOUT_MS (default 0 = off) puts the GPU into FAULT.
 *  - FAULT (a work item reports failure, e.g. VK_ERROR_DEVICE_LOST, or the hard limit): nothing can still touch guest memory, so every pending
 *    stamp completes as failed (fault_completed), and later commands are rejected cheaply (begin marks the ctx rejected, add_work refuses) while
 *    their stamps still complete (fault_rejected). FAULT lasts until the queue is shut down / the protocol reset. On a hard timeout the possibly
 *    in-flight fence is leaked, never destroyed.
 *  - Failure before any submit (mark_failed, dropped command): completes as failed (failed_pre), in order, like Apple's.
 *  - Batch: one retire pass writes, per channel, the highest retired stamp once, then raises the IRQ once.
 *  - Teardown / reset: lagfx_arm64_cq_shutdown waits (bounded) for outstanding work, releases it and writes NO stamps (the guest is going away).
 *
 * Threading (read before changing)
 *  - The queue is guarded by ONE recursive mutex (cq->lock). Every public function except begin, current, the add_ functions and mark_failed (which only touch the
 *    ctx owned by the one thread dispatching that command) takes it. Callbacks into the shell (write_memory, raise_interrupt) and the work's wait()
 *    are made WITH the lock held: retire order and stamp-cell monotonicity depend on that. The lock is a leaf with respect to the library: the
 *    code under it takes no other library lock (display rt_lock, ...), and nothing that holds rt_lock calls into it. A shell callback that re-enters
 *    the library (doorbell read -> poll, drain -> end) on the same thread is safe (recursive lock + in_retire guard); another thread waits.
 *  - No worker thread. Henge's pvg_lite.c (hv_pvg_lite_complete) completes from the Metal completion thread with no device lock: an atomic release
 *    store of the stamp, an atomic OR of the status bit, hv_gicv3_pend (atomic) and a machine wake; so a shell modelled on it may raise the IRQ from
 *    any thread. That is a property of the SHELL's write_memory / raise_interrupt: if they need a lock (QEMU: BQL for msix_notify) poll must run
 *    only where the shell already holds it. Library side: the queue mutex is the only lock, p->diag2.irq_raised / last_completed_stamp are plain
 *    counters (diagnostics only, may race). poll() is therefore called on the
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

/* Barriers gate PROCESSING order across channels (Henge pvg_lite.c ~930-960; all work goes through one in-order worker, so everything up to the
 * stamp must be handed over first or a frame is shown half drawn). `slots` = the command's first n barrier slots {channel, stamp} read from the ring
 * (n = min(arg_count_8b, LAGFX_A64CQ_MAX_BARRIERS)). A channel's "submitted stamp" is the last stamp it dispatched (set by submit_end). The compute
 * drain: (1) blocker() names a channel that has not submitted up to the stamp; the drain first drains that channel (guarded against recursion with
 * chan_enter/leave), (2) gate() decides: GO, HOLD (command stays in the ring; the channel is added to held_channels, retried at the end of any drain
 * and by lagfx_device_poll_completions) or LET_THROUGH after LAGFX_ARM64_BARRIER_RETRIES (default 20000, as Henge) holds, logged and counted. */
typedef enum { LAGFX_BARRIER_GO = 0, LAGFX_BARRIER_HOLD, LAGFX_BARRIER_LET_THROUGH } lagfx_barrier_verdict_t;
uint32_t lagfx_arm64_barrier_blocker(lagfx_protocol_t *p, uint32_t slot, const uint8_t *slots, uint32_t n);   /* 0 = none */
lagfx_barrier_verdict_t lagfx_arm64_barrier_gate(lagfx_protocol_t *p, uint32_t slot, const uint8_t *slots, uint32_t n);
void lagfx_arm64_barrier_note_drain(lagfx_protocol_t *p);
uint32_t lagfx_arm64_held_channels(lagfx_protocol_t *p);
bool lagfx_arm64_chan_enter(lagfx_protocol_t *p, uint32_t ch);
void lagfx_arm64_chan_leave(lagfx_protocol_t *p, uint32_t ch);

/* Retire every finished entry in per-channel order. Returns the number of stamps retired (>=0); at most one IRQ is raised for the batch. */
size_t lagfx_arm64_poll_completions(lagfx_protocol_t *p);
size_t lagfx_arm64_cq_pending(lagfx_protocol_t *p);
/* Counters (tests / diagnostics): retired, failed (incl. timed out), timed_out. */
typedef struct {
    uint64_t ok;              /* completed after the GPU work (or with none) finished */
    uint64_t failed_pre;      /* failed before any GPU submit (decode / dropped / mark_failed): stamp completed so the guest does not hang */
    uint64_t fault_completed; /* pending when the GPU entered FAULT: completed as failed */
    uint64_t fault_rejected;  /* begun while FAULTed: rejected cheaply, stamp completed */
    uint64_t slow;            /* entries that passed the soft bound (kept pending, NOT failed) */
    uint64_t hard_timeouts;   /* entries that hit LAGFX_GPU_HARD_TIMEOUT_MS (-> FAULT) */
    uint64_t barrier_holds, barrier_drains, barrier_timeouts;   /* processing-order barriers: held, dependency channel drained first, let through after the bound */
    bool fault;
} lagfx_arm64_cq_stats_t;
void lagfx_arm64_cq_stats(lagfx_protocol_t *p, lagfx_arm64_cq_stats_t *out);
/* True once the GPU is in FAULT: handlers should not record/submit (begin() already marks the ctx rejected; add_work then releases and returns false). */
bool lagfx_arm64_gpu_faulted(lagfx_protocol_t *p);
/* Wait up to timeout_ms for outstanding work, release everything, free the queue. No stamps are written. Safe on NULL / queue off. */
void lagfx_arm64_cq_shutdown(lagfx_protocol_t *p, uint32_t timeout_ms);

#endif
