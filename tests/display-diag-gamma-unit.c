/*
 * libapplegfx-vulkan — display diagnostics: op 0x0a classification and the gamma-region read, through lagfx_dd_note_cmd (links the library, no Vulkan device)
 * tests/display-diag-gamma-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Replays the real wire bytes of the g7 vm2 run (/tmp/vm2 g7_lagfx_1795752677846.txt lines 5478-5686): the op 0x08 payload with the gamma descriptor and the op 0x0a payload,
 * and checks what the summary line (g7-pvg-ops) says. Two defects this pins:
 *  (1) op 0x0a was classified with a packed-3x3 test at payload byte 0, so the real (identity) payload counted as "other";
 *  (2) the gamma region was read only through the task of the LAST 0x44 (task 6 in vm2) although the display surface belongs to task 2; one wrong guess ended the search for good
 *      because only the first op 0x08 carries the descriptor.
 */
#include "protocol/state.h"
#include "protocol/display_diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_reader_ok_task, g_reader_calls;
static bool reader(lagfx_protocol_t *p, uint32_t task, uint64_t va, uint32_t len, uint8_t *out) {
    (void)p; g_reader_calls++;
    if (task != g_reader_ok_task || va != 0x118000u || len != 0x4000u) return false;
    for (uint32_t i = 0; i < len; i++) out[i] = (uint8_t)(i & 0xff);
    return true;
}
static void w32(uint8_t *b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24); }
static void hdr(uint8_t *b, uint32_t op, uint32_t len, uint32_t stamp) { w32(b, 0, op); w32(b, 4, len); w32(b, 8, stamp); }
static unsigned long long field(const char *line, const char *key) { const char *k = strstr(line, key); assert(k); return strtoull(k + strlen(key), NULL, 10); }

int main(void) {
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    assert(p);
    p->magic = LAGFX_PROTOCOL_MAGIC;
    lagfx_dd_set_reader(reader);
    char line[2048];

    /* op 0x0a, 76 bytes, as logged: payload {0, 1, 0, 0x0c, 1.0, 0,0,0, 0, 1.0, ...}; the third diagonal element is inferred (not in the logged 64 bytes) */
    uint8_t op0a[76]; memset(op0a, 0, sizeof op0a);
    hdr(op0a, 0x0a, 76, 0x500);
    w32(op0a, 12 + 4, 1); w32(op0a, 12 + 12, 0x0c); w32(op0a, 12 + 16, 0x3f800000u); w32(op0a, 12 + 36, 0x3f800000u); w32(op0a, 12 + 56, 0x3f800000u);
    lagfx_dd_note_cmd(p, 1, 4, op0a, 76);
    lagfx_dd_note_cmd(p, 1, 4, op0a, 76);
    lagfx_dd_stats_line(line, sizeof line);
    assert(field(line, "op0a=") == 2);
    assert(field(line, "identity=") == 2 && field(line, "other=") == 0);   /* before the fix: identity=0 other=2, exactly what the vm2 run printed */
    /* a real non-identity matrix is still reported as other */
    w32(op0a, 12 + 36, 0x40000000u);
    lagfx_dd_note_cmd(p, 1, 4, op0a, 76);
    lagfx_dd_stats_line(line, sizeof line);
    assert(field(line, "identity=") == 2 && field(line, "other=") == 1);

    /* op 0x08, 52 bytes: {u32 port 0, u32 w1 0, u64 mapping 7, u64 gamma offset 0x118000, u64 length 0x4000, u32 count 257, u32 0x48400000} */
    uint8_t op8[52]; memset(op8, 0, sizeof op8);
    hdr(op8, 0x08, 52, 0x400);
    w32(op8, 12 + 8, 7); w32(op8, 12 + 16, 0x118000u); w32(op8, 12 + 24, 0x4000u); w32(op8, 12 + 32, 0x101u); w32(op8, 12 + 36, 0x48400000u);
    /* vm2: the last 0x44 was task 6, the display surface was created by task 2. No live task can read the region: attempt counted, nothing done. */
    p->diag2.last44_global_valid = 1; p->diag2.last44_global_task = 6;
    g_reader_ok_task = 2;
    lagfx_dd_note_cmd(p, 1, 4, op8, 52);
    lagfx_dd_stats_line(line, sizeof line);
    assert(field(line, "read_attempts=") == 1 && field(line, "read_done=") == 0);
    /* tasks 2 and 6 exist now; the descriptor arrives again (as a retry would): the read must find task 2 although the last 0x44 was task 6 */
    lagfx_task_entry_t *t2 = lagfx_protocol_alloc_task_slot(p), *t6;
    assert(t2); t2->id = 2; t2->live = true;
    t6 = lagfx_protocol_alloc_task_slot(p);
    assert(t6); t6->id = 6; t6->live = true;
    g_reader_calls = 0;
    lagfx_dd_note_cmd(p, 1, 4, op8, 52);
    lagfx_dd_stats_line(line, sizeof line);
    assert(field(line, "read_attempts=") == 2 && field(line, "read_done=") == 1);   /* before the fix: read_done=0 */
    assert(g_reader_calls == 2);   /* task 6 (the last 0x44) first, then task 2 */
    printf("display-diag-gamma-unit OK\n");
    free(p);
    return 0;
}
