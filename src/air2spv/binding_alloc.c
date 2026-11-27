/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — the descriptor binding allocator / table
 * src/air2spv/binding_alloc.c
 *
 * Copyright © 2026 Matthew Jackson
 */
#include "binding_alloc.h"

#include <stdio.h>
#include <string.h>

void lagfx_ba_init(lagfx_ba_t *a) { memset(a, 0, sizeof *a); }

static lagfx_ba_entry_t *find(lagfx_ba_t *a, uint32_t set, uint32_t binding) {
    for (uint32_t i = 0; i < a->n; i++)
        if (a->e[i].set == set && a->e[i].binding == binding) return &a->e[i];
    return NULL;
}

const char *lagfx_ba_kind_name(lagfx_spv_binding_kind_t k) {
    switch (k) {
    case LAGFX_SPV_BINDING_SAMPLED_IMAGE: return "SAMPLED_IMAGE";
    case LAGFX_SPV_BINDING_SAMPLER: return "SAMPLER";
    case LAGFX_SPV_BINDING_STORAGE_BUFFER: return "STORAGE_BUFFER";
    case LAGFX_SPV_BINDING_INPUT_ATTACHMENT: return "INPUT_ATTACHMENT";
    case LAGFX_SPV_BINDING_STORAGE_IMAGE: return "STORAGE_IMAGE";
    default: return "UNKNOWN";
    }
}

static void record_conflict(lagfx_ba_t *a, uint32_t reason, const lagfx_ba_entry_t *have, const lagfx_ba_entry_t *want) {
    if (a->nconflict < LAGFX_BA_MAX_CONFLICTS) {
        lagfx_ba_conflict_t *c = &a->conflict[a->nconflict];
        c->reason = reason, c->set = want->set, c->binding = want->binding, c->kind_a = have->kind, c->kind_b = want->kind, c->role_a = have->role_index, c->role_b = want->role_index;
    }
    a->nconflict++;
}

int lagfx_ba_claim(lagfx_ba_t *a, const lagfx_ba_entry_t *want) {
    lagfx_ba_entry_t *have = find(a, want->set, want->binding);
    if (have) {
        if (have->kind != want->kind) { record_conflict(a, LAGFX_BA_REASON_KIND, have, want); return LAGFX_BA_CONFLICT; }
        if (want->kind == LAGFX_SPV_BINDING_INPUT_ATTACHMENT && have->role_index != want->role_index) { record_conflict(a, LAGFX_BA_REASON_ROLE, have, want); return LAGFX_BA_CONFLICT; }
        if (want->kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE && have->image_dim && want->image_dim && have->image_dim != want->image_dim) { record_conflict(a, LAGFX_BA_REASON_DIM, have, want); return LAGFX_BA_CONFLICT; }
        have->stages |= want->stages;
        have->may_write |= want->may_write;
        if (!have->image_dim) have->image_dim = want->image_dim;
        return LAGFX_BA_SHARED;
    }
    if (a->n >= LAGFX_BA_MAX) { a->noverflow++; return LAGFX_BA_FULL; }
    a->e[a->n++] = *want;
    return LAGFX_BA_OK;
}

int lagfx_ba_alloc(lagfx_ba_t *a, uint32_t set, lagfx_spv_binding_kind_t kind, uint32_t stages, uint32_t image_dim, uint32_t role_index, uint32_t *binding) {
    uint32_t b = 0;
    while (find(a, set, b)) b++;   /* the lowest free binding: sequential for a fresh table, and it can never land on a taken one */
    lagfx_ba_entry_t e = { .set = set, .binding = b, .kind = kind, .stages = stages, .image_dim = image_dim, .role_index = role_index, .may_write = 0 };
    int r = lagfx_ba_claim(a, &e);
    if (binding) *binding = b;
    return r;
}

int lagfx_ba_ok(const lagfx_ba_t *a) { return a->nconflict == 0 && a->noverflow == 0; }

uint32_t lagfx_ba_import(lagfx_ba_t *a, const lagfx_spv_binding_t *refl, size_t n, uint32_t stages) {
    uint32_t bad = 0;
    for (size_t i = 0; i < n; i++) {
        if (refl[i].kind == LAGFX_SPV_BINDING_UNKNOWN) continue;
        lagfx_ba_entry_t e = { .set = refl[i].set, .binding = refl[i].binding, .kind = refl[i].kind, .stages = stages, .image_dim = refl[i].image_dim, .role_index = refl[i].role_index, .may_write = refl[i].may_write };
        int r = lagfx_ba_claim(a, &e);
        if (r == LAGFX_BA_CONFLICT || r == LAGFX_BA_FULL) bad++;
    }
    return bad;
}

void lagfx_ba_sort(lagfx_ba_t *a) {
    for (uint32_t i = 1; i < a->n; i++) {
        lagfx_ba_entry_t k = a->e[i];
        uint32_t j = i;
        while (j && (a->e[j - 1].set > k.set || (a->e[j - 1].set == k.set && a->e[j - 1].binding > k.binding))) a->e[j] = a->e[j - 1], j--;
        a->e[j] = k;
    }
}

int lagfx_ba_merge_modules(lagfx_ba_t *a, const uint8_t *const *spv, const size_t *len, uint32_t n_modules) {
    for (uint32_t m = 0; m < n_modules; m++) {
        if (!spv[m] || !len[m]) continue;
        lagfx_spv_binding_t refl[LAGFX_BA_MAX];
        size_t total = lagfx_spv_reflect_bindings(spv[m], len[m], refl, LAGFX_BA_MAX);
        size_t have = total < LAGFX_BA_MAX ? total : LAGFX_BA_MAX;
        if (total > LAGFX_BA_MAX) a->noverflow += (uint32_t)(total - LAGFX_BA_MAX);   /* reflect returns the TOTAL: more than fit is an error, not a truncation */
        uint32_t model = lagfx_spv_reflect_exec_model(spv[m], len[m]);
        uint32_t stage = model == 0u ? LAGFX_BA_STAGE_VERTEX : model == 4u ? LAGFX_BA_STAGE_FRAGMENT : model == 5u ? LAGFX_BA_STAGE_COMPUTE : (LAGFX_BA_STAGE_VERTEX | LAGFX_BA_STAGE_FRAGMENT);
        lagfx_ba_import(a, refl, have, stage);
    }
    lagfx_ba_sort(a);
    return lagfx_ba_ok(a);
}

uint64_t lagfx_ba_key(const lagfx_ba_t *a) {
    lagfx_ba_t s = *a;
    lagfx_ba_sort(&s);
    uint64_t h = 1469598103934665603ull;
#define MIX(v) do { uint32_t v_ = (uint32_t)(v); for (int k_ = 0; k_ < 4; k_++) { h ^= (v_ >> (8 * k_)) & 0xffu; h *= 1099511628211ull; } } while (0)
    for (uint32_t i = 0; i < s.n; i++) { MIX(s.e[i].set); MIX(s.e[i].binding); MIX(s.e[i].kind); MIX(s.e[i].stages); MIX(s.e[i].image_dim); MIX(s.e[i].role_index); }
    MIX(s.nconflict); MIX(s.noverflow);
#undef MIX
    return h;
}

uint32_t lagfx_ba_stage_base(const uint8_t *prev, size_t len, uint32_t min_base) {
    uint32_t base = min_base;
    if (!prev || !len) return base;
    lagfx_spv_binding_t refl[LAGFX_BA_MAX];
    size_t total = lagfx_spv_reflect_bindings(prev, len, refl, LAGFX_BA_MAX), have = total < LAGFX_BA_MAX ? total : LAGFX_BA_MAX;
    for (size_t i = 0; i < have; i++)
        if (refl[i].set == 0u && refl[i].binding + 1u > base) base = refl[i].binding + 1u;
    return base;
}

size_t lagfx_ba_describe(const lagfx_ba_t *a, char *buf, size_t cap) {
    size_t need = 0;
    char line[200];
    if (buf && cap) buf[0] = 0;
    lagfx_ba_t s = *a;
    lagfx_ba_sort(&s);
#define EMIT() do { size_t l_ = strlen(line); if (buf && need + l_ + 1 <= cap) memcpy(buf + need, line, l_ + 1); need += l_; } while (0)
    for (uint32_t i = 0; i < s.n; i++) {
        const lagfx_ba_entry_t *e = &s.e[i];
        char role[24] = "";
        if (e->role_index != LAGFX_SPV_NO_ROLE) snprintf(role, sizeof role, " role=%u", e->role_index);
        snprintf(line, sizeof line, "set %u binding %-3u %-16s stages=%s%s%s%s\n", e->set, e->binding, lagfx_ba_kind_name(e->kind), (e->stages & LAGFX_BA_STAGE_VERTEX) ? "V" : "", (e->stages & LAGFX_BA_STAGE_FRAGMENT) ? "F" : "", (e->stages & LAGFX_BA_STAGE_COMPUTE) ? "C" : "", role);
        EMIT();
    }
    for (uint32_t i = 0; i < s.nconflict && i < LAGFX_BA_MAX_CONFLICTS; i++) {
        const lagfx_ba_conflict_t *c = &s.conflict[i];
        if (c->reason == LAGFX_BA_REASON_KIND) snprintf(line, sizeof line, "CONFLICT set %u binding %u: %s vs %s (one binding holds one descriptor type)\n", c->set, c->binding, lagfx_ba_kind_name(c->kind_a), lagfx_ba_kind_name(c->kind_b));
        else if (c->reason == LAGFX_BA_REASON_ROLE) snprintf(line, sizeof line, "CONFLICT set %u binding %u: input attachments %u vs %u on one binding\n", c->set, c->binding, c->role_a, c->role_b);
        else snprintf(line, sizeof line, "CONFLICT set %u binding %u: sampled images of different dimensionality on one binding\n", c->set, c->binding);
        EMIT();
    }
    if (s.nconflict > LAGFX_BA_MAX_CONFLICTS) { snprintf(line, sizeof line, "... %u more conflict(s)\n", s.nconflict - LAGFX_BA_MAX_CONFLICTS); EMIT(); }
    if (s.noverflow) { snprintf(line, sizeof line, "OVERFLOW: %u descriptor(s) did not fit the %u-entry table\n", s.noverflow, LAGFX_BA_MAX); EMIT(); }
#undef EMIT
    return need;
}
