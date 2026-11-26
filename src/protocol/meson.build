# src/protocol/meson.build — Protocol state machine sources
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: MIT

lagfx_sources += files(
  'state.h',
  'lifecycle.c',    # new/free/reset
  'stamp.c',        # advance_stamp_cell, complete_stamp_slot
  'resource_registry.c',  # resource mapping table (CmdMapMemory2, etc.)
  'ops_misc.c',     # NOP, Debug, GetDeviceInfo handlers
  # ops_display.c removed: uses old protocol state layout (lagfx_display_entry_t renamed to lagfx_display_mode_t)
  # Legacy handlers moved to src/handlers/display/display.c for vchan opcodes (0x01-0x07)
  'arm64_completion.c',  # ARM64 deferred stamp completion queue (LAGFX_ARM64_EXEC=1)
  'arm64_resources.c',    # ARM64 exec: guest-memory-backed buffer/texture mirror + blit executor
  'arm64_exec_decode.c',  # ARM64 0x43/0x45 decode (+ optional execution)
  'arm64_objects.c',      # ARM64 per-task (task, ref) object table
  'arm64_exec_run.c',     # ARM64 Render segment execution on Vulkan (LAGFX_ARM64_EXEC=1)
  'diag.c',         # command diagnostics (logging + counters only)
  'test_api.c',     # Test API exports: dispatch_one, mmio_read/write
   'object_resolver.c',  # Phase B object ID resolver for metallib lookup
)
