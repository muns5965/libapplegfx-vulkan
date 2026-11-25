/*
 * libapplegfx-vulkan — Root channel dispatcher (ch 0)
 * src/dispatchers/channel_0_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#ifndef LAGFX_CHANNEL_0_DISPATCHER_H
#define LAGFX_CHANNEL_0_DISPATCHER_H

#include <stddef.h>
#include "protocol/state.h"
#include "protocol/opcodes.h"

/* Drain root channel ring buffer. Returns number of commands processed. */
size_t channel_0_dispatcher_drain(lagfx_protocol_t *p);

/* Task registration shared by root (0x38) and compute (0x38/0x44) channels. */
bool lagfx_define_host_task(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
bool lagfx_define_task_lists(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif /* LAGFX_CHANNEL_0_DISPATCHER_H */
