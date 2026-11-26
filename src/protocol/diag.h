/*
 * libapplegfx-vulkan — command diagnostics (logging and counters only; nothing here changes behaviour)
 * src/protocol/diag.h
 *
 * Added to find out what the guest really sends: a global command sequence with monotonic time, the raw bytes of selected commands
 * (header and barrier slots included), the last 0x44 task per channel, and cumulative counters that keep counting even if the detailed
 * log is limited. A "g7-gpu:" statistics line goes to stderr and to the lagfx log every 10 s (checked on guest MMIO accesses).
 */
#ifndef LAGFX_PROTOCOL_DIAG_H
#define LAGFX_PROTOCOL_DIAG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

enum { LAGFX_DIAG_ROOT = 0, LAGFX_DIAG_COMPUTE = 1, LAGFX_DIAG_DISPLAY = 2 };

/* One command, called by a drain loop before it dispatches. `raw` is the whole command starting at its header, `length` its header length field. */
void lagfx_diag_cmd(lagfx_protocol_t *p, int src, uint32_t chan, uint32_t rp, const uint8_t *raw, uint32_t length);
/* A compute 0x44 for `task` was seen on `chan`. */
void lagfx_diag_note_0x44(lagfx_protocol_t *p, uint32_t chan, uint32_t task);
/* Guest MMIO access: counts it and prints the 10 s statistics line when due. */
void lagfx_diag_mmio(lagfx_protocol_t *p, bool is_write);
/* Timer-driven: prints the 10 s statistics even when the guest makes no MMIO accesses (idle in WFI). */
void lagfx_diag_tick(lagfx_protocol_t *p);
/* At device teardown: prints the cumulative statistics once. */
void lagfx_diag_final(lagfx_protocol_t *p);
/* Bytes written to / write errors of the lagfx log (device.c). */
uint64_t lagfx_log_bytes_written(void);
uint64_t lagfx_log_write_errors(void);

#endif
