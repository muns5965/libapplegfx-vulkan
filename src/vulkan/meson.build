# src/vulkan/meson.build — Vulkan init TU (Phase 1.B)
#
# Called from src/meson.build. Appends instance.c to lagfx_sources.
# The LAGFX_HAVE_VULKAN define is added in the top-level meson.build
# when vulkan_dep.found() is true; this file just contributes the
# source unconditionally (the file itself #ifdefs around the symbol).

lagfx_sources += files(
  'instance.c',
  'caps.c',
  'devinfo.c',
  'qcom_tile.c',
  'format_caps.c',
  'msaa.c',
  'command.c',
  'pipeline.c',
  'pipeline_build.c',
  'pipeline_bundle.c',
  'pipeline_repro.c',
  'spv_variants.c',
  'spv_iface.c',
  'pipeline_matrix.c',
  'spv_reduce.c',
  'spv_ssa.c',
  'pipeline_reduce.c',
  'descriptor_layout.c',
  'render_target.c',
  'cursor.c',
  'composite.c',
  'iosurface.c',
  'display_blit.c',
  'draw_record.c',
  'surface_writeback.c',
  'gpu_readback.c',
  'sampler_cache.c',
  'filter_cubic.c',
)
