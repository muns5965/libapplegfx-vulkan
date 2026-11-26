/*
 * libapplegfx-vulkan — ARM64 deferred stamp completion queue. See arm64_completion.h for the contract and the threading rules.
 * SPDX-License-Identifier: MIT
 */
#include "arm64_completion.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"
#include "../vulkan/instance.h"
#include "../vulkan/command.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CQ_SLOTS   LAGFX_MAX_CHANNELS
#define CQ_ZOMBIES 64u

typedef struct { bool done; lagfx_arm64_gpu_work_t w; } cq_work_t;

struct lagfx_arm64_submit_ctx {
    struct lagfx_arm64_submit_ctx *next;
    uint32_t slot, stamp;
    uint32_t nwork, nbar;
    bool failed;
    uint64_t t0_ns, t_head;   /* t_head: when the entry first reached the head of its channel (the watchdog runs from there) */
    cq_work_t work[LAGFX_A64CQ_MAX_WORK];
    struct { uint32_t slot, stamp; } bar[LAGFX_A64CQ_MAX_BARRIERS];
};

struct lagfx_arm64_cq {
    pthread_mutex_t lock;                 /* recursive; guards everything below */
    bool in_retire;                       /* re-entrancy guard (shell callback -> poll) */
    struct lagfx_arm64_submit_ctx *cur;   /* command being dispatched (drain thread only) */
    struct lagfx_arm64_submit_ctx *head[CQ_SLOTS], *tail[CQ_SLOTS];
    size_t pending;
    cq_work_t zombie[CQ_ZOMBIES];         /* work of timed-out entries that the GPU has not finished yet: cannot be destroyed while in flight */
    uint64_t timeout_ns;
    uint64_t retired, failed, timed_out;
};

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool lagfx_arm64_cq_enable(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p) || !lagfx_proto_arm64(p)) return false;
    if (p->a64cq) return true;
    struct lagfx_arm64_cq *cq = calloc(1, sizeof *cq);
    if (!cq) return false;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cq->lock, &a); pthread_mutexattr_destroy(&a);
    const char *t = getenv("LAGFX_ARM64_EXEC_TIMEOUT_MS");
    unsigned long ms = t ? strtoul(t, NULL, 10) : 0;
    cq->timeout_ns = (ms ? ms : 2000ul) * 1000000ull;
    p->a64cq = cq;
    LAGFX_LOG("arm64 completion queue enabled (timeout %lu ms)", ms ? ms : 2000ul);
    return true;
}

bool lagfx_arm64_cq_ensure(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p) || !lagfx_proto_arm64(p)) return false;
    if (p->a64cq) return true;
    const char *e = getenv("LAGFX_ARM64_EXEC");
    if (!e || e[0] != '1') return false;
    return lagfx_arm64_cq_enable(p);
}

static void work_release(cq_work_t *w) {
    if (w->w.release) w->w.release(w->w.opaque);
    memset(w, 0, sizeof *w);
}

lagfx_arm64_submit_ctx_t *lagfx_arm64_submit_begin(lagfx_protocol_t *p, uint32_t slot, const lagfx_cmd_header_t *hdr) {
    if (!lagfx_protocol_is_valid(p) || !p->a64cq || !hdr) return NULL;
    struct lagfx_arm64_cq *cq = p->a64cq;
    lagfx_arm64_submit_ctx_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->slot = slot; c->stamp = hdr->stamp; c->t0_ns = now_ns();
    /* barrier slots: {channel, stamp} * arg_count_8b, before the arguments (Henge pvg_lite.c) */
    if (hdr->payload) {
        uint32_t n = hdr->arg_count_8b;
        for (uint32_t k = 0; k < n && (k + 1u) * 8u <= hdr->payload_size; k++) {
            uint32_t bc = lagfx_le32(hdr->payload + 8u * k), bs = lagfx_le32(hdr->payload + 8u * k + 4u);
            if (bc == 0 || bc >= CQ_SLOTS || bc == slot) continue;   /* Henge: only another non-root channel is waited for */
            if (c->nbar < LAGFX_A64CQ_MAX_BARRIERS) { c->bar[c->nbar].slot = bc; c->bar[c->nbar].stamp = bs; c->nbar++; }
        }
    }
    pthread_mutex_lock(&cq->lock);
    cq->cur = c;
    pthread_mutex_unlock(&cq->lock);
    return c;
}

lagfx_arm64_submit_ctx_t *lagfx_arm64_submit_current(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p) || !p->a64cq) return NULL;
    return p->a64cq->cur;
}

bool lagfx_arm64_submit_add_work(lagfx_arm64_submit_ctx_t *ctx, const lagfx_arm64_gpu_work_t *w) {
    if (!w) return false;
    if (!ctx || ctx->nwork >= LAGFX_A64CQ_MAX_WORK || !w->wait) {
        if (w->release) w->release(w->opaque);
        return false;
    }
    ctx->work[ctx->nwork].done = false;
    ctx->work[ctx->nwork].w = *w;
    ctx->nwork++;
    return true;
}

void lagfx_arm64_submit_mark_failed(lagfx_arm64_submit_ctx_t *ctx) { if (ctx) ctx->failed = true; }

#ifdef LAGFX_HAVE_VULKAN
typedef struct { struct lagfx_vk_state *vk; VkFence fence; bool owns; VkSemaphore sem; uint64_t value; } vkw_t;

static bool vkw_fence_wait(void *o, uint64_t timeout_ns, bool *failed) {
    vkw_t *w = o;
    VkResult r = timeout_ns ? vkWaitForFences(w->vk->device, 1, &w->fence, VK_TRUE, timeout_ns) : vkGetFenceStatus(w->vk->device, w->fence);
    if (r == VK_SUCCESS) return true;
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return false;
    if (failed) *failed = true;
    return true;
}
static void vkw_fence_release(void *o) {
    vkw_t *w = o;
    if (w->owns && w->fence != VK_NULL_HANDLE) vkDestroyFence(w->vk->device, w->fence, NULL);
    free(w);
}
static bool vkw_sem_wait(void *o, uint64_t timeout_ns, bool *failed) {
    vkw_t *w = o;
    VkResult r;
    if (timeout_ns) {
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &w->sem, .pValues = &w->value };
        r = vkWaitSemaphores(w->vk->device, &wi, timeout_ns);
    } else {
        uint64_t v = 0;
        r = vkGetSemaphoreCounterValue(w->vk->device, w->sem, &v);
        if (r == VK_SUCCESS) r = v >= w->value ? VK_SUCCESS : VK_NOT_READY;
    }
    if (r == VK_SUCCESS) return true;
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return false;
    if (failed) *failed = true;
    return true;
}
static void vkw_sem_release(void *o) { free(o); }

bool lagfx_arm64_submit_add_vk_fence(lagfx_arm64_submit_ctx_t *ctx, struct lagfx_vk_state *vk, VkFence fence, bool owns) {
    if (!vk || fence == VK_NULL_HANDLE) return false;
    vkw_t *w = calloc(1, sizeof *w);
    if (!w) return false;
    w->vk = vk; w->fence = fence; w->owns = owns;
    lagfx_arm64_gpu_work_t gw = { w, vkw_fence_wait, vkw_fence_release };
    return lagfx_arm64_submit_add_work(ctx, &gw);
}
bool lagfx_arm64_submit_add_vk_timeline(lagfx_arm64_submit_ctx_t *ctx, struct lagfx_vk_state *vk, VkSemaphore sem, uint64_t value) {
    if (!vk || sem == VK_NULL_HANDLE || !vk->have_timeline_semaphore) return false;
    vkw_t *w = calloc(1, sizeof *w);
    if (!w) return false;
    w->vk = vk; w->sem = sem; w->value = value;
    lagfx_arm64_gpu_work_t gw = { w, vkw_sem_wait, vkw_sem_release };
    return lagfx_arm64_submit_add_work(ctx, &gw);
}
#endif

static void q_push(struct lagfx_arm64_cq *cq, lagfx_arm64_submit_ctx_t *c) {
    c->next = NULL;
    if (cq->tail[c->slot]) cq->tail[c->slot]->next = c; else cq->head[c->slot] = c;
    cq->tail[c->slot] = c;
    cq->pending++;
}

/* A barrier {ch, st} is satisfied unless channel ch still has a queued entry with stamp <= st (the queue is in submission order, so the head says). */
static bool barriers_ok(struct lagfx_arm64_cq *cq, const lagfx_arm64_submit_ctx_t *c) {
    for (uint32_t k = 0; k < c->nbar; k++) {
        const lagfx_arm64_submit_ctx_t *h = cq->head[c->bar[k].slot];
        if (h && (int32_t)(h->stamp - c->bar[k].stamp) <= 0) return false;
    }
    return true;
}

static void retire_zombies(struct lagfx_arm64_cq *cq) {
    for (uint32_t i = 0; i < CQ_ZOMBIES; i++) {
        cq_work_t *z = &cq->zombie[i];
        if (!z->w.wait) continue;
        bool f = false;
        if (z->w.wait(z->w.opaque, 0, &f)) work_release(z);
    }
}

static void to_zombie(struct lagfx_arm64_cq *cq, cq_work_t *w) {
    for (uint32_t i = 0; i < CQ_ZOMBIES; i++)
        if (!cq->zombie[i].w.wait) { cq->zombie[i] = *w; memset(w, 0, sizeof *w); return; }
    LAGFX_WARN("arm64 cq: zombie table full; leaking in-flight GPU work");
    memset(w, 0, sizeof *w);
}

/* true when the entry may complete now. `timed` is set when it was forced by the watchdog. */
static bool entry_ready(struct lagfx_arm64_cq *cq, lagfx_arm64_submit_ctx_t *c, uint64_t now, bool *timed) {
    bool all = true;
    for (uint32_t i = 0; i < c->nwork; i++) {
        cq_work_t *w = &c->work[i];
        if (!w->w.wait) continue;          /* already released */
        bool f = false;
        if (w->w.wait(w->w.opaque, 0, &f)) { if (f) c->failed = true; work_release(w); }
        else all = false;
    }
    if (all && barriers_ok(cq, c)) return true;
    if (!c->t_head) c->t_head = now;
    if (now - c->t_head >= cq->timeout_ns) {
        LAGFX_WARN("arm64 cq: slot %u stamp 0x%x forced after %llu ms (%s)", c->slot, c->stamp, (unsigned long long)((now - c->t_head) / 1000000ull),
                   all ? "barrier never satisfied" : "GPU work unfinished");
        for (uint32_t i = 0; i < c->nwork; i++) if (c->work[i].w.wait) to_zombie(cq, &c->work[i]);
        c->failed = true; *timed = true;
#ifdef LAGFX_HAVE_VULKAN
        lagfx_vk_note_gpu_failure(true);
#endif
        return true;
    }
    return false;
}

static size_t retire_locked(lagfx_protocol_t *p, struct lagfx_arm64_cq *cq) {
    if (cq->in_retire) return 0;
    cq->in_retire = true;
    retire_zombies(cq);
    uint32_t last[CQ_SLOTS]; bool has[CQ_SLOTS]; memset(has, 0, sizeof has);
    size_t n = 0;
    uint64_t now = now_ns();
    bool progress = true;
    while (progress) {
        progress = false;
        for (uint32_t s = 0; s < CQ_SLOTS; s++) {
            for (lagfx_arm64_submit_ctx_t *c; (c = cq->head[s]) != NULL; ) {
                bool timed = false;
                if (!entry_ready(cq, c, now, &timed)) break;
                cq->head[s] = c->next; if (!c->next) cq->tail[s] = NULL;
                cq->pending--; cq->retired++; if (c->failed) cq->failed++; if (timed) cq->timed_out++;
                last[s] = c->stamp; has[s] = true; n++; progress = true;
                free(c);
            }
        }
    }
    for (uint32_t s = 0; s < CQ_SLOTS; s++)
        if (has[s]) lagfx_protocol_complete_stamp_slot_noirq(p, s, last[s]);
    if (n) lagfx_protocol_raise_completion_irq(p);   /* once per batch */
    cq->in_retire = false;
    return n;
}

bool lagfx_arm64_submit_end(lagfx_protocol_t *p, lagfx_arm64_submit_ctx_t *c) {
    if (!lagfx_protocol_is_valid(p) || !p->a64cq || !c) return false;
    struct lagfx_arm64_cq *cq = p->a64cq;
    pthread_mutex_lock(&cq->lock);
    if (cq->cur == c) cq->cur = NULL;
    bool immediate = c->slot >= CQ_SLOTS ||
                     (c->nwork == 0 && !cq->head[c->slot] && barriers_ok(cq, c));
    if (immediate) {
        if (c->failed) cq->failed++;
        cq->retired++;
        lagfx_protocol_complete_stamp_slot(p, c->slot, c->stamp);   /* exactly the pre-queue behaviour */
        free(c);
    } else {
        q_push(cq, c);
    }
    pthread_mutex_unlock(&cq->lock);
    return immediate;
}

bool lagfx_arm64_submit_dropped(lagfx_protocol_t *p, uint32_t slot, const lagfx_cmd_header_t *hdr) {
    lagfx_arm64_submit_ctx_t *c = lagfx_arm64_submit_begin(p, slot, hdr);
    if (!c) return false;
    c->failed = true;
    c->nbar = 0;   /* a dropped command was not executed: nothing to order against, only the channel's earlier completions */
    return lagfx_arm64_submit_end(p, c);
}

size_t lagfx_arm64_poll_completions(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p) || !p->a64cq) return 0;
    struct lagfx_arm64_cq *cq = p->a64cq;
    if (pthread_mutex_trylock(&cq->lock) != 0) return 0;
    size_t n = cq->pending || cq->zombie[0].w.wait ? retire_locked(p, cq) : 0;
    pthread_mutex_unlock(&cq->lock);
    return n;
}

size_t lagfx_arm64_cq_pending(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p) || !p->a64cq) return 0;
    pthread_mutex_lock(&p->a64cq->lock);
    size_t n = p->a64cq->pending;
    pthread_mutex_unlock(&p->a64cq->lock);
    return n;
}

void lagfx_arm64_cq_stats(lagfx_protocol_t *p, uint64_t *retired, uint64_t *failed, uint64_t *timed_out) {
    uint64_t r = 0, f = 0, t = 0;
    if (lagfx_protocol_is_valid(p) && p->a64cq) {
        pthread_mutex_lock(&p->a64cq->lock);
        r = p->a64cq->retired; f = p->a64cq->failed; t = p->a64cq->timed_out;
        pthread_mutex_unlock(&p->a64cq->lock);
    }
    if (retired) *retired = r;
    if (failed) *failed = f;
    if (timed_out) *timed_out = t;
}

static void drain_work(cq_work_t *w, uint64_t deadline_ns) {
    if (!w->w.wait) return;
    uint64_t now = now_ns();
    bool f = false;
    uint64_t left = deadline_ns > now ? deadline_ns - now : 0;
    if (!w->w.wait(w->w.opaque, left, &f))
        LAGFX_WARN("arm64 cq: GPU work still unfinished at teardown; releasing anyway");
    work_release(w);
}

void lagfx_arm64_cq_shutdown(lagfx_protocol_t *p, uint32_t timeout_ms) {
    if (!lagfx_protocol_is_valid(p) || !p->a64cq) return;
    struct lagfx_arm64_cq *cq = p->a64cq;
    pthread_mutex_lock(&cq->lock);
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    size_t dropped = 0;
    for (uint32_t s = 0; s < CQ_SLOTS; s++) {
        for (lagfx_arm64_submit_ctx_t *c = cq->head[s], *nx; c; c = nx) {
            nx = c->next;
            for (uint32_t i = 0; i < c->nwork; i++) drain_work(&c->work[i], deadline);
            free(c); dropped++;
        }
        cq->head[s] = cq->tail[s] = NULL;
    }
    for (uint32_t i = 0; i < CQ_ZOMBIES; i++) drain_work(&cq->zombie[i], deadline);
    if (cq->cur) { for (uint32_t i = 0; i < cq->cur->nwork; i++) drain_work(&cq->cur->work[i], deadline); free(cq->cur); cq->cur = NULL; }
    if (dropped) LAGFX_WARN("arm64 cq: shutdown with %zu uncompleted command(s); their stamps are not written", dropped);
    p->a64cq = NULL;
    pthread_mutex_unlock(&cq->lock);
    pthread_mutex_destroy(&cq->lock);
    free(cq);
}
