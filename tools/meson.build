# Standalone tools that don't link against lagfx.
# Python scripts don't need build registration; this file exists
# so `subdir('tools')` works from the top level if needed later.

# VK_QCOM_tile_shading device probe (docs/qcom-tile-shading.md section 7): standalone, links only the Vulkan loader and libm; not part of the library.
if vulkan_dep.found()
  qcom_tile_probe = executable('qcom-tile-probe',
    'qcom-tile-probe/qcom_tile_probe.c',
    include_directories : include_directories('qcom-tile-probe'),
    dependencies : [vulkan_dep, cc.find_library('m', required : false)],
    c_args : ['-D_GNU_SOURCE'],
    build_by_default : true,
  )
endif
