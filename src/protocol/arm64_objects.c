/*
 * libapplegfx-vulkan — ARM64 per-task object table. See arm64_objects.h.
 */
#include "arm64_objects.h"
#include "arm64_exec_decode.h"
#include "../common/le.h"

#include <stdlib.h>
#include <string.h>

#define NBUCKET 512u

typedef struct node {
    struct node *next;
    uint32_t task, ref;
    uint32_t entry[3];
    uint64_t root_pfn;     /* the task's root page when the object was built: another one means a re-defined task */
    void *obj;
} node_t;

struct lagfx_a64_objects {
    lagfx_a64_obj_ops_t ops;
    node_t *bucket[NBUCKET];
    lagfx_a64_obj_stats_t st;
};

static uint32_t hash(uint32_t task, uint32_t ref) { return ((task * 0x9e3779b1u) ^ (ref * 0x85ebca6bu)) % NBUCKET; }

lagfx_a64_objects_t *lagfx_a64_objects_new(const lagfx_a64_obj_ops_t *ops) {
    if (!ops || !ops->create || !ops->destroy) return NULL;
    lagfx_a64_objects_t *t = calloc(1, sizeof *t);
    if (t) t->ops = *ops;
    return t;
}

static void release(lagfx_a64_objects_t *t, node_t *n) {
    if (n->obj) {
        t->ops.destroy(t->ops.ctx, n->obj);
        t->st.destroyed++;
        n->obj = NULL;
    }
}

uint32_t lagfx_a64_objects_drop_task(lagfx_a64_objects_t *t, uint32_t task_id) {
    if (!t) return 0;
    uint32_t dropped = 0;
    for (uint32_t b = 0; b < NBUCKET; b++) {
        node_t **pp = &t->bucket[b];
        while (*pp) {
            node_t *n = *pp;
            if (n->task != task_id) { pp = &n->next; continue; }
            *pp = n->next;
            release(t, n);
            free(n);
            t->st.live--;
            dropped++;
        }
    }
    return dropped;
}

void lagfx_a64_objects_free(lagfx_a64_objects_t *t) {
    if (!t) return;
    for (uint32_t b = 0; b < NBUCKET; b++) {
        node_t *n = t->bucket[b];
        while (n) {
            node_t *nx = n->next;
            release(t, n);
            free(n);
            n = nx;
        }
    }
    free(t);
}

void lagfx_a64_objects_stats(const lagfx_a64_objects_t *t, lagfx_a64_obj_stats_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (t && out) *out = t->st;
}

void *lagfx_a64_objects_get(lagfx_a64_objects_t *t, lagfx_protocol_t *p, const lagfx_task_entry_t *task, uint32_t ref, uint32_t out_entry[3]) {
    if (!t || !p || !task || !ref) return NULL;
    /* the same bounds as the decoder: Apple only reads the list when the task has set it */
    if (!task->lists_set || !task->objects_addr || (task->objects_len && ((uint64_t)ref + 1u) * 12u > task->objects_len)) return NULL;
    uint8_t eb[12];
    if (!lagfx_arm64_task_read(p, task, task->objects_addr + (uint64_t)ref * 12u, eb, 12)) { t->st.read_failed++; return NULL; }
    uint32_t e[3] = { lagfx_le32(eb), lagfx_le32(eb + 4), lagfx_le32(eb + 8) };
    if (out_entry) memcpy(out_entry, e, sizeof e);

    uint32_t h = hash(task->id, ref);
    node_t *n = t->bucket[h];
    while (n && !(n->task == task->id && n->ref == ref)) n = n->next;
    if (n && !memcmp(n->entry, e, sizeof e) && n->root_pfn == task->root_page_pfn) { t->st.hits++; return n->obj; }
    if (n) {                                   /* the guest wrote a new entry into the slot (or the task was re-defined): the old object is stale */
        release(t, n);
        t->st.recreated++;
    } else {
        n = calloc(1, sizeof *n);
        if (!n) return NULL;
        n->task = task->id, n->ref = ref;
        n->next = t->bucket[h], t->bucket[h] = n;
        t->st.live++;
    }
    memcpy(n->entry, e, sizeof e);
    n->root_pfn = task->root_page_pfn;
    if ((e[0] & 0xffu) == 0u) return NULL;     /* an empty slot: remembered (so a later entry is noticed), nothing to build */
    n->obj = t->ops.create(t->ops.ctx, p, task, ref, e);
    if (n->obj) t->st.created++; else t->st.failed++;
    return n->obj;
}
