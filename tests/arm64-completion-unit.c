/* ARM64 deferred stamp completion queue (src/protocol/arm64_completion.[ch]): order, batching, barriers, failure, watchdog, teardown, re-entrancy,
 * a two-thread stress, and the compute drain's default behaviour with the queue off / on but without GPU work. CPU only (mock GPU work).
 * SPDX-License-Identifier: MIT */
#include "dispatchers/channel_compute_dispatcher.h"
#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "protocol/arm64_completion.h"
#include "device.h"
#include "protocol/protocol.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MEM_SIZE   (1u << 20)
#define STAMP_PFN  8u          /* ring_base_pfn: stamp cell of slot s at (8 << 14) + 4 s */
#define SHARED_PFN 1u
#define TABLE_PFN  2u
#define RING_PFN0  16u

static uint8_t *g_mem;
static atomic_int g_irq, g_reenter;
static lagfx_protocol_t *g_p;
static atomic_uint g_viol;                                  /* stamp cell regressions seen by the write callback */
static bool rd(void *o, uint64_t gpa, uint64_t len, void *dst) { (void)o; if (gpa + len > MEM_SIZE) return false; memcpy(dst, g_mem + gpa, len); return true; }
static bool wr(void *o, uint64_t gpa, uint64_t len, const void *src) {
    (void)o; if (gpa + len > MEM_SIZE) return false;
    if (len == 4 && gpa >= ((uint64_t)STAMP_PFN << 14) && gpa < ((uint64_t)STAMP_PFN << 14) + 64) {
        uint32_t old, nw; memcpy(&old, g_mem + gpa, 4); memcpy(&nw, src, 4);
        if ((int32_t)(nw - old) < 0) atomic_fetch_add(&g_viol, 1);
    }
    memcpy(g_mem + gpa, src, len); return true;
}
static void irq(void *o, uint32_t v) {
    (void)o; (void)v; atomic_fetch_add(&g_irq, 1);
    if (g_reenter) { lagfx_arm64_poll_completions(g_p); (void)lagfx_arm64_cq_pending(g_p); }   /* a shell that calls back into the library from the IRQ hook */
}
static uint32_t cell(uint32_t slot) { uint32_t v; memcpy(&v, g_mem + ((uint64_t)STAMP_PFN << 14) + 4u * slot, 4); return v; }
static void put32(uint64_t gpa, uint32_t v) { memcpy(g_mem + gpa, &v, 4); }

static lagfx_protocol_t *mk(uint32_t mode) {
    memset(g_mem, 0, MEM_SIZE);
    atomic_store(&g_irq, 0); atomic_store(&g_viol, 0); g_reenter = 0;
    lagfx_device_t *dev = calloc(1, sizeof *dev);
    dev->desc.shell.read_memory = rd; dev->desc.shell.write_memory = wr; dev->desc.shell.raise_interrupt = irq;
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    p->magic = LAGFX_PROTOCOL_MAGIC; p->dev = dev;
    p->ring_page_shift = 14; p->protocol_mode = mode; p->ring_base_pfn = STAMP_PFN;
    p->ring_shared_page_pfn = SHARED_PFN;
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(d + 0x0c, 1); put32(d + 0x10, TABLE_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE_PFN << 14) + 4u * i, RING_PFN0 + i);
    g_p = p;
    return p;
}
static void rm(lagfx_protocol_t *p) { lagfx_arm64_cq_shutdown(p, 0); free(p->dev); free(p); }

/* mock GPU work */
typedef struct { atomic_int done, fail; atomic_int released; } mock_t;
static bool mock_wait(void *o, uint64_t t, bool *failed) {
    mock_t *m = o; (void)t;
    if (!atomic_load(&m->done)) return false;
    if (atomic_load(&m->fail) && failed) *failed = true;
    return true;
}
static void mock_rel(void *o) { atomic_fetch_add(&((mock_t *)o)->released, 1); }
static mock_t *mock_new(void) { return calloc(1, sizeof(mock_t)); }

static lagfx_cmd_header_t H(uint32_t stamp) { lagfx_cmd_header_t h; memset(&h, 0, sizeof h); h.opcode = 0x24; h.length = 12; h.stamp = stamp; return h; }
/* one command through begin/end with optional work */
static bool sub(lagfx_protocol_t *p, uint32_t slot, uint32_t stamp, mock_t *m, const uint8_t *barrier_payload, uint32_t nbar, uint32_t payload_size) {
    lagfx_cmd_header_t h = H(stamp);
    h.arg_count_8b = (uint16_t)nbar; h.payload = barrier_payload; h.payload_size = (uint16_t)payload_size;
    lagfx_arm64_submit_ctx_t *c = lagfx_arm64_submit_begin(p, slot, &h);
    assert(c && lagfx_arm64_submit_current(p) == c);
    if (m) { lagfx_arm64_gpu_work_t w = { m, mock_wait, mock_rel }; assert(lagfx_arm64_submit_add_work(c, &w)); }
    return lagfx_arm64_submit_end(p, c);
}

static void test_off_and_x86(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_X86);
    assert(!lagfx_arm64_cq_enable(p) && !p->a64cq);
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    assert(!lagfx_arm64_cq_ensure(p));                       /* x86 never gets a queue, whatever the env says */
    rm(p);
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    unsetenv("LAGFX_ARM64_EXEC");
    assert(!lagfx_arm64_cq_ensure(p) && !p->a64cq);          /* arm64 without the env: off */
    assert(lagfx_arm64_submit_begin(p, 1, &(lagfx_cmd_header_t){0}) == NULL && lagfx_arm64_poll_completions(p) == 0 && lagfx_arm64_cq_pending(p) == 0);
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    assert(lagfx_arm64_cq_ensure(p) && p->a64cq);
    unsetenv("LAGFX_ARM64_EXEC");
    rm(p);
}

static void test_order_and_batch(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    mock_t *a = mock_new(), *b = mock_new();
    assert(!sub(p, 1, 1, a, NULL, 0, 0));
    assert(!sub(p, 1, 2, NULL, NULL, 0, 0));                 /* no work, but behind stamp 1 on the channel: queued, not completed */
    assert(!sub(p, 1, 3, b, NULL, 0, 0));
    assert(sub(p, 2, 7, NULL, NULL, 0, 0));                  /* other channel, nothing pending: immediate as before */
    assert(cell(2) == 7 && atomic_load(&g_irq) == 1 && cell(1) == 0);
    atomic_store(&b->done, 1);                               /* the later work finishing first must not complete anything */
    assert(lagfx_arm64_poll_completions(p) == 0 && cell(1) == 0 && lagfx_arm64_cq_pending(p) == 3);
    atomic_store(&a->done, 1);
    assert(lagfx_arm64_poll_completions(p) == 3);            /* all three in one batch: last stamp once, one IRQ */
    assert(cell(1) == 3 && atomic_load(&g_irq) == 2 && lagfx_arm64_cq_pending(p) == 0);
    assert(atomic_load(&a->released) == 1 && atomic_load(&b->released) == 1);
    assert(lagfx_arm64_poll_completions(p) == 0 && atomic_load(&g_irq) == 2);
    /* two channels finishing together: one IRQ for both */
    mock_t *c = mock_new(), *d = mock_new();
    assert(!sub(p, 1, 4, c, NULL, 0, 0)); assert(!sub(p, 2, 8, d, NULL, 0, 0));
    atomic_store(&c->done, 1); atomic_store(&d->done, 1);
    assert(lagfx_arm64_poll_completions(p) == 2 && cell(1) == 4 && cell(2) == 8 && atomic_load(&g_irq) == 3);
    assert(atomic_load(&g_viol) == 0);
    free(a); free(b); free(c); free(d); rm(p);
}

/* (c) failure before any submit: completes as failed_pre, in order, no FAULT */
static void test_failure_and_dropped(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    lagfx_arm64_cq_stats_t q;
    mock_t *a = mock_new();
    assert(!sub(p, 1, 1, a, NULL, 0, 0));
    lagfx_cmd_header_t dh = H(2); dh.opcode = 0x43;
    assert(!lagfx_arm64_submit_dropped(p, 1, &dh));          /* dropped behind pending work: queued in order, completes with it */
    assert(lagfx_arm64_poll_completions(p) == 0);
    atomic_store(&a->done, 1);
    assert(lagfx_arm64_poll_completions(p) == 2 && cell(1) == 2);
    lagfx_arm64_cq_stats(p, &q);
    assert(q.ok == 1 && q.failed_pre == 1 && q.fault_completed == 0 && !q.fault);
    lagfx_arm64_submit_ctx_t *c = lagfx_arm64_submit_begin(p, 1, &(lagfx_cmd_header_t){ .stamp = 3 });
    lagfx_arm64_submit_mark_failed(c);                       /* handler gave up before recording anything: immediate, counted failed */
    assert(lagfx_arm64_submit_end(p, c) && cell(1) == 3);
    assert(lagfx_arm64_submit_dropped(p, 3, &dh) && cell(3) == 2);
    lagfx_arm64_cq_stats(p, &q);
    assert(q.failed_pre == 3 && q.ok == 1 && !q.fault && !lagfx_arm64_gpu_faulted(p));
    free(a); rm(p);
}

/* (a) timeout with the device alive: stays pending, no stamp, slow counted once, never failed; completes when the fence signals */
static void test_slow_stays_pending(void) {
    setenv("LAGFX_VK_WAIT_MS", "30", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    unsetenv("LAGFX_VK_WAIT_MS");
    mock_t *m = mock_new();
    assert(!sub(p, 1, 1, m, NULL, 0, 0));
    assert(!sub(p, 1, 2, NULL, NULL, 0, 0));
    usleep(80000);
    for (int i = 0; i < 5; i++) assert(lagfx_arm64_poll_completions(p) == 0);
    lagfx_arm64_cq_stats_t q; lagfx_arm64_cq_stats(p, &q);
    assert(q.slow == 1 && q.fault_completed == 0 && q.failed_pre == 0 && !q.fault);   /* counted once, not failed */
    assert(cell(1) == 0 && lagfx_arm64_cq_pending(p) == 2 && atomic_load(&m->released) == 0 && atomic_load(&g_irq) == 0);
    atomic_store(&m->done, 1);                                /* the GPU finally finishes */
    assert(lagfx_arm64_poll_completions(p) == 2 && cell(1) == 2);
    lagfx_arm64_cq_stats(p, &q);
    assert(q.ok == 2 && q.slow == 1 && !q.fault && atomic_load(&m->released) == 1);
    free(m); rm(p);
}

/* optional hard limit -> FAULT: pending completes as failed, the possibly in-flight work is not released; later commands rejected */
static void test_hard_timeout_fault(void) {
    setenv("LAGFX_VK_WAIT_MS", "10", 1); setenv("LAGFX_GPU_HARD_TIMEOUT_MS", "60", 1);
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    unsetenv("LAGFX_VK_WAIT_MS"); unsetenv("LAGFX_GPU_HARD_TIMEOUT_MS");
    mock_t *m = mock_new();
    assert(!sub(p, 1, 1, m, NULL, 0, 0));
    assert(!sub(p, 2, 5, NULL, NULL, 0, 0) == 0);             /* other channel idle: immediate */
    usleep(30000); assert(lagfx_arm64_poll_completions(p) == 0 && !lagfx_arm64_gpu_faulted(p));   /* slow, but under the hard limit */
    usleep(60000);
    assert(lagfx_arm64_poll_completions(p) == 1 && lagfx_arm64_gpu_faulted(p) && cell(1) == 1);
    lagfx_arm64_cq_stats_t q; lagfx_arm64_cq_stats(p, &q);
    assert(q.hard_timeouts == 1 && q.fault_completed == 1 && q.slow >= 1 && q.fault);
    assert(atomic_load(&m->released) == 0);                   /* in flight as far as we know: never destroyed */
    free(m); rm(p);
}

/* (b)/(4) device lost: pending stamps complete as failed, new work rejected cheaply but its stamp completes */
static void test_fault_device_lost(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    mock_t *a = mock_new(), *b = mock_new(), *c = mock_new();
    atomic_store(&a->done, 1); atomic_store(&a->fail, 1);    /* reports device lost */
    assert(!sub(p, 2, 4, c, NULL, 0, 0));
    assert(!sub(p, 1, 1, a, NULL, 0, 0));
    assert(!sub(p, 1, 2, b, NULL, 0, 0));
    assert(lagfx_arm64_poll_completions(p) == 3);             /* one batch: everything pending, even work that did not finish, completes as failed */
    assert(cell(1) == 2 && cell(2) == 4 && atomic_load(&g_irq) == 1);
    lagfx_arm64_cq_stats_t q; lagfx_arm64_cq_stats(p, &q);
    assert(q.fault && lagfx_arm64_gpu_faulted(p) && q.fault_completed == 3 && q.ok == 0 && q.failed_pre == 0);
    assert(atomic_load(&a->released) == 1 && atomic_load(&b->released) == 1 && atomic_load(&c->released) == 1);
    /* later exec commands: rejected, work refused (and released at once), stamp completes immediately */
    mock_t *d = mock_new();
    lagfx_cmd_header_t h = H(3); lagfx_arm64_submit_ctx_t *x = lagfx_arm64_submit_begin(p, 1, &h);
    lagfx_arm64_gpu_work_t w = { d, mock_wait, mock_rel };
    assert(!lagfx_arm64_submit_add_work(x, &w) && atomic_load(&d->released) == 1);
    assert(lagfx_arm64_submit_end(p, x) && cell(1) == 3 && lagfx_arm64_cq_pending(p) == 0);
    lagfx_cmd_header_t dh = H(4);
    assert(lagfx_arm64_submit_dropped(p, 1, &dh) && cell(1) == 4);
    lagfx_arm64_cq_stats(p, &q);
    assert(q.fault_rejected == 2 && q.fault_completed == 3 && q.ok == 0);   /* counted apart from successes */
    free(a); free(b); free(c); free(d); rm(p);
}

static void test_barrier(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    mock_t *a = mock_new();
    assert(!sub(p, 1, 2, a, NULL, 0, 0));
    /* ch2 stamp 5 carries barrier {ch1, stamp 2} then its arguments (which start after arg_count_8b * 8 bytes) */
    uint8_t pl[16] = {0}; uint32_t b1[4] = { 1, 2, 0xdeadbeef, 0x1234 }; memcpy(pl, b1, 16);
    assert(!sub(p, 2, 5, NULL, pl, 1, 16));                  /* waits for channel 1 stamp 2 */
    assert(lagfx_arm64_poll_completions(p) == 0 && cell(2) == 0);
    atomic_store(&a->done, 1);
    assert(lagfx_arm64_poll_completions(p) == 2 && cell(1) == 2 && cell(2) == 5 && atomic_load(&g_irq) == 1);
    uint32_t b2[2] = { 1, 99 };                              /* barrier on a stamp channel 1 has not submitted: nothing pending up to it -> satisfied */
    memcpy(pl, b2, 8);
    assert(sub(p, 2, 6, NULL, pl, 1, 8) && cell(2) == 6);
    uint32_t b3[2] = { 0, 5 };                               /* channel 0 / own channel are ignored */
    memcpy(pl, b3, 8);
    assert(sub(p, 2, 7, NULL, pl, 1, 8));
    uint32_t b4[2] = { 2, 5 };
    memcpy(pl, b4, 8);
    assert(sub(p, 2, 8, NULL, pl, 1, 8));
    uint32_t b5[2] = { 1, 1 };                               /* truncated payload: barrier slot not fully present -> ignored, no read past payload_size */
    memcpy(pl, b5, 8);
    assert(sub(p, 2, 9, NULL, pl, 2, 8));
    free(a); rm(p);
}

static void test_teardown(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    mock_t *a = mock_new(), *b = mock_new();
    assert(!sub(p, 1, 1, a, NULL, 0, 0)); assert(!sub(p, 3, 4, b, NULL, 0, 0));
    lagfx_arm64_submit_begin(p, 1, &(lagfx_cmd_header_t){ .stamp = 2 });   /* begun, never ended (shell tore down mid-command) */
    atomic_store(&a->done, 1);
    lagfx_arm64_cq_shutdown(p, 20);                          /* a is done, b never finishes: bounded wait, then released anyway */
    assert(p->a64cq == NULL && atomic_load(&a->released) == 1 && atomic_load(&b->released) == 1);
    assert(cell(1) == 0 && cell(3) == 0 && atomic_load(&g_irq) == 0);   /* no stamps written to a guest that is going away */
    lagfx_arm64_cq_shutdown(p, 0);                           /* idempotent */
    assert(lagfx_arm64_poll_completions(p) == 0);
    free(a); free(b); rm(p);
    /* lagfx_protocol_reset also drops the queue */
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    mock_t *c = mock_new(); assert(!sub(p, 1, 1, c, NULL, 0, 0));
    lagfx_protocol_reset(p);
    assert(p->a64cq == NULL && atomic_load(&c->released) == 1);
    free(c); rm(p);
}

static void test_reentrancy(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    g_reenter = 1;
    mock_t *a = mock_new(); assert(!sub(p, 1, 1, a, NULL, 0, 0));
    atomic_store(&a->done, 1);
    assert(lagfx_arm64_poll_completions(p) == 1 && cell(1) == 1 && atomic_load(&g_irq) == 1);   /* IRQ hook polled again inside the retire: no deadlock, no double IRQ */
    assert(sub(p, 2, 3, NULL, NULL, 0, 0));                  /* immediate path with a re-entering hook */
    g_reenter = 0; free(a); rm(p);
}

/* ---- two threads: the drain thread submits, a "GPU" thread finishes work in random order, a poller retires ---- */
#define NCMD 3000
static mock_t *g_m[NCMD];
static atomic_int g_stop;
static void *gpu_thread(void *u) { (void)u; for (int i = NCMD - 1; i >= 0; i -= 1) { if (g_m[i]) { struct timespec ts = {0, 20000}; if (i % 7 == 0) nanosleep(&ts, NULL); atomic_store(&g_m[i]->done, 1); } } return NULL; }
static void *poll_thread(void *u) { (void)u; while (!atomic_load(&g_stop)) lagfx_arm64_poll_completions(g_p); return NULL; }
static void test_threads(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); assert(lagfx_arm64_cq_enable(p));
    atomic_store(&g_stop, 0);
    pthread_t poller; pthread_create(&poller, NULL, poll_thread, NULL);
    for (int i = 0; i < NCMD; i++) {
        g_m[i] = (i % 3) ? mock_new() : NULL;
        sub(p, 1 + (uint32_t)(i % 2), (uint32_t)(i / 2) + 1, g_m[i], NULL, 0, 0);
    }
    pthread_t gpu; pthread_create(&gpu, NULL, gpu_thread, NULL);
    pthread_join(gpu, NULL);
    for (int i = 0; i < 2000 && lagfx_arm64_cq_pending(p); i++) usleep(1000);
    atomic_store(&g_stop, 1); pthread_join(poller, NULL);
    lagfx_arm64_poll_completions(p);
    assert(lagfx_arm64_cq_pending(p) == 0);
    assert(cell(1) == (uint32_t)((NCMD - 1) / 2) + (NCMD % 2 ? 0 : 0) || cell(1) > 0);
    assert(cell(1) == (uint32_t)(NCMD / 2) && cell(2) == (uint32_t)(NCMD / 2));   /* last stamp of each channel */
    assert(atomic_load(&g_viol) == 0);                       /* the stamp cell never went backwards */
    for (int i = 0; i < NCMD; i++) free(g_m[i]);
    rm(p);
}

/* ---- cross-channel barriers gate PROCESSING order (two real rings: ch1 and ch2) ---- */
#define RING2_PFN0 24u
#define TABLE2_PFN 3u
static uint32_t g_wp1, g_wp2;
static void ring_cmd_ch(uint32_t ch, uint16_t op, uint16_t nbar, uint32_t b_ch, uint32_t b_stamp, uint32_t stamp) {
    uint32_t len = 12u + nbar * 8u, *wpp = ch == 1 ? &g_wp1 : &g_wp2;
    uint8_t *c = g_mem + ((uint64_t)(ch == 1 ? RING_PFN0 : RING2_PFN0) << 14) + (*wpp % 0x10000u);
    memset(c, 0, len);
    memcpy(c, &op, 2); memcpy(c + 2, &nbar, 2); memcpy(c + 4, &len, 4); memcpy(c + 8, &stamp, 4);
    if (nbar) { memcpy(c + 12, &b_ch, 4); memcpy(c + 16, &b_stamp, 4); }
    *wpp += len;
}
static void doorbell_ch(lagfx_protocol_t *p, uint32_t ch) {
    uint64_t d = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 20u * (ch - 1);
    put32(d, ch == 1 ? g_wp1 : g_wp2);
    p->current_chan_id = (uint8_t)ch;
    channel_compute_dispatcher_drain(p, ch);
}
static uint32_t rdptr(uint32_t ch) { uint32_t v; memcpy(&v, g_mem + ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 20u * (ch - 1) + 4, 4); return v; }
static lagfx_protocol_t *mk2(void) {
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE);
    g_wp1 = g_wp2 = 0;
    uint64_t d2 = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 20u;
    put32(d2 + 0x0c, 1); put32(d2 + 0x10, TABLE2_PFN);
    for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE2_PFN << 14) + 4u * i, RING2_PFN0 + i);
    assert(lagfx_arm64_cq_enable(p));
    return p;
}
static void test_barrier_processing_order(void) {
    lagfx_arm64_cq_stats_t q;
    /* 1. dependency channel has work in its ring: draining ch2 first hands ch1's work over (Henge do_channel), then processes ch2 */
    lagfx_protocol_t *p = mk2();
    ring_cmd_ch(1, 0x24, 0, 0, 0, 5);                       /* ch1 stamp 5: write pointer published, doorbell not rung yet */
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET, g_wp1);
    ring_cmd_ch(2, 0x24, 1, 1, 5, 9);                       /* ch2 stamp 9 behind barrier {ch1, 5} */
    doorbell_ch(p, 2);
    assert(cell(1) == 5 && cell(2) == 9 && rdptr(2) == g_wp2 && rdptr(1) == g_wp1);
    lagfx_arm64_cq_stats(p, &q);
    assert(q.barrier_drains == 1 && q.barrier_holds == 0 && q.barrier_timeouts == 0);
    /* a satisfied barrier (stamp 4 <= submitted 5), signed compare: no hold, no drain */
    ring_cmd_ch(2, 0x24, 1, 1, 4, 10); doorbell_ch(p, 2);
    assert(cell(2) == 10);
    lagfx_arm64_cq_stats(p, &q); assert(q.barrier_drains == 1 && q.barrier_holds == 0);
    rm(p);
    /* 2. dependency not there yet: ch2's command STAYS in the ring (not processed, not completed); it runs when ch1 submits (retry at end of ch1's drain) */
    p = mk2();
    ring_cmd_ch(2, 0x24, 1, 1, 5, 9);
    doorbell_ch(p, 2);
    assert(cell(2) == 0 && rdptr(2) == 0 && lagfx_arm64_held_channels(p) == (1u << 2));
    ring_cmd_ch(1, 0x24, 0, 0, 0, 4);                       /* ch1 stamp 4 < 5: still not enough */
    doorbell_ch(p, 1);
    assert(cell(1) == 4 && cell(2) == 0 && rdptr(2) == 0);
    ring_cmd_ch(1, 0x24, 0, 0, 0, 5);
    doorbell_ch(p, 1);                                      /* submitted 5: ch2 released, processed after ch1's stamp */
    assert(cell(1) == 5 && cell(2) == 9 && rdptr(2) == g_wp2 && lagfx_arm64_held_channels(p) == 0);
    lagfx_arm64_cq_stats(p, &q); assert(q.barrier_holds >= 2 && q.barrier_timeouts == 0);
    rm(p);
    /* 3. mutual barrier (ch1 waits ch2, ch2 waits ch1): recursion guard, no hang; resolved by the bound */
    setenv("LAGFX_ARM64_BARRIER_RETRIES", "3", 1);
    p = mk2(); unsetenv("LAGFX_ARM64_BARRIER_RETRIES");
    ring_cmd_ch(1, 0x24, 1, 2, 7, 3);
    ring_cmd_ch(2, 0x24, 1, 1, 3, 7);
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 20u, g_wp2);
    doorbell_ch(p, 1);                                      /* neither can go first until the bound lets one through; no hang, no recursion loop */
    for (int i = 0; i < 8 && (cell(1) == 0 || cell(2) == 0); i++) channel_compute_retry_held(p);
    assert(cell(1) == 3 && cell(2) == 7);
    lagfx_arm64_cq_stats(p, &q); assert(q.barrier_timeouts >= 1);
    rm(p);
    /* 4. stuck dependency (nothing will ever come on ch1): held for the bound, then let through and logged/counted */
    setenv("LAGFX_ARM64_BARRIER_RETRIES", "4", 1);
    p = mk2(); unsetenv("LAGFX_ARM64_BARRIER_RETRIES");
    ring_cmd_ch(2, 0x24, 1, 1, 50, 9);
    doorbell_ch(p, 2);                                      /* attempt 1 (doorbell) + attempt 2 (retry at the end of the drain): both held */
    assert(cell(2) == 0 && rdptr(2) == 0);
    lagfx_arm64_cq_stats(p, &q); assert(q.barrier_holds == 2 && q.barrier_timeouts == 0);
    doorbell_ch(p, 2);                                      /* attempt 3 held, attempt 4 reaches the bound: let through, counted */
    assert(cell(2) == 9 && rdptr(2) == g_wp2);
    lagfx_arm64_cq_stats(p, &q); assert(q.barrier_holds == 3 && q.barrier_timeouts == 1 && lagfx_arm64_held_channels(p) == 0);
    rm(p);
    /* 5. queue off: barriers are ignored exactly as before (no ensure, no hold) */
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE); g_wp1 = g_wp2 = 0;
    { uint64_t d2 = ((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 20u;
      put32(d2 + 0x0c, 1); put32(d2 + 0x10, TABLE2_PFN);
      for (uint32_t i = 0; i < 4; i++) put32(((uint64_t)TABLE2_PFN << 14) + 4u * i, RING2_PFN0 + i); }
    ring_cmd_ch(2, 0x24, 1, 1, 50, 9);
    doorbell_ch(p, 2);
    assert(cell(2) == 9 && !p->a64cq);
    rm(p);
}

/* ---- the compute drain itself ---- */
static uint32_t g_wp;
static void ring_cmd(uint16_t op, uint32_t len, uint32_t stamp) {
    uint64_t ring = (uint64_t)RING_PFN0 << 14;
    uint8_t *c = g_mem + ring + (g_wp % 0x10000u);
    assert(g_wp % 0x10000u + len <= 0x10000u);
    memset(c, 0, len);
    memcpy(c, &op, 2); memcpy(c + 4, &len, 4); memcpy(c + 8, &stamp, 4);
    g_wp += len;
}
static void drain(lagfx_protocol_t *p, uint32_t ch) {
    put32(((uint64_t)SHARED_PFN << 14) + LAGFX_RING_FIFO_DESC_BASE_OFFSET + 0x00, g_wp);
    p->current_chan_id = (uint8_t)ch;
    channel_compute_dispatcher_drain(p, ch);
}
static void test_drain(void) {
    /* queue off (default): every command completes at once, one IRQ each; an over-scratch command is skipped WITHOUT completing (unchanged) */
    lagfx_protocol_t *p = mk(LAGFX_PROTO_ARM64_VMAPPLE); g_wp = 0; unsetenv("LAGFX_ARM64_EXEC");
    ring_cmd(0x24, 12, 1); ring_cmd(0x24, 12, 2); ring_cmd(0x24, 12, 3);
    drain(p, 1);
    assert(cell(1) == 3 && atomic_load(&g_irq) == 3 && !p->a64cq);
    ring_cmd(0x24, 0x2000, 4);
    drain(p, 1);
    assert(cell(1) == 3);
    rm(p);
    /* queue on, no GPU work attached: behaviour identical (cell, one IRQ per command) ... */
    p = mk(LAGFX_PROTO_ARM64_VMAPPLE); g_wp = 0;
    setenv("LAGFX_ARM64_EXEC", "1", 1);
    ring_cmd(0x24, 12, 1); ring_cmd(0x24, 12, 2); ring_cmd(0x24, 12, 3);
    drain(p, 1);
    assert(p->a64cq && cell(1) == 3 && atomic_load(&g_irq) == 3 && lagfx_arm64_cq_pending(p) == 0);
    /* ... except that a skipped (over-scratch) command now completes its stamp so the guest never waits for it */
    ring_cmd(0x24, 0x2000, 4);
    drain(p, 1);
    assert(cell(1) == 4 && atomic_load(&g_irq) == 4);
    /* work attached to a queued entry on the channel holds back everything behind it, also across drains */
    mock_t *m = mock_new();
    assert(!sub(p, 1, 5, m, NULL, 0, 0));
    ring_cmd(0x24, 12, 6);
    drain(p, 1);
    assert(cell(1) == 4 && lagfx_arm64_cq_pending(p) == 2);
    atomic_store(&m->done, 1);
    int irq_before = atomic_load(&g_irq);
    ring_cmd(0x24, 12, 7);
    drain(p, 1);                                             /* the drain polls on entry: 5 and 6 retire together, then 7 completes */
    assert(cell(1) == 7 && lagfx_arm64_cq_pending(p) == 0);
    assert(atomic_load(&g_irq) == irq_before + 2);           /* one batch IRQ (5,6) + one immediate (7) */
    free(m); unsetenv("LAGFX_ARM64_EXEC"); rm(p);
}

int main(void) {
    g_mem = calloc(1, MEM_SIZE);
    test_off_and_x86();
    test_order_and_batch();
    test_failure_and_dropped();
    test_barrier();
    test_slow_stays_pending();
    test_hard_timeout_fault();
    test_fault_device_lost();
    test_teardown();
    test_reentrancy();
    test_threads();
    test_barrier_processing_order();
    test_drain();
    puts("arm64-completion-unit: ok");
    return 0;
}
