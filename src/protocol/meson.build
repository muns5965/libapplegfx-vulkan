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
  'arm64_pipeline.c',     # ARM64 pipeline-state resolver (serialized pipeline -> SPIR-V, layouts)
  'arm64_tile.c',         # ARM64 tile shaders: tag 0x33 tile pipelines, tile binds, compute dispatch between two halves of a render pass
  'arm64_depth_stencil.c', # ARM64 depth/stencil state object (tag 4) decoder
  'arm64_capture_load.c', # ARM64 render replay capture: loader (no Vulkan)
  'arm64_capture.c',      # ARM64 render replay capture: writer (LAGFX_ARM64_DUMP_DATA=1)
  'arm64_dump.c',         # ARM64 bounded capture of real pipeline/library/binding data (LAGFX_ARM64_DUMP=1)
  'arm64_sampler.c',      # ARM64 serialized sampler (tag 3) -> VkSamplerCreateInfo
  'arm64_draw_decode.c',  # ARM64 render draw record decoder (tags 0-11, 0x10, 0x11)
  'arm64_exec_run.c',     # ARM64 Render segment execution on Vulkan (LAGFX_ARM64_EXEC=1)
  'draw_diag.c',    # bounded per-draw diagnostics (G7_DRAW_DIAG)
  'diag.c',         # command diagnostics (logging + counters only)
  'gamma_diag.c',   # gamma descriptor diagnostics (read-only): arm64 16 KiB reads, split counters, address-space probes
  'display_diag.c', # display-path diagnostics: swap format, gamma capture (read-only), cursor/vbl counters
  'test_api.c',     # Test API exports: dispatch_one, mmio_read/write
   'object_resolver.c',  # Phase B object ID resolver for metallib lookup
)
