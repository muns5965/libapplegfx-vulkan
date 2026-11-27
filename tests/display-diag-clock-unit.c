/*
 * libapplegfx-vulkan — the g7-pvg-ops clock starts with the first command the diagnostics see (links the library, no Vulkan device)
 * tests/display-diag-clock-unit.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The "t=+Ns" and "first +Nms" values of the g7-pvg-ops lines are milliseconds since the first call of dd_ms(). That call used to happen only when one of the
 * four tracked opcodes (0x37, 0x22, 0x08, 0x0a) first arrived or when the first statistics line was printed, so the origin was the first statistics tick (~10 s
 * after the first ring command on a device run): every "first op=0x08 ... t=+15035ms" was ~10 s earlier than the same moment in the runner's own log.
 * The origin must be the first command that reaches lagfx_dd_note_cmd(), tracked or not (the same moment lagfx_diag_cmd() starts its own clock).
 *
 * Here an untracked op (0x01) is the first command, 200 ms pass, then a tracked op (0x22) arrives: its "first +Nms" has to be about 200, not 0.
 */
#include "protocol/state.h"
#include "protocol/display_diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void w32(uint8_t *b, size_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24); }
static unsigned long long field(const char *line, const char *key) { const char *k = strstr(line, key); assert(k); return strtoull(k + strlen(key), NULL, 10); }
static void sleep_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

int main(void) {
    lagfx_protocol_t *p = calloc(1, sizeof *p);
    assert(p);
    p->magic = LAGFX_PROTOCOL_MAGIC;
    uint8_t other[16]; memset(other, 0, sizeof other); w32(other, 0, 0x01); w32(other, 4, 16);   /* op 0x01: not one of the four tracked ops */
    uint8_t op22[16];  memset(op22, 0, sizeof op22);   w32(op22, 0, 0x22); w32(op22, 4, 16);
    char line[2048];

    lagfx_dd_note_cmd(p, 1, 3, other, sizeof other);   /* the first command the diagnostics see */
    sleep_ms(200);
    lagfx_dd_note_cmd(p, 1, 3, op22, sizeof op22);     /* the first tracked op, 200 ms later */
    lagfx_dd_stats_line(line, sizeof line);
    unsigned long long first22 = field(line, "op22=1(first +");
    printf("g7-pvg-ops op22 first +%llums (expected ~200)\n", first22);
    assert(first22 >= 150 && first22 < 1500);          /* before the fix: 0 (the origin was the first tracked op) */
    printf("display-diag-clock-unit OK (the pvg-ops clock starts at the first command, not at the first tracked op)\n");
    free(p);
    return 0;
}
