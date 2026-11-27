/*
 * libapplegfx-vulkan — gamma-region read prefers the task that created the display mapping (links the library, no Vulkan device)
 * tests/display-diag-gamma-owner-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The same VA can exist in several tasks. The read must ask (1) the owner of the mapping named by the op 0x08, (2) the last 0x44's task, and only then (3) scan the
 * other live tasks as a diagnostic fallback. Here tasks 2, 6 and 9 can ALL read the region; the owner is task 9 (neither the last 0x44 = 6 nor the lowest id).
 * Reading task 2 or 6 instead would be the wrong content in a real run, so the first reader call must be task 9 and it must be the only one.
 */
#include "protocol/state.h"
#include "protocol/display_diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_calls, g_first, g_can[16], g_ncan;
static bool reader(lagfx_protocol_t *p, uint32_t task, uint64_t va, uint32_t len, uint8_t *out) {
    (void)p; if (!g_calls++) g_first = task;
    bool ok = false; for (uint32_t i = 0; i < g_ncan; i++) ok |= g_can[i] == task;
    if (!ok || va != 0x118000u || len != 0x4000u) return false;
    memset(out, 0, len);
    return true;
}
static bool g_owner_known; static uint64_t g_owner_map; static uint32_t g_owner_task;
static bool owner(lagfx_protocol_t *p, uint64_t mapping, uint32_t *task) { (void)p; if (!g_owner_known || mapping != g_owner_map) return false; *task = g_owner_task; return true; }
static void w32(uint8_t *b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24); }
static unsigned long long field(const char *line, const char *key) { const char *k = strstr(line, key); assert(k); return strtoull(k + strlen(key), NULL, 10); }

int main(void) {
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    assert(p);
    p->magic = LAGFX_PROTOCOL_MAGIC;
    lagfx_dd_set_reader(reader);
    lagfx_dd_set_owner_lookup(owner);
    uint32_t ids[3] = { 2, 6, 9 };
    for (int i = 0; i < 3; i++) { lagfx_task_entry_t *t = lagfx_protocol_alloc_task_slot(p); assert(t); t->id = ids[i]; t->live = true; }
    p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = 6;
    uint8_t op8[52]; memset(op8, 0, sizeof op8);
    w32(op8, 0, 0x08); w32(op8, 4, 52); w32(op8, 8, 0x400);
    w32(op8, 12 + 8, 7); w32(op8, 12 + 16, 0x118000u); w32(op8, 12 + 24, 0x4000u); w32(op8, 12 + 32, 0x101u); w32(op8, 12 + 36, 0x48400000u);
    char line[2048];

    /* 1. owner known, every task could read it: the owner is asked first and alone */
    g_ncan = 3; g_can[0] = 2; g_can[1] = 6; g_can[2] = 9;
    g_owner_known = true; g_owner_map = 7; g_owner_task = 9;
    lagfx_dd_note_cmd(p, 1, 4, op8, 52);
    lagfx_dd_stats_line(line, sizeof line);
    assert(field(line, "read_done=") == 1);
    assert(g_calls == 1 && g_first == 9);
    printf("display-diag-gamma-owner-unit OK (owner task asked first, alone)\n");
    free(p);
    return 0;
}
