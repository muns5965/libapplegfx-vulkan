# tests/meson.build — header, memory, lifecycle, and optional vulkan smoke.

# Header syntax check — always runnable; also smoke-tests version.c
# via libapplegfx_vulkan_dep.
header_deps = [libapplegfx_vulkan_dep]
if vulkan_dep.found()
  header_deps += [vulkan_dep]
endif
header_test = executable('header-syntax-check',
  'header-syntax-check.c',
  include_directories : include_dir,
  dependencies : header_deps,
  build_by_default : true,
)
test('header syntax', header_test)

# Lifecycle smoke — covers device + display + mmio stubs.
if fs.is_file(meson.current_source_dir() / 'lifecycle-smoke.c')
  lifecycle_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    lifecycle_deps += [vulkan_dep]
  endif
  lifecycle_test = executable('lifecycle-smoke',
    'lifecycle-smoke.c',
    include_directories : include_dir,
    dependencies : lifecycle_deps,
    build_by_default : true,
  )
  test('lifecycle smoke', lifecycle_test)
endif

# memory-task test (only built when src/memory has produced task.c AND
# the local test source exists). Runtime memfd/MAP_FIXED behavior is
# Linux-only, so we only register the meson test() on Linux hosts;
# on other hosts we still BUILD it to catch syntax regressions.
if fs.is_file(meson.current_source_dir() / 'memory-task.c') and \
   fs.is_file(meson.project_source_root() / 'src' / 'memory' / 'task.c')
  memory_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    memory_deps += [vulkan_dep]
  endif
  memory_test = executable('memory-task-test',
    'memory-task.c',
    include_directories : include_dir,
    dependencies : memory_deps,
    build_by_default : true,
  )
  if host_machine.system() == 'linux'
    test('memory task', memory_test)
  else
    message('memory-task test built but not run (non-Linux host).')
  endif
endif

# memory-coherence test (Phase 1.C gate for Phase 2 first-pixel).
# Proves lagfx_task_map_host_memory aliases host_ptr's pages into the
# task VA rather than copying them. See docs/memory-coherence-audit.md
# and tests/memory-coherence.c for the full rationale.
#
# Linux-only test() registration: the fix uses mremap() which is Linux-
# specific. On Darwin the library intentionally falls back to copy-on-
# map (dev path only, per audit §4), and the test would correctly fail
# there — so we BUILD on Darwin for syntax coverage but only RUN on
# Linux. The test itself exits 77 (meson SKIP) at runtime if the host
# kernel lacks memfd_create, keeping CI green on very old Linux.
if fs.is_file(meson.current_source_dir() / 'memory-coherence.c') and \
   fs.is_file(meson.project_source_root() / 'src' / 'memory' / 'task.c')
  memory_coherence_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    memory_coherence_deps += [vulkan_dep]
  endif
  memory_coherence_test = executable('memory-coherence',
    'memory-coherence.c',
    include_directories : include_dir,
    dependencies : memory_coherence_deps,
    build_by_default : true,
  )
  if host_machine.system() == 'linux'
    test('memory coherence', memory_coherence_test)
  else
    message('memory-coherence test built but not run (non-Linux host; copy-on-map fallback is expected there).')
  endif
endif

# gpu_cores / thread_count env-plumbing test. Verifies that
# lagfx_device_descriptor_t::thread_count is translated into the
# LP_NUM_THREADS env var by lagfx_device_new, per the spec at
# paravirt-re/gpu-cores-implementation-spec.md.
if fs.is_file(meson.current_source_dir() / 'gpu-cores.c')
  gpu_cores_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    gpu_cores_deps += [vulkan_dep]
  endif
  gpu_cores_test = executable('gpu-cores',
    'gpu-cores.c',
    include_directories : include_dir,
    dependencies : gpu_cores_deps,
    build_by_default : true,
  )
  test('gpu cores', gpu_cores_test)
endif

# Resource registry unit tests (standalone — no Vulkan dependency).
# Tests lagfx_resource_register/lookup/unregister/clear_task directly.
if fs.is_file(meson.current_source_dir() / 'resource-unit.c')
  resource_unit_test = executable('resource-unit',
    'resource-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('resource registry (unit)', resource_unit_test)
endif

# Pipeline descriptor function-ref scan (unit) — red-on-revert for the
# login-composite substitution bug (fragment-ref-first / vertex past 128B).
if fs.is_file(meson.current_source_dir() / 'object-resolver-scan-unit.c')
  objres_scan_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    objres_scan_deps += [vulkan_dep]
  endif
  objres_scan_test = executable('object-resolver-scan-unit',
    'object-resolver-scan-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : objres_scan_deps,
    build_by_default : true,
  )
  test('object-resolver scan (unit)', objres_scan_test)
endif

# Stamp advancement unit tests (standalone — no Vulkan dependency).
# Tests lagfx_advance_stamp_cell monotonicity directly with buffer inputs.
if fs.is_file(meson.current_source_dir() / 'stamp-unit.c')
  stamp_unit_test = executable('stamp-unit',
    'stamp-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('stamp helpers (unit)', stamp_unit_test)
endif

# Deadlock detection tests. Regression suite for ABBA deadlock between
# WindowServer and DisplayPipe. Tests online event timing (must be IMMEDIATE,
# not deferred), stamp ACK latency (<10ms), and device creation timeout (<5s).
if fs.is_file(meson.current_source_dir() / 'deadlock-detect.c')
  deadlock_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    deadlock_deps += [vulkan_dep]
  endif
  deadlock_detect_test = executable('deadlock-detect',
    'deadlock-detect.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : deadlock_deps,
    build_by_default : true,
  )
  test('deadlock detection', deadlock_detect_test)
endif

# Render opcode tests - drawPrimitives family (most used opcodes).
if fs.is_file(meson.current_source_dir() / 'render-opcode-draw.c')
  render_draw_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    render_draw_deps += [vulkan_dep]
  endif
  render_draw_test = executable('render-opcode-draw',
    'render-opcode-draw.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : render_draw_deps,
    build_by_default : true,
  )
  test('render opcode draw', render_draw_test)
endif

# Render opcode tests - pipeline state setup (setPipelineState, blend constants).
if fs.is_file(meson.current_source_dir() / 'render-opcode-pipeline.c')
  render_pipe_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    render_pipe_deps += [vulkan_dep]
  endif
  render_pipeline_test = executable('render-opcode-pipeline',
    'render-opcode-pipeline.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : render_pipe_deps,
    build_by_default : true,
  )
  test('render opcode pipeline', render_pipeline_test)
endif

# ExecIndirect2 outer-payload parser tests. Validates CmdExecIndirect2
# payload parsing (task_id, descriptor_count, resource_count), query mode
# with minimal payloads, and empty completion handling.
if fs.is_file(meson.current_source_dir() / 'cmdexecindirect2-parser.c')
  execind_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    execind_deps += [vulkan_dep]
  endif
  execind_test = executable('cmdexecindirect2-parser',
    'cmdexecindirect2-parser.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : execind_deps,
    build_by_default : true,
  )
  test('execindirect2 parser', execind_test)
endif

# Shader catalog unit tests - LRU eviction and lookup.
if fs.is_file(meson.current_source_dir() / 'm1-shader-catalog.c')
  shader_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    shader_deps += [vulkan_dep]
  endif
  shader_catalog_test = executable('m1-shader-catalog',
    'm1-shader-catalog.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : shader_deps,
    build_by_default : true,
  )
  test('shader catalog', shader_catalog_test)
endif

# Optional lavapipe smoke test — only built when vulkan is present.
if fs.is_file(meson.current_source_dir() / 'lavapipe-smoke.c') \
   and vulkan_dep.found()
  lavapipe_test = executable('lavapipe-smoke',
    'lavapipe-smoke.c',
    include_directories : include_dir,
    dependencies : [vulkan_dep],
    build_by_default : false,  # long-ish; run manually
  )
  # Not wired to `meson test` auto-run — it requires lavapipe ICD on
  # the host which CI may or may not provide. Run with:
  #   meson compile -C build lavapipe-smoke && ./build/tests/lavapipe-smoke
endif

# Phase 1.B: Vulkan init smoke. Only registered when vulkan_dep is
# present at configure time; the test itself emits SKIP at runtime
# if the host has no loadable ICD (typical on a Darwin dev box —
# the library's LAGFX_HAVE_VULKAN path still falls back to the no-op
# stub init, which the test recognises and skips gracefully).
if fs.is_file(meson.current_source_dir() / 'vulkan-init.c') \
   and vulkan_dep.found()
  vulkan_init_test = executable('vulkan-init',
    'vulkan-init.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vulkan init', vulkan_init_test)
endif

# Phase 1.B.2: Vulkan command pool + empty submit smoke. Same gating
# + runtime SKIP policy as the Phase 1.B init test — depends on the
# vulkan dep at configure time, tolerates the no-loadable-ICD case at
# runtime by emitting SKIP instead of failing.
if fs.is_file(meson.current_source_dir() / 'vulkan-command.c') \
   and vulkan_dep.found()
  vulkan_command_test = executable('vulkan-command',
    'vulkan-command.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vulkan command', vulkan_command_test)
endif

# Phase 2.B: Vulkan render target + clear + readback smoke. Same gating
# + runtime SKIP policy — the test creates a 64x64 BGRA8 render target,
# clears it to red, reads back, and asserts the centre pixel is red.
if fs.is_file(meson.current_source_dir() / 'vulkan-render.c') \
   and vulkan_dep.found()
  vulkan_render_test = executable('vulkan-render',
    'vulkan-render.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vulkan render', vulkan_render_test)
endif

# Phase 3.C scaffold: shader catalog lookup smoke. Exercises the
# static catalog table and SPIR-V-magic asserts on every stock
# shader's vertex + fragment blob. Darwin-compatible — does not
# touch Vulkan at runtime (vkCreateShaderModule lands Phase 3.E);
# the gate is only on the test source existing. Reaches into src/
# for shaders/catalog.h, so it needs internal_include_dir.
if fs.is_file(meson.current_source_dir() / 'shader-catalog.c')
  shader_catalog_test = executable('shader-catalog',
    'shader-catalog.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('shader catalog', shader_catalog_test)
endif

# Phase 3.C: Stock-shader VkShaderModule round-trip smoke. Complements
# tests/shader-catalog.c — that test only asserts SPIR-V magic on the
# embedded blobs; this one actually feeds each blob through
# vkCreateShaderModule on a live Vulkan device and asserts the loader
# accepts it. Same gating + runtime-SKIP policy as vulkan-render:
# registered only when vulkan_dep was present at configure time, and
# the test itself emits SKIP (exit 0) when no loadable ICD is present.
# Reaches into src/ for vulkan/instance.h + shaders/catalog.h, so
# internal_include_dir is needed.
if fs.is_file(meson.current_source_dir() / 'stock-shaders.c')
  stock_shaders_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    stock_shaders_deps += [vulkan_dep]
  endif
  stock_shaders_test = executable('stock-shaders',
    'stock-shaders.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : stock_shaders_deps,
    build_by_default : true,
  )
  test('stock shaders', stock_shaders_test)
endif

# Phase 3.C.2 M5: Apple-pipeline stock-shader lavapipe load smoke.
# Parallel to stock-shaders above, but exercises the Apple-AIR-sourced
# SPV blobs shipped at src/shaders/spv-apple/ (produced by src/
# shaders/compile_msl.sh). Point of the test is to prove the stock
# shaders ALSO reach lavapipe via the real Apple translation pipeline
# (MSL -> AIR -> metallib -> llc -> sig-xform -> SPIR-V), not just the
# GLSL reference bypass — the M5 full-completeness bar.
#
# Uses LAGFX_APPLE_SPV_BUILD_DIR (compile-time define, populated from
# src/shaders/meson.build's apple_shader_spv_build_dir variable) to
# locate the blobs staged into the build tree via configure_file(
# copy:true). Runtime env LAGFX_APPLE_SPV_DIR overrides.
#
# Declares apple_spv_files as additional sources so meson retriggers
# the test when any apple_*.spv changes — same pattern as
# catalog_embedded for the GLSL path.
if fs.is_file(meson.current_source_dir() / 'apple-stock-shaders.c')
  apple_stock_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    apple_stock_deps += [vulkan_dep]
  endif
  apple_stock_cargs = [
    '-DLAGFX_APPLE_SPV_BUILD_DIR="' + apple_shader_spv_build_dir + '"',
  ]
  apple_stock_test = executable('apple-stock-shaders',
    'apple-stock-shaders.c',
    include_directories : [include_dir],
    dependencies : apple_stock_deps,
    c_args : apple_stock_cargs,
    build_by_default : true,
  )
  # The apple_spv_files are staged at configure time via configure_file(
  # copy:true) — they already exist by the time this test runs, so no
  # explicit test() depends entry is required. A `meson setup
  # --reconfigure` after running src/shaders/compile_msl.sh is enough
  # to pick up refreshed blobs.
  test('apple stock shaders', apple_stock_test)
endif

# Phase 3.A: Metal-to-Vulkan render encoder smoke. The state-
# machine pass runs on every host; the real-Vulkan record path
# self-skips when no loadable ICD is present. When vulkan_dep is
# not present at configure time the test still builds (it exercises
# the LAGFX_ERR_BACKEND stub path) — hence no vulkan_dep gate here.
# Reaches into src/ for translate/render_encoder.h and the vulkan
# helpers, so it needs internal_include_dir.
if fs.is_file(meson.current_source_dir() / 'translate-render.c')
  translate_render_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    translate_render_deps += [vulkan_dep]
  endif
  translate_render_test = executable('translate-render',
    'translate-render.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : translate_render_deps,
    build_by_default : true,
  )
  test('translate render', translate_render_test)
endif

# Phase 3.C.2 E2E: triangle-lavapipe-e2e. Loads SPIR-V blobs produced by
# examples/triangle/build_spirv.sh (MSL -> metallib -> extract -> retarget
# -> llc + GLSL reference pair for the fallback path) and draws a triangle
# via lavapipe. Gated on vulkan_dep + the test source being present; the
# test itself SKIPs at runtime (exit 77) when LAGFX_TRIANGLE_SPV_DIR is
# not populated, which keeps the Mac dev box (no lavapipe ICD, no llc)
# green.
if fs.is_file(meson.current_source_dir() / 'triangle-lavapipe-e2e.c') \
   and vulkan_dep.found()
  triangle_e2e_test = executable('triangle-lavapipe-e2e',
    'triangle-lavapipe-e2e.c',
    include_directories : include_dir,
    dependencies : [vulkan_dep],
    build_by_default : true,
  )
  test('triangle lavapipe e2e', triangle_e2e_test,
  env : {'LAGFX_TRIANGLE_SPV_DIR':
         meson.project_source_root() / 'examples' / 'triangle' / 'out'})
endif

# Phase 3.C.2: air2spirv (metallib extraction + LLVM Bitcode
# target-triple retarget) smoke. In-process test — exercises the
# parser against both synthesised MTLB fixtures AND a real-bytes
# `default.metallib` copy (when fixtures/default.metallib exists at
# configure time). The real fixture ships at tests/fixtures/ so the
# Linux + Darwin test runs can validate that our MTLB walker
# agrees with Apple's actual container layout (u16 tag lengths,
# entry-size-prefixed records, stage enum 0/1/2 = vertex/fragment/
# kernel) — the kind of format details a synthesised fixture would
# never surface.
# Reaches into src/ for air2spirv/*.h, so it needs internal_include_dir.
if fs.is_file(meson.current_source_dir() / 'air2spirv.c')
  air2spirv_args = [
    '-DLAGFX_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"',
  ]
  if fs.is_file(meson.current_source_dir() / 'fixtures' / 'default.metallib')
    air2spirv_args += [ '-DLAGFX_HAVE_REAL_METALLIB=1' ]
  endif
  air2spirv_test = executable('air2spirv',
    'air2spirv.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    c_args : air2spirv_args,
    build_by_default : true,
  )
  test('air2spirv', air2spirv_test)
endif


# Page-walked translation regression test for commit 3d9cf76.
if fs.is_file(meson.current_source_dir() / 'exec-cmdbuf-translation.c')
  exec_cmdbuf_translation_test = executable('exec-cmdbuf-translation',
    'exec-cmdbuf-translation.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('page-walked translation regression (unit)', exec_cmdbuf_translation_test)
endif

# InfoDecoder reply unit tests. Validates 0x1c2 threadExecutionWidth invariant,
# 0x1c3 NSUInteger long-form fields, and 0x1c6/0x1c7 coordinate pass-through.
if fs.is_file(meson.current_source_dir() / 'info-replies-unit.c')
  info_replies_unit_test = executable('info-replies-unit',
    'info-replies-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('InfoDecoder replies (unit)', info_replies_unit_test)
endif

# Render inner-opcode unit tests. Validates 0x1a DescribeRenderPass parse-and-count,
# 0x65 SetBlendColor float field parsing, and 0x74 SetRenderPipelineState reference resolution.
if fs.is_file(meson.current_source_dir() / 'render-inner-ops-unit.c')
  render_inner_ops_unit_test = executable('render-inner-ops-unit',
    'render-inner-ops-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('Render inner ops (unit)', render_inner_ops_unit_test)
endif

# Blit inner-opcode unit tests. Validates 0x12f CopyFromTextureToTexture parse and
# 0x141 FillTextureWithColor RGBA64 color field parsing.
if fs.is_file(meson.current_source_dir() / 'blit-inner-ops-unit.c')
  blit_inner_ops_unit_test = executable('blit-inner-ops-unit',
    'blit-inner-ops-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('Blit inner ops (unit)', blit_inner_ops_unit_test)
endif

# Phase 3.C.2: shader_translate unit tests. Exercises lagfx_shader_translate_run()
# end-to-end against triangle.metallib fixture. Gated on llc availability at runtime;
# if llc missing, vertex/fragment tests skip (exit 77), other tests still pass.
if fs.is_file(meson.current_source_dir() / 'shader_translate_test.c')
   shader_translate_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     shader_translate_deps += [vulkan_dep]
   endif
   shader_translate_test = executable('shader-translate-test',
     'shader_translate_test.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : shader_translate_deps,
     build_by_default : true,
   )
  test('shader translate (unit)', shader_translate_test,
         workdir : meson.project_source_root())
endif

# Phase 70e: lagfx_pipeline_build unit tests. Exercises pipeline build from
# triangle.metallib shaders via shader_translate -> VkShaderModule -> pipeline.
if fs.is_file(meson.current_source_dir() / 'pipeline_build_test.c')
  pipeline_build_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    pipeline_build_deps += [vulkan_dep]
  endif
  pipeline_build_test = executable('pipeline-build-test',
    'pipeline_build_test.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : pipeline_build_deps,
    build_by_default : true,
  )
  test('pipeline build (unit)', pipeline_build_test,
       workdir : meson.project_source_root())
endif


# Phase 65e: shader_cache unit tests. Exercises LRU eviction, hit/miss stats, and
# stage-distinguishing key generation for the content-addressed SPIR-V cache.
if fs.is_file(meson.current_source_dir() / 'shader_cache_test.c')
    shader_cache_deps = [libapplegfx_vulkan_dep]
    if vulkan_dep.found()
      shader_cache_deps += [vulkan_dep]
    endif
    shader_cache_test = executable('shader-cache-test',
      'shader_cache_test.c',
      include_directories : [include_dir, internal_include_dir],
      dependencies : shader_cache_deps,
      build_by_default : true,
    )
    test('shader cache (unit)', shader_cache_test)
endif

# Phase B object resolver test: empirical validation via LAGFX_PHASE_B_LOOKUP=1
# deploy on phase4 instead of a unit test. The freshman's draft unit test had
# multiple bugs and was removed; the API is correctness-validated by the
# in-vivo MTLB hits documented in phase_b_step5_v2_4_MTLB_CONFIRMED.md.

# Phase 1 clean-room AIR bitcode reader unit tests. Validates wrapper-header
# parsing + invalid-input rejection. Per-block decoder tests land as the
# decoders themselves do (TYPE_BLOCK, PARAMATTR, CONSTANTS, METADATA, etc.).
if fs.is_file(meson.current_source_dir() / 'air-bitcode-reader-unit.c')
  air_reader_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air_reader_deps += [vulkan_dep]
  endif
  air_reader_test = executable('air-bitcode-reader-unit',
    'air-bitcode-reader-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air_reader_deps,
    build_by_default : true,
  )
  test('air bitcode reader (unit)', air_reader_test)
endif

# Phase 3 — METADATA_BLOCK walker. Asserts byte-exact counts +
# specific records against `llvm-bcanalyzer --dump` on triangle.
if fs.is_file(meson.current_source_dir() / 'air-bitcode-reader-metadata-unit.c')
  air_md_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air_md_deps += [vulkan_dep]
  endif
  air_md_test = executable('air-bitcode-reader-metadata-unit',
    'air-bitcode-reader-metadata-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air_md_deps,
    build_by_default : true,
  )
  test('air bitcode reader metadata (unit)', air_md_test)
endif

# Phase 4 reference SPIR-V emitter — air.position output side.
# Validates the spv_builder scaffolding produces a Vulkan-acceptable
# SPIR-V module with a BuiltIn Position output.
if fs.is_file(meson.current_source_dir() / 'air2spv-position-emit-unit.c')
  air2spv_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_test_deps += [vulkan_dep]
  endif
  air2spv_pos_test = executable('air2spv-position-emit-unit',
    'air2spv-position-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_test_deps,
    build_by_default : true,
  )
  test('air2spv position emit (unit)', air2spv_pos_test)
endif

# Phase 4 reference SPIR-V emitter — air.vertex_id input + Position
# output. Validates the input-side builtin pattern + load/convert/
# composite-construct ops.
if fs.is_file(meson.current_source_dir() / 'air2spv-vertex-id-emit-unit.c')
  air2spv_vid_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_vid_test_deps += [vulkan_dep]
  endif
  air2spv_vid_test = executable('air2spv-vertex-id-emit-unit',
    'air2spv-vertex-id-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_vid_test_deps,
    build_by_default : true,
  )
  test('air2spv vertex_id emit (unit)', air2spv_vid_test)
endif

# Phase 4 reference SPIR-V emitter — air.render_target (fragment stage).
# Validates the fragment-stage pattern + OpExecutionMode + Location
# decoration (vs BuiltIn for the other two emitters).
if fs.is_file(meson.current_source_dir() / 'air2spv-render-target-emit-unit.c')
  air2spv_rt_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_rt_test_deps += [vulkan_dep]
  endif
  air2spv_rt_test = executable('air2spv-render-target-emit-unit',
    'air2spv-render-target-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_rt_test_deps,
    build_by_default : true,
  )
  test('air2spv render_target emit (unit)', air2spv_rt_test)
endif

# Phase 4 reference SPIR-V emitter — air.fragment_input (fragment input).
# Validates fragment-stage INPUT variable with Location decoration +
# OpCompositeExtract/OpCompositeConstruct pattern for vec2->vec4 conversion.
if fs.is_file(meson.current_source_dir() / 'air2spv-fragment-input-emit-unit.c')
  air2spv_fi_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_fi_test_deps += [vulkan_dep]
  endif
  air2spv_fi_test = executable('air2spv-fragment-input-emit-unit',
    'air2spv-fragment-input-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_fi_test_deps,
    build_by_default : true,
  )
  test('air2spv fragment_input emit (unit)', air2spv_fi_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern E (Uniform-storage-class
# buffer load). Exercises OpAccessChain + OpLoad on a uniform-buffer
 # variable, OpFMul, OpTypeStruct + DescriptorSet/Binding/Block/Offset
 # decorations.
 if fs.is_file(meson.current_source_dir() / 'air2spv-buffer-load-emit-unit.c')
   air2spv_bl_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_bl_test_deps += [vulkan_dep]
   endif
   air2spv_bl_test = executable('air2spv-buffer-load-emit-unit',
     'air2spv-buffer-load-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_bl_test_deps,
     build_by_default : true,
   )
   test('air2spv buffer_load emit (unit)', air2spv_bl_test)
 endif

 # Phase 4 reference SPIR-V emitter — binary ALU ops (OpFAdd, OpFSub, OpFDiv, OpDot).
 # Exercises chained vec4 arithmetic plus scalar dot product.
 if fs.is_file(meson.current_source_dir() / 'air2spv-alu-binops-emit-unit.c')
   air2spv_ab_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_ab_test_deps += [vulkan_dep]
   endif
   air2spv_ab_test = executable('air2spv-alu-binops-emit-unit',
     'air2spv-alu-binops-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_ab_test_deps,
     build_by_default : true,
   )
   test('air2spv alu_binops emit (unit)', air2spv_ab_test)
 endif

# Phase 4 reference SPIR-V emitter — multi-distinct-float
# OpConstantComposite (clear-color shape).
if fs.is_file(meson.current_source_dir() / 'air2spv-constant-float-emit-unit.c')
  air2spv_cf_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_cf_test_deps += [vulkan_dep]
  endif
  air2spv_cf_test = executable('air2spv-constant-float-emit-unit',
    'air2spv-constant-float-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_cf_test_deps,
    build_by_default : true,
  )
  test('air2spv constant_float emit (unit)', air2spv_cf_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern F (separate image +
# sampler texture-sample). OpTypeImage / OpTypeSampler /
# OpTypeSampledImage / OpSampledImage / OpImageSampleImplicitLod.
if fs.is_file(meson.current_source_dir() / 'air2spv-texture-sample-emit-unit.c')
  air2spv_ts_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_ts_test_deps += [vulkan_dep]
  endif
  air2spv_ts_test = executable('air2spv-texture-sample-emit-unit',
    'air2spv-texture-sample-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_ts_test_deps,
    build_by_default : true,
  )
  test('air2spv texture_sample emit (unit)', air2spv_ts_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern G (OpExtInst GLSL.std.450).
# Single emitter unlocks every air.* math intrinsic family.
if fs.is_file(meson.current_source_dir() / 'air2spv-extinst-glsl-emit-unit.c')
   air2spv_ext_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_ext_test_deps += [vulkan_dep]
   endif
   air2spv_ext_test = executable('air2spv-extinst-glsl-emit-unit',
     'air2spv-extinst-glsl-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_ext_test_deps,
     build_by_default : true,
   )
   test('air2spv extinst_glsl emit (unit)', air2spv_ext_test)
endif

# Phase 4 reference SPIR-V emitter — unary float ops (OpFNegate + OpExtInst FAbs).
# Demonstrates both core SPIR-V unary and GLSL.std.450 ExtInst patterns.
if fs.is_file(meson.current_source_dir() / 'air2spv-unop-emit-unit.c')
   air2spv_unop_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_unop_test_deps += [vulkan_dep]
   endif
   air2spv_unop_test = executable('air2spv-unop-emit-unit',
     'air2spv-unop-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_unop_test_deps,
     build_by_default : true,
   )
   test('air2spv unop emit (unit)', air2spv_unop_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern H (OpVectorShuffle).
if fs.is_file(meson.current_source_dir() / 'air2spv-vector-shuffle-emit-unit.c')
  air2spv_sh_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_sh_test_deps += [vulkan_dep]
  endif
  air2spv_sh_test = executable('air2spv-vector-shuffle-emit-unit',
    'air2spv-vector-shuffle-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_sh_test_deps,
    build_by_default : true,
  )
  test('air2spv vector_shuffle emit (unit)', air2spv_sh_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern I (structured control flow).
# OpSelectionMerge + OpBranchConditional + OpPhi.
if fs.is_file(meson.current_source_dir() / 'air2spv-control-flow-emit-unit.c')
  air2spv_cf2_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_cf2_test_deps += [vulkan_dep]
  endif
  air2spv_cf2_test = executable('air2spv-control-flow-emit-unit',
    'air2spv-control-flow-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_cf2_test_deps,
    build_by_default : true,
  )
  test('air2spv control_flow emit (unit)', air2spv_cf2_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern J (structured loop).
# OpLoopMerge + Function-storage OpVariable.
if fs.is_file(meson.current_source_dir() / 'air2spv-loop-emit-unit.c')
  air2spv_lp_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_lp_test_deps += [vulkan_dep]
  endif
  air2spv_lp_test = executable('air2spv-loop-emit-unit',
    'air2spv-loop-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_lp_test_deps,
    build_by_default : true,
  )
  test('air2spv loop emit (unit)', air2spv_lp_test)
endif

# Phase 5 — AIR module → SPIR-V translator skeleton.
 if fs.is_file(meson.current_source_dir() / 'air2spv-translate-module-unit.c')
   air2spv_xl_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_xl_test_deps += [vulkan_dep]
   endif
   air2spv_xl_test = executable('air2spv-translate-module-unit',
     'air2spv-translate-module-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_xl_test_deps,
     build_by_default : true,
   )
   test('air2spv translate module (unit)', air2spv_xl_test)
 endif

 # Phase 5 — AIR BINOP + UNOP dispatcher unit test.
 if fs.is_file(meson.current_source_dir() / 'air2spv-translate-function-binop-unop-unit.c')
   air2spv_binop_unop_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_binop_unop_deps += [vulkan_dep]
   endif
   air2spv_binop_unop_test = executable('air2spv-translate-function-binop-unop-unit',
     'air2spv-translate-function-binop-unop-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_binop_unop_deps,
     build_by_default : true,
   )
   test('air2spv translate function binop unop (unit)', air2spv_binop_unop_test)
 endif

# Compile-time opcode pinning — guards the §3.32.15 relational + §3.32.14
# shift/bitwise opcode constants against the canonical SPIR-V numbers.
if fs.is_file(meson.current_source_dir() / 'air2spv-relational-opcodes-unit.c')
  air2spv_relop_test = executable('air2spv-relational-opcodes-unit',
    'air2spv-relational-opcodes-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('air2spv relational opcode pinning (unit)', air2spv_relop_test)
endif

# Blocker A regression: a non-void CALL (texture sample) must be counted
# as a value producer so the downstream FADD resolves float types and
# emits OpFAdd, not OpIAdd. Feeds a real compiled fragment shader fixture
# through the translator and asserts the opcode + spirv-val cleanliness.
if fs.is_file(meson.current_source_dir() / 'air2spv-nonvoid-call-valuenumber-unit.c')
  air2spv_nvcall_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_nvcall_deps += [vulkan_dep]
  endif
  air2spv_nvcall_test = executable('air2spv-nonvoid-call-valuenumber-unit',
    'air2spv-nonvoid-call-valuenumber-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_nvcall_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv non-void CALL value-numbering (unit)', air2spv_nvcall_test,
    workdir : meson.current_source_dir() / '..')
endif

# Structured control flow: a counted for-loop must lower to multi-basic-block
# SPIR-V (OpLoopMerge + OpPhi + OpBranchConditional), not a single linear
# block with the loop body dropped after OpReturn.
if fs.is_file(meson.current_source_dir() / 'air2spv-structured-loop-unit.c')
  air2spv_loop_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_loop_deps += [vulkan_dep]
  endif
  air2spv_loop_test = executable('air2spv-structured-loop-unit',
    'air2spv-structured-loop-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_loop_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv structured loop (unit)', air2spv_loop_test,
    workdir : meson.current_source_dir() / '..')
endif

# Structured control flow, guarded variant: a `while (cond) {...}` (or a
# for-loop with a runtime trip-count guard) lowers to a 4-block guarded
# rotated loop — an ENTRY GUARD (conditional br → {header, merge}) plus
# LOOP-CLOSING PHIs in the merge block. Must emit OpSelectionMerge (guard)
# + OpLoopMerge + the merge-closing OpPhi, not fall to the linear path.
if fs.is_file(meson.current_source_dir() / 'air2spv-guarded-loop-unit.c')
  air2spv_gloop_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_gloop_deps += [vulkan_dep]
  endif
  air2spv_gloop_test = executable('air2spv-guarded-loop-unit',
    'air2spv-guarded-loop-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_gloop_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv guarded loop (unit)', air2spv_gloop_test,
    workdir : meson.current_source_dir() / '..')
endif

# SPIR-V §2.4 section-ordering invariant: no OpType*/OpConstant* may
# follow the first OpFunction. Guards against handlers (CMP/SELECT,
# non-void CALL, …) emitting a stray type/constant mid-body.
if fs.is_file(meson.current_source_dir() / 'air2spv-type-section-ordering-unit.c')
  air2spv_secord_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_secord_deps += [vulkan_dep]
  endif
  air2spv_secord_test = executable('air2spv-type-section-ordering-unit',
    'air2spv-type-section-ordering-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_secord_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv type/constant section ordering (unit)', air2spv_secord_test,
    workdir : meson.current_source_dir() / '..')
endif

# Login-screen SkyLight compositor corpus: the 7 real AIR shaders the
# macOS 15.7.5 login screen submits (captured live, pipelines 0xd/0x14/
# 0x1d/0x20/0x23/0x28/0x2c). Each must translate valid + role-correct —
# texture fragments must keep their OpImageSampleImplicitLod (dropped
# sample = black wallpaper), vertices must keep BuiltIn Position. Guards
# the wallpaper/composite path end-to-end against silent mistranslation.
if fs.is_file(meson.current_source_dir() / 'air2spv-login-corpus-unit.c')
  air2spv_login_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_login_deps += [vulkan_dep]
  endif
  air2spv_login_test = executable('air2spv-login-corpus-unit',
    'air2spv-login-corpus-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_login_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv login-screen shader corpus (unit)', air2spv_login_test,
    workdir : meson.current_source_dir() / '..')
endif

# AIR-type-index-0-vs-unknown sentinel: float (type index 0) was read as
# "unknown", mis-dispatching CMP/SELECT/BINOP. Real compositor shader
# (clear vertex) must emit a typed OpSelect, not drop or mis-type it.
if fs.is_file(meson.current_source_dir() / 'air2spv-local-const-type-unit.c')
  air2spv_lconst_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_lconst_deps += [vulkan_dep]
  endif
  air2spv_lconst_test = executable('air2spv-local-const-type-unit',
    'air2spv-local-const-type-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_lconst_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv local-const operand type (unit)', air2spv_lconst_test,
    workdir : meson.current_source_dir() / '..')
endif

# Vertex-input ABI: stage-in attributes (vec params) must be typed
# Location Input variables, not gl_VertexIndex. Real compositor shader
# (color_fill vertex) feeds an attribute into an OpVectorShuffle.
if fs.is_file(meson.current_source_dir() / 'air2spv-vertex-attribute-input-unit.c')
  air2spv_vattr_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_vattr_deps += [vulkan_dep]
  endif
  air2spv_vattr_test = executable('air2spv-vertex-attribute-input-unit',
    'air2spv-vertex-attribute-input-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_vattr_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv vertex attribute input (unit)', air2spv_vattr_test,
    workdir : meson.current_source_dir() / '..')
endif

# CALL_FMF (fast-math-flags) operand skip + type-index-0 return type. A
# fast-math conversion CALL must propagate its float result type so a
# downstream float multiply dispatches as OpFMul, not OpIMul.
if fs.is_file(meson.current_source_dir() / 'air2spv-call-fmf-unit.c')
  air2spv_fmf_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_fmf_deps += [vulkan_dep]
  endif
  air2spv_fmf_test = executable('air2spv-call-fmf-unit',
    'air2spv-call-fmf-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_fmf_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv CALL_FMF operand skip (unit)', air2spv_fmf_test,
    workdir : meson.current_source_dir() / '..')
endif

# Fragment RET → Location-0 colour-output store regression guard.
if fs.is_file(meson.current_source_dir() / 'air2spv-fragment-color-store-unit.c')
  air2spv_fcs_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_fcs_deps += [vulkan_dep]
  endif
  air2spv_fcs_test = executable('air2spv-fragment-color-store-unit',
    'air2spv-fragment-color-store-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_fcs_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv fragment colour-output store (unit)', air2spv_fcs_test,
    workdir : meson.current_source_dir() / '..')
endif

# air.convert.* numeric-conversion lowering (Stage-80 black-screen guard).
if fs.is_file(meson.current_source_dir() / 'air2spv-air-convert-uitofp-unit.c')
  air2spv_cvt_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_cvt_deps += [vulkan_dep]
  endif
  air2spv_cvt_test = executable('air2spv-air-convert-uitofp-unit',
    'air2spv-air-convert-uitofp-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_cvt_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv air.convert uitofp lowering (unit)', air2spv_cvt_test,
    workdir : meson.current_source_dir() / '..')
endif

# Real-Apple math-intrinsic naming (air.<op>/air.fast_<op>) lowering guard.
if fs.is_file(meson.current_source_dir() / 'air2spv-air-math-intrinsics-unit.c')
  air2spv_math_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_math_deps += [vulkan_dep]
  endif
  air2spv_math_test = executable('air2spv-air-math-intrinsics-unit',
    'air2spv-air-math-intrinsics-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_math_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv real-Apple math intrinsics (unit)', air2spv_math_test,
    workdir : meson.current_source_dir() / '..')
endif

# Vector result-type consistency (composite-extract / shuffle-width / binop-type).
if fs.is_file(meson.current_source_dir() / 'air2spv-vector-typing-unit.c')
  air2spv_vty_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_vty_deps += [vulkan_dep]
  endif
  air2spv_vty_test = executable('air2spv-vector-typing-unit',
    'air2spv-vector-typing-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_vty_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv vector result-type consistency (unit)', air2spv_vty_test,
    workdir : meson.current_source_dir() / '..')
endif

# Fragment stage_in inputs + extractelement element-type-index-0 fix.
if fs.is_file(meson.current_source_dir() / 'air2spv-fragment-stagein-unit.c')
  air2spv_fsi_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_fsi_deps += [vulkan_dep]
  endif
  air2spv_fsi_test = executable('air2spv-fragment-stagein-unit',
    'air2spv-fragment-stagein-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_fsi_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv fragment stage_in inputs (unit)', air2spv_fsi_test,
    workdir : meson.current_source_dir() / '..')
endif

# Fragment stage linker: VS output / FS input type mismatch, missing VS output, unlinked, disabled.
if fs.is_file(meson.current_source_dir() / 'air2spv-stage-link-unit.c')
  air2spv_sl_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_sl_deps += [vulkan_dep]
  endif
  air2spv_sl_test = executable('air2spv-stage-link-unit',
    'air2spv-stage-link-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_sl_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv stage linker (unit)', air2spv_sl_test,
    workdir : meson.current_source_dir() / '..')
endif

# air.sample_texture_2d -> real OpImageSampleImplicitLod lowering.
if fs.is_file(meson.current_source_dir() / 'air2spv-texture-sample-translate-unit.c')
  air2spv_txs_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_txs_deps += [vulkan_dep]
  endif
  air2spv_txs_test = executable('air2spv-texture-sample-translate-unit',
    'air2spv-texture-sample-translate-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_txs_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv texture sample translate (unit)', air2spv_txs_test,
    workdir : meson.current_source_dir() / '..')
endif

# air.sample_texture_3d -> Dim3D image sampled with a 3-component coordinate (real device module A2Xghfc).
air2spv_t3d_test = executable('air2spv-texture3d-translate-unit',
  'air2spv-texture3d-translate-unit.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : [libapplegfx_vulkan_dep],
  c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
  build_by_default : true,
)
test('air2spv texture3d translate (unit)', air2spv_t3d_test, workdir : meson.current_source_dir() / '..')

# [[buffer(n)]] argument -> StorageBuffer Block (std430) lowering.
if fs.is_file(meson.current_source_dir() / 'air2spv-buffer-arg-unit.c')
  air2spv_bua_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_bua_deps += [vulkan_dep]
  endif
  air2spv_bua_test = executable('air2spv-buffer-arg-unit',
    'air2spv-buffer-arg-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_bua_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv buffer arg storage block (unit)', air2spv_bua_test,
    workdir : meson.current_source_dir() / '..')
endif

# Nested Block struct layout — Offset/ArrayStride on transitively-reachable
# aggregates inside a [[buffer(n)]] Block (SkyLight SimpleTextureLightingVertex).
if fs.is_file(meson.current_source_dir() / 'air2spv-nested-block-offset-unit.c')
  air2spv_nbo_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_nbo_deps += [vulkan_dep]
  endif
  air2spv_nbo_test = executable('air2spv-nested-block-offset-unit',
    'air2spv-nested-block-offset-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_nbo_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv nested block offset layout (unit)', air2spv_nbo_test,
    workdir : meson.current_source_dir() / '..')
endif

# Non-struct [[buffer(n)]] storage-class correctness — real macOS-guest
# SkyLight shaders (ViewportToNDC vertex / ColorFill fragment) whose buffer
# pointee is an UNWRAPPED array/vector (device packed_float2* / device float4*).
# Guards against: mis-classifying them as textures, and lowering their
# GEP/load to a Function-class OpAccessChain into a StorageBuffer var.
if fs.is_file(meson.current_source_dir() / 'air2spv-buffer-storage-class-unit.c')
  air2spv_bsc_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_bsc_deps += [vulkan_dep]
  endif
  air2spv_bsc_test = executable('air2spv-buffer-storage-class-unit',
    'air2spv-buffer-storage-class-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_bsc_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv non-struct buffer storage class (unit)', air2spv_bsc_test,
    workdir : meson.current_source_dir() / '..')
endif

# CALL callee value-id base (functions enumerate after globalvars) +
# fragment-output typing from the return type (SkyLight SimpleTextureFragmentUV).
if fs.is_file(meson.current_source_dir() / 'air2spv-call-globalvar-base-unit.c')
  air2spv_cgb_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_cgb_deps += [vulkan_dep]
  endif
  air2spv_cgb_test = executable('air2spv-call-globalvar-base-unit',
    'air2spv-call-globalvar-base-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_cgb_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv call globalvar base + frag output type (unit)', air2spv_cgb_test,
    workdir : meson.current_source_dir() / '..')
endif

# Multi-render-target struct-return fragment (SkyLight ColorFillYCbCr).
if fs.is_file(meson.current_source_dir() / 'air2spv-multi-render-target-unit.c')
  air2spv_mrt_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_mrt_deps += [vulkan_dep]
  endif
  air2spv_mrt_test = executable('air2spv-multi-render-target-unit',
    'air2spv-multi-render-target-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_mrt_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv multi render target (unit)', air2spv_mrt_test,
    workdir : meson.current_source_dir() / '..')
endif

# SPIR-V descriptor-binding reflection (host descriptor-set-layout source).
if fs.is_file(meson.current_source_dir() / 'air2spv-spv-reflect-bindings-unit.c')
  air2spv_refl_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_refl_deps += [vulkan_dep]
  endif
  air2spv_refl_test = executable('air2spv-spv-reflect-bindings-unit',
    'air2spv-spv-reflect-bindings-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_refl_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv spv reflect bindings (unit)', air2spv_refl_test,
    workdir : meson.current_source_dir() / '..')
endif

# Phase 4 reference SPIR-V emitter — Pattern K (storage-image OpImageRead / OpImageWrite).
if fs.is_file(meson.current_source_dir() / 'air2spv-storage-image-emit-unit.c')
  air2spv_si_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_si_test_deps += [vulkan_dep]
  endif
  air2spv_si_test = executable('air2spv-storage-image-emit-unit',
    'air2spv-storage-image-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_si_test_deps,
    build_by_default : true,
  )
  test('air2spv storage_image emit (unit)', air2spv_si_test)
endif

# --- Coverage-corpus regression guards (2026-05-30, xcrun-metal corpus) ---

# `half` (IEEE-754 binary16) type + half arithmetic lowering guard.
if fs.is_file(meson.current_source_dir() / 'air2spv-half-type-unit.c')
  air2spv_half_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_half_deps += [vulkan_dep]
  endif
  air2spv_half_test = executable('air2spv-half-type-unit',
    'air2spv-half-type-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_half_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv half type (unit)', air2spv_half_test,
    workdir : meson.current_source_dir() / '..')
endif

# bool(i1) -> float convert via OpSelect (step()/comparison-to-float) guard.
if fs.is_file(meson.current_source_dir() / 'air2spv-bool-to-float-unit.c')
  air2spv_b2f_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_b2f_deps += [vulkan_dep]
  endif
  air2spv_b2f_test = executable('air2spv-bool-to-float-unit',
    'air2spv-bool-to-float-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_b2f_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv bool to float (unit)', air2spv_b2f_test,
    workdir : meson.current_source_dir() / '..')
endif

# numeric bitcast (`as_type`) -> real OpBitcast (not silent alias) guard.
if fs.is_file(meson.current_source_dir() / 'air2spv-bitcast-as-type-unit.c')
  air2spv_bc_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_bc_deps += [vulkan_dep]
  endif
  air2spv_bc_test = executable('air2spv-bitcast-as-type-unit',
    'air2spv-bitcast-as-type-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_bc_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv bitcast as_type (unit)', air2spv_bc_test,
    workdir : meson.current_source_dir() / '..')
endif



# Ring-wrap header read: a 12-byte header straddling a ring-page boundary
# across NON-contiguous physical pages must resolve each chunk via the
# PFN-array (red-on-revert guard for the flat-read drain-stop fix).
if fs.is_file(meson.current_source_dir() / 'ring-wrap-header-unit.c')
  ring_wrap_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    ring_wrap_deps += [vulkan_dep]
  endif
  ring_wrap_test = executable('ring-wrap-header-unit',
    'ring-wrap-header-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : ring_wrap_deps,
    build_by_default : true,
  )
  test('ring-wrap header read (unit)', ring_wrap_test)
endif

if fs.is_file(meson.current_source_dir() / 'pfn-shift-unit.c')
  pfn_shift_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    pfn_shift_deps += [vulkan_dep]
  endif
  pfn_shift_test = executable('pfn-shift-unit',
    'pfn-shift-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : pfn_shift_deps,
    build_by_default : true,
  )
  test('PFN shift 12/14 (unit)', pfn_shift_test)
endif

if fs.is_file(meson.current_source_dir() / 'task-define-unit.c')
  task_define_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    task_define_deps += [vulkan_dep]
  endif
  task_define_test = executable('task-define-unit',
    'task-define-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : task_define_deps,
    build_by_default : true,
  )
  test('compute task define 0x38/0x44 (unit)', task_define_test)
endif

if fs.is_file(meson.current_source_dir() / 'diag-unit.c')
  diag_deps = [libapplegfx_vulkan_dep, dependency('threads')]
  if vulkan_dep.found()
    diag_deps += [vulkan_dep]
  endif
  diag_test = executable('diag-unit',
    'diag-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : diag_deps,
    build_by_default : true,
  )
  test('command diagnostics (unit)', diag_test)
endif

if fs.is_file(meson.current_source_dir() / 'draw-diag-unit.c')
  draw_diag_test = executable('draw-diag-unit',
    'draw-diag-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : diag_deps,
    build_by_default : true,
  )
  test('bounded per-draw diagnostics (unit)', draw_diag_test)
endif

if fs.is_file(meson.current_source_dir() / 'spv-glsl-std450-numbers-unit.c')
  glsl450_num_test = executable('spv-glsl-std450-numbers-unit',
    'spv-glsl-std450-numbers-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('GLSL.std.450 instruction numbers vs Khronos header (unit)', glsl450_num_test)
endif

if fs.is_file(meson.current_source_dir() / 'air2spv-glsl-intrinsic-table-unit.c')
  glsl_table_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    glsl_table_deps += [vulkan_dep]
  endif
  glsl_table_test = executable('air2spv-glsl-intrinsic-table-unit',
    'air2spv-glsl-intrinsic-table-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : glsl_table_deps,
    build_by_default : true,
  )
  test('air2spv GLSL.std.450 intrinsic table audit (unit)', glsl_table_test)
endif

if fs.is_file(meson.current_source_dir() / 'metal-format-map-unit.c') and vulkan_dep.found()
  metal_fmt_test = executable('metal-format-map-unit',
    'metal-format-map-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('Metal pixel / vertex format tables (unit)', metal_fmt_test)
endif

if fs.is_file(meson.current_source_dir() / 'vertex-input-unknown-fmt-unit.c') and vulkan_dep.found()
  vtx_unk_fmt_test = executable('vertex-input-unknown-fmt-unit',
    'vertex-input-unknown-fmt-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vertex input: unknown Metal format is per-attribute (unit)', vtx_unk_fmt_test)
endif

if fs.is_file(meson.current_source_dir() / 'vertex-fetch-offset-pairing-unit.c') and vulkan_dep.found()
  vfetch_pair_test = executable('vertex-fetch-offset-pairing-unit',
    'vertex-fetch-offset-pairing-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DLAGFX_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"'],
    build_by_default : true,
  )
  test('vertex fetch wrapper: source offsets pair stage-in inputs with PSO attributes (unit)', vfetch_pair_test)
endif

if fs.is_file(meson.current_source_dir() / 'arm64-vertex-desc-real-unit.c') and vulkan_dep.found()
  vdesc_real_test = executable('arm64-vertex-desc-real-unit',
    'arm64-vertex-desc-real-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DLAGFX_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"'],
    build_by_default : true,
  )
  test('arm64 vertex descriptor: real device-captured PSOs -> attributes / stride / Vulkan input (unit)', vdesc_real_test)
endif

# Vertex float-plausibility scorer: text/poison score ~0, real floats ~100.
if fs.is_file(meson.current_source_dir() / 'vtx-plausibility-unit.c') and vulkan_dep.found()
  vtx_plaus_test = executable('vtx-plausibility-unit',
    'vtx-plausibility-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vtx float plausibility (unit)', vtx_plaus_test)
endif

# AIR arg-metadata Metal-index map: the authoritative binding→slot map
# (air.location_index per resource arg). Guards the M2v fix — the Metal
# buffer index varies per shader (UberComposite mvp=1, Vfx=2, ViewportToNDC
# 0/1/2), so heuristics can't substitute for this decode.
if fs.is_file(meson.current_source_dir() / 'air2spv-arg-bindings-unit.c')
  air2spv_argbind_test = executable('air2spv-arg-bindings-unit',
    'air2spv-arg-bindings-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv arg-metadata metal-index map (unit)', air2spv_argbind_test,
    workdir : meson.current_source_dir() / '..')
endif

# PGCmdSetBufferOffset (0x6f/0x7e) wire-format: [index:u32@0][offset:u64@4]
# per Apple's decoder disasm. Red-on-revert guard for the swapped-field
# parse that zeroed the Xgc backdrop draws' vertex stream (2026-07-23).
if fs.is_file(meson.current_source_dir() / 'compute-inner-buffer-offset-unit.c')
  buf_off_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    buf_off_deps += [vulkan_dep]
  endif
  buf_off_test = executable('compute-inner-buffer-offset-unit',
    'compute-inner-buffer-offset-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : buf_off_deps,
    build_by_default : true,
  )
  test('PGCmdSetBufferOffset field order (unit)', buf_off_test)
endif

if fs.is_file(meson.current_source_dir() / 'protocol-mode-unit.c')
  pm_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    pm_deps += [vulkan_dep]
  endif
  pm_test = executable('protocol-mode-unit',
    'protocol-mode-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : pm_deps,
    build_by_default : true,
  )
  test('protocol mode x86/arm64 compute semantics (unit)', pm_test)
endif

if fs.is_file(meson.current_source_dir() / 'arm64-exec-decode-unit.c')
  ed_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    ed_deps += [vulkan_dep]
  endif
  ed_test = executable('arm64-exec-decode-unit',
    'arm64-exec-decode-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : ed_deps,
    build_by_default : true,
  )
  test('arm64 exec 0x43/0x45 decode (unit)', ed_test)
endif

# ARM64 deferred stamp completion queue (CPU only, mock GPU work) and the compute drain's behaviour with it off / on.
if fs.is_file(meson.current_source_dir() / 'arm64-completion-unit.c')
  ac_deps = [libapplegfx_vulkan_dep, dependency('threads')]
  if vulkan_dep.found()
    ac_deps += [vulkan_dep]
  endif
  ac_test = executable('arm64-completion-unit',
    'arm64-completion-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : ac_deps,
    build_by_default : true,
  )
  test('arm64 deferred stamp completion (unit)', ac_test)
endif

# (F+D) fence-tracked completion + guest-surface writeback on lavapipe (16 KiB page surface, row stride).
if fs.is_file(meson.current_source_dir() / 'arm64-surface-writeback-unit.c')
  sw_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    sw_deps += [vulkan_dep]
  endif
  sw_test = executable('arm64-surface-writeback-unit',
    'arm64-surface-writeback-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : sw_deps,
    build_by_default : true,
  )
  test('arm64 completion fence + guest surface writeback (lavapipe)', sw_test)
endif

# Display diagnostics through lagfx_dd_note_cmd: op 0x0a identity classification, gamma-region task choice (no Vulkan device).
if fs.is_file(meson.current_source_dir() / 'display-diag-gamma-unit.c')
  display_diag_gamma_test = executable('display-diag-gamma-unit',
    'display-diag-gamma-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    build_by_default : true,
  )
  test('display diagnostics: op 0x0a classification + gamma region task (unit)', display_diag_gamma_test)
endif

# LAGFX_WB_VERIFY read-only writeback comparison (no Vulkan: fake guest memory).
if fs.is_file(meson.current_source_dir() / 'arm64-wb-verify-unit.c')
  wbv_test = executable('arm64-wb-verify-unit',
    'arm64-wb-verify-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    build_by_default : true,
  )
  test('arm64 writeback verify diagnostic (unit)', wbv_test)
endif

# ARM64 render-segment execution (LAGFX_ARM64_EXEC=1): (task, ref) object table lifetime + real Vulkan recording read back on lavapipe.
if fs.is_file(meson.current_source_dir() / 'arm64-exec-run-unit.c')
  er_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    er_deps += [vulkan_dep]
  endif
  er_test = executable('arm64-exec-run-unit',
    'arm64-exec-run-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : er_deps,
    build_by_default : true,
  )
  test('arm64 exec render execution (unit, lavapipe)', er_test, env : ['LAGFX_VK_VALIDATION=1'])
  test('arm64 exec render, local read forced off (unit, lavapipe)', er_test, env : ['LAGFX_VK_VALIDATION=1', 'LAGFX_VK_NO_LOCAL_READ=1'])
  test('arm64 exec render, raster order forced off (unit, lavapipe)', er_test, env : ['LAGFX_VK_VALIDATION=1', 'LAGFX_VK_NO_RASTER_ORDER=1'])
endif

# ARM64 render draw record layouts + serialized sampler decoding (pure, no device).
if fs.is_file(meson.current_source_dir() / 'arm64-draw-decode-unit.c')
  dd_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    dd_deps += [vulkan_dep]
  endif
  dd_test = executable('arm64-draw-decode-unit',
    'arm64-draw-decode-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : dd_deps,
    build_by_default : true,
  )
  test('arm64 draw record + sampler decoding (unit)', dd_test)
endif

# ARM64 render exec extensions (draw variants, indirect, no-op tags, LOD samplers) on lavapipe + validation.
if fs.is_file(meson.current_source_dir() / 'arm64-exec-ext-unit.c') and vulkan_dep.found()
  ee_test = executable('arm64-exec-ext-unit',
    'arm64-exec-ext-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('arm64 exec render extensions (unit, lavapipe)', ee_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

# sRGB colour target numeric check (format 81 vs 80 control) on lavapipe + validation.
if fs.is_file(meson.current_source_dir() / 'arm64-exec-gamma-unit.c') and vulkan_dep.found()
  gm_test = executable('arm64-exec-gamma-unit',
    'arm64-exec-gamma-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep, meson.get_compiler('c').find_library('m', required : false)],
    build_by_default : true,
  )
  test('arm64 exec sRGB render target numeric (unit, lavapipe)', gm_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

# Vertex fetch / index handling against a CPU model (indexed offsets, base vertex, vertex pulling at gathered offsets, real compositor vertex layout) on lavapipe.
if fs.is_file(meson.current_source_dir() / 'arm64-vertex-index-unit.c') and vulkan_dep.found()
  vi_test = executable('arm64-vertex-index-unit',
    'arm64-vertex-index-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('arm64 vertex fetch / index handling vs CPU model (unit, lavapipe)', vi_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

if fs.is_file(meson.current_source_dir() / 'arm64-exec-blit-unit.c') and vulkan_dep.found()
  eb_test = executable('arm64-exec-blit-unit',
    'arm64-exec-blit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('arm64 exec blit + resource flags on lavapipe (unit)', eb_test, timeout : 120)
endif

# ARM64 exec integration (Render + Blit in one exec, shared mirror, write-back, PENDING stamp, deferred release) on lavapipe + validation.
if fs.is_file(meson.current_source_dir() / 'arm64-exec-integ-unit.c') and vulkan_dep.found()
  ei_test = executable('arm64-exec-integ-unit',
    'arm64-exec-integ-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep, dependency('threads')],
    build_by_default : true,
  )
  test('arm64 exec render+blit integration (lavapipe, validation)', ei_test, env : ['LAGFX_VK_VALIDATION=1'], timeout : 120)
endif

# ARM64 type-11 mapped textures (guest display surfaces through the shell's get_surface_desc) on lavapipe + validation.
if fs.is_file(meson.current_source_dir() / 'arm64-mapped-unit.c') and vulkan_dep.found()
  mp_test = executable('arm64-mapped-unit',
    'arm64-mapped-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep, dependency('threads')],
    build_by_default : true,
  )
  test('arm64 mapped textures (type 11) over guest surfaces (lavapipe, validation)', mp_test, env : ['LAGFX_VK_VALIDATION=1'], timeout : 120)
endif

# Pipeline-build failure diagnostics (log once per failure key, failing SPIR-V capture) on lavapipe.
if fs.is_file(meson.current_source_dir() / 'arm64-pipefail-unit.c') and vulkan_dep.found()
  pf_test = executable('arm64-pipefail-unit',
    'arm64-pipefail-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep, dependency('threads')],
    build_by_default : true,
  )
  test('pipeline build failure diagnostics + SPIR-V capture (lavapipe)', pf_test, timeout : 120)
endif

# ARM64 pipeline resolver + descriptor binding + topology + cache invalidation + real-data capture (lavapipe; honest SKIP without fixtures / ICD).
if fs.is_file(meson.current_source_dir() / 'arm64-pipe-unit.c')
  pu_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    pu_deps += [vulkan_dep]
  endif
  pu_test = executable('arm64-pipe-unit',
    'arm64-pipe-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : pu_deps,
    c_args : ['-DLAGFX_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"'],
    build_by_default : true,
  )
  test('arm64 pipeline resolver / bindings / invalidation / capture (lavapipe)', pu_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

# ARM64 tile shaders (tag 0x33 pipeline, 0x9d / 0xa1 binds, 0x9b / 0xa2 dispatch) on lavapipe: real tile_simd_blur_15, every pixel vs a CPU model.
if fs.is_file(meson.current_source_dir() / 'arm64-tile-unit.c') and vulkan_dep.found()
  tile_unit_test = executable('arm64-tile-unit',
    'arm64-tile-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DLAGFX_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"'],
    build_by_default : true,
  )
  test('arm64 tile shader pipeline + dispatch between render pass halves (lavapipe)', tile_unit_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

# ARM64: tile-path and graphics-path storage-buffer gathers share one exec's 64 scratch slots (under / at / over the cap, reuse across execs) on lavapipe.
if fs.is_file(meson.current_source_dir() / 'arm64-gather-mix-unit.c') and vulkan_dep.found()
  gather_mix_test = executable('arm64-gather-mix-unit',
    'arm64-gather-mix-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DLAGFX_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"'],
    build_by_default : true,
  )
  test('arm64 tile + graphics gathers share the exec scratch slots (lavapipe)', gather_mix_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

# ARM64 depth/stencil state object (serialized tag 4) decoder: pure, synthetic records.
if fs.is_file(meson.current_source_dir() / 'arm64-depth-stencil-unit.c')
  ds_test = executable('arm64-depth-stencil-unit',
    'arm64-depth-stencil-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    build_by_default : true,
  )
  test('arm64 depth/stencil state object decoder (unit)', ds_test)
endif

# Compute stage (air.kernel): tile kernel structure, simd-group ops, float math coverage.
if fs.is_file(meson.current_source_dir() / 'air2spv-compute-unit.c')
  air2spv_compute_test = executable('air2spv-compute-unit',
    'air2spv-compute-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv compute stage + simd + f16 math (unit)', air2spv_compute_test,
    workdir : meson.current_source_dir() / '..')
endif

# The translated tile kernel executed on a Vulkan 1.3 device (lavapipe): compares every pixel with a CPU model. Skips (77) without a device / capability.
if fs.is_file(meson.current_source_dir() / 'air2spv-compute-lavapipe.c') and vulkan_dep.found()
  air2spv_compute_vk_test = executable('air2spv-compute-lavapipe',
    'air2spv-compute-lavapipe.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv translated tile kernel on lavapipe', air2spv_compute_vk_test,
    workdir : meson.current_source_dir() / '..')
endif

# ARM64 render exec: storage images (shader-written textures), vertex-stage textures / samplers, unnormalized samplers, writable storage buffers.
if fs.is_file(meson.current_source_dir() / 'arm64-exec-res-unit.c') and vulkan_dep.found()
  ex_test = executable('arm64-exec-res-unit',
    'arm64-exec-res-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('arm64 exec storage images + vertex textures + unnormalized samplers (unit, lavapipe)', ex_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

# Pure regression test: framebuffer-fetch binding range (fragment offset) and the shared binding limits.
if fs.is_file(meson.current_source_dir() / 'arm64-fbfetch-check-unit.c')
  fbf_check_test = executable('arm64-fbfetch-check-unit', 'arm64-fbfetch-check-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    build_by_default : true)
  test('arm64 framebuffer-fetch binding check', fbf_check_test)
endif

# ARM64 render replay capture (LAGFX_ARM64_DUMP_DATA=1): format round trip through the loader, filters, caps, on lavapipe.
if fs.is_file(meson.current_source_dir() / 'arm64-capture-unit.c') and vulkan_dep.found()
  cap_test = executable('arm64-capture-unit',
    'arm64-capture-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('arm64 render replay capture (unit, lavapipe)', cap_test, env : ['LAGFX_VK_VALIDATION=1'])
endif

if fs.is_file(meson.current_source_dir() / 'arm64-replay-unit.c') and vulkan_dep.found()
  rp_test = executable('arm64-replay-unit',
    'arm64-replay-unit.c',
    include_directories : [include_dir, internal_include_dir, include_directories('../src')],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep, meson.get_compiler('c').find_library('m', required : false)],
    build_by_default : true,
  )
  test('render-replay self tests (unit; scenes need LAGFX_TEST_DUMP)', rp_test, env : ['LAGFX_VK_VALIDATION=1'], timeout : 180)
endif

# i1 / <N x i1> constants and air.any / air.all (fixture i1const_fragment.air.bc), translated and rendered on a Vulkan 1.3 device (lavapipe): every pixel value-checked. Skips (77) without a device.
if fs.is_file(meson.current_source_dir() / 'air2spv-i1-const-lavapipe.c') and vulkan_dep.found()
  air2spv_i1_vk_test = executable('air2spv-i1-const-lavapipe',
    'air2spv-i1-const-lavapipe.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv i1 constants + any/all on lavapipe', air2spv_i1_vk_test,
    workdir : meson.current_source_dir() / '..')
endif

# AIR integer intrinsics (census X11): hand-written AIR fixtures translated, per-op opcode check + spirv-val. Skips (77) without spirv-val.
if fs.is_file(meson.current_source_dir() / 'air2spv-int-intrinsics-unit.c')
  air2spv_int_test = executable('air2spv-int-intrinsics-unit',
    'air2spv-int-intrinsics-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv integer intrinsics (unit)', air2spv_int_test,
    workdir : meson.current_source_dir() / '..')
endif

# air.sample_texture_{2d,3d} option operands (census X04): bias / explicit level / constant offset / min_lod, checked per call at the SPIR-V level + spirv-val.
if fs.is_file(meson.current_source_dir() / 'air2spv-texture-sample-options-unit.c')
  air2spv_tso_test = executable('air2spv-texture-sample-options-unit',
    'air2spv-texture-sample-options-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv texture sample options (unit)', air2spv_tso_test,
    workdir : meson.current_source_dir() / '..')
endif

# AIR constant-expression casts (census X03): LangRef folding rules (pure) + a fixture with the casts LLVM cannot fold (addrspacecast / ptrtoint of a module global), spirv-val. Skips (77) without spirv-val.
if fs.is_file(meson.current_source_dir() / 'air2spv-ce-cast-unit.c')
  air2spv_cec_test = executable('air2spv-ce-cast-unit',
    'air2spv-ce-cast-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv constant-expression casts (unit)', air2spv_cec_test,
    workdir : meson.current_source_dir() / '..')
endif

# The same integer intrinsics executed on a Vulkan 1.3 device (lavapipe), every pixel compared with a CPU reference. Skips (77) without a suitable device.
if fs.is_file(meson.current_source_dir() / 'air2spv-int-intrinsics-lavapipe.c') and vulkan_dep.found()
  air2spv_int_vk_test = executable('air2spv-int-intrinsics-lavapipe',
    'air2spv-int-intrinsics-lavapipe.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv integer intrinsics on lavapipe', air2spv_int_vk_test,
    workdir : meson.current_source_dir() / '..')
endif

# The same sampling options executed on a Vulkan 1.3 device (lavapipe): mip-mapped texture, explicit level / bias / constant offset compared with the expected texel. Skips (77) without a device.
if fs.is_file(meson.current_source_dir() / 'air2spv-texture-sample-lavapipe.c') and vulkan_dep.found()
  air2spv_tso_vk_test = executable('air2spv-texture-sample-lavapipe',
    'air2spv-texture-sample-lavapipe.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv texture sample options on lavapipe', air2spv_tso_vk_test,
    workdir : meson.current_source_dir() / '..')
endif

# Sampler state + texture3d on a Vulkan 1.3 device (lavapipe), compared with an independent CPU reference: constexpr samplers (normalized + pixel coordinates), runtime sampler
# descriptors (address modes, filters, unnormalized), Dim3D images. Skips (77) without a device. Fixtures: tests/fixtures/gen-sampler-fixtures.sh.
if fs.is_file(meson.current_source_dir() / 'air2spv-sampler-lavapipe.c') and vulkan_dep.found()
  air2spv_smp_vk_test = executable('air2spv-sampler-lavapipe',
    'air2spv-sampler-lavapipe.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep, meson.get_compiler('c').find_library('m', required : false)],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv sampler state + texture3d on lavapipe', air2spv_smp_vk_test,
    workdir : meson.current_source_dir() / '..')
endif

# OpUndef usage report + fail-closed translation (fixtures undefctl_fragment / i1const_fragment).
if fs.is_file(meson.current_source_dir() / 'air2spv-undef-report-unit.c')
  air2spv_undef_test = executable('air2spv-undef-report-unit',
    'air2spv-undef-report-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv undef report + fail closed (unit)', air2spv_undef_test,
    workdir : meson.current_source_dir() / '..')
endif

# The hash= of the "pipeline-build-fail:" device log line (FNV-1a 64) + deterministic resolver-order translation of a fixture pair.
pipeline_spv_hash_test = executable('pipeline-spv-hash-unit',
  'pipeline-spv-hash-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : [libapplegfx_vulkan_dep],
  c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
  build_by_default : true,
)
test('pipeline spv hash (unit)', pipeline_spv_hash_test, workdir : meson.current_source_dir() / '..')

# Pipeline failure bundle (LAGFX_PIPEFAIL_BUNDLE_DIR): forced vkCreateGraphicsPipelines failure on lavapipe writes pipefail_<hash>/.
pipeline_bundle_deps = [libapplegfx_vulkan_dep]
if vulkan_dep.found()
  pipeline_bundle_deps += [vulkan_dep]
endif
pipeline_bundle_test = executable('pipeline-bundle-unit',
  'pipeline-bundle-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : pipeline_bundle_deps,
  build_by_default : true,
)
test('pipeline failure bundle (unit, lavapipe)', pipeline_bundle_test)

# Pipeline-layout-level descriptor check (set / binding vs descriptor type / count / stage) that spirv-val cannot do; no Vulkan device. Self test always; with
# LAGFX_PIPEFAIL_CHECK_DIR=<dir with pipefail_* bundles> the second test checks real device bundles (they are not in the repo) and FAILS on a binding collision.
pipeline_layout_check_deps = [libapplegfx_vulkan_dep]
if vulkan_dep.found()
  pipeline_layout_check_deps += [vulkan_dep]
endif
pipeline_layout_check_test = executable('pipeline-layout-check-unit',
  'pipeline-layout-check-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : pipeline_layout_check_deps,
  build_by_default : true,
)
test('pipeline layout vs SPIR-V descriptor check (unit; with LAGFX_PIPEFAIL_CHECK_DIR also real bundles, which FAIL on a collision)', pipeline_layout_check_test)
sh = find_program('sh', required : false)
if sh.found()
  test('pipefail bundles: pipeline creation replay with validation layers (needs LAGFX_PIPEFAIL_CHECK_DIR; skips otherwise)', sh,
    args : [meson.current_source_dir() / '../tools/replay-pipefail-validate.sh', meson.project_build_root()], timeout : 600)
endif

# DIAGNOSTIC SPIR-V rewrites of the pipeline repro's variant matrix (pure; validated with spirv-val when installed).
spv_variants_test = executable('spv-variants-unit',
  'spv-variants-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : [libapplegfx_vulkan_dep],
  build_by_default : true,
)
test('spv diagnostic variants (unit)', spv_variants_test)

# lagfx_pipeline_repro_run: forced failure -> bundle -> repro reproduces the same VkResult; UberComposite fixture pair reproduces OK. LAGFX_TEST_DUMP_DIR=<dump> adds the device pairs.
pipeline_repro_test = executable('pipeline-repro-unit',
  'pipeline-repro-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : pipeline_bundle_deps,
  c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"', '-D_GNU_SOURCE'],
  build_by_default : true,
)
test('pipeline repro (unit, lavapipe)', pipeline_repro_test, workdir : meson.current_source_dir() / '..')


# The -13 pipeline matrix (opt-in diagnostics) on lavapipe: rows, bounded output, M10 combinations, the LAGFX_PIPELINE_MATRIX production hook.
pipeline_matrix_test = executable('pipeline-matrix-unit',
  'pipeline-matrix-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : pipeline_bundle_deps,
  c_args : ['-D_GNU_SOURCE'],
  build_by_default : true,
)
test('pipeline -13 matrix (unit, lavapipe)', pipeline_matrix_test)

# The fragment-module reduction ladder (opt-in diagnostics): rows + spirv-val, crash isolation (fork per row), in-process on lavapipe, the LAGFX_PIPELINE_REDUCE hook.
pipeline_reduce_test = executable('pipeline-reduce-unit',
  'pipeline-reduce-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : pipeline_bundle_deps,
  c_args : ['-D_GNU_SOURCE'],
  build_by_default : true,
)
test('pipeline reduction ladder (unit, lavapipe)', pipeline_reduce_test, timeout : 300)

# Read-only device diagnostic g7-vkdev (identity, full extension list, feature summary): structure checks on the available ICD, skip (77) without one.
vkdev_info_test = executable('vkdev-info-unit',
  'vkdev-info-unit.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : pipeline_bundle_deps,
  build_by_default : true,
)
test('device diagnostic g7-vkdev (unit, lavapipe)', vkdev_info_test)

# Constexpr sampler extraction (AIR global -> i64 word -> decoded state).
if fs.is_file(meson.current_source_dir() / 'air2spv-constexpr-sampler-unit.c')
  constexpr_sampler_test = executable('air2spv-constexpr-sampler-unit',
    'air2spv-constexpr-sampler-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    build_by_default : true,
  )
  test('air2spv constexpr sampler (unit)', constexpr_sampler_test,
       workdir : meson.current_source_dir() / '..')
endif

# The descriptor binding allocator / table: sequential allocation, sharing, every incompatibility, growth + overflow, the table key, reflected translator output.
binding_alloc_test = executable('binding-alloc-unit',
  'binding-alloc-unit.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : [libapplegfx_vulkan_dep],
  c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
  build_by_default : true,
)
test('descriptor binding allocator (unit)', binding_alloc_test, workdir : meson.current_source_dir() / '..')

# Descriptor bindings of translated modules: no set+binding with two descriptor types (many resources + constexpr samplers + input attachments), growth, VS+FS table.
air2spv_binding_translate_test = executable('air2spv-binding-translate-unit',
  'air2spv-binding-translate-unit.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : [libapplegfx_vulkan_dep],
  c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
  build_by_default : true,
)
test('air2spv descriptor bindings (translated modules, unit)', air2spv_binding_translate_test, workdir : meson.current_source_dir() / '..')

# Descriptor binding table at the Vulkan level (lavapipe + Khronos validation layer when installed): collisions are reported, VS+FS sharing keeps one binding, growth, pipeline + descriptor writes of translated pairs.
vk_binding_layout_test = executable('vk-binding-layout-lavapipe',
  'vk-binding-layout-lavapipe.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : pipeline_bundle_deps,
  c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"', '-D_GNU_SOURCE'],
  build_by_default : true,
)
test('vulkan descriptor binding layout (lavapipe, validation)', vk_binding_layout_test, env : ['LAGFX_VK_VALIDATION=1'], workdir : meson.current_source_dir() / '..', timeout : 120)

# VkSampler cache by state (lavapipe): distinct states -> distinct samplers.
if fs.is_file(meson.current_source_dir() / 'vulkan-sampler-cache-unit.c')
  sampler_cache_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    sampler_cache_deps += [vulkan_dep]
  endif
  sampler_cache_test = executable('vulkan-sampler-cache-unit',
    'vulkan-sampler-cache-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : sampler_cache_deps,
    build_by_default : true,
  )
  test('vulkan sampler cache (unit)', sampler_cache_test,
       workdir : meson.current_source_dir() / '..')
endif

# Sampler address / filter / mip mapping + fidelity counters (state level, no device): the one mapper, production vs test-only path, undefined values.
if fs.is_file(meson.current_source_dir() / 'vulkan-sampler-mapping-unit.c')
  sampler_map_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    sampler_map_deps += [vulkan_dep]
  endif
  sampler_map_test = executable('vulkan-sampler-mapping-unit',
    'vulkan-sampler-mapping-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : sampler_map_deps,
    build_by_default : true,
  )
  test('vulkan sampler mapping + fidelity counters (unit)', sampler_map_test,
       workdir : meson.current_source_dir() / '..')
endif

# Fragment [[position]]: BuiltIn FragCoord only with the pair keys (translate_module_ex), Location fallback without. Hand-written fixture.
if fs.is_file(meson.current_source_dir() / 'air2spv-fragment-position-unit.c')
  air2spv_fpos_test = executable('air2spv-fragment-position-unit',
    'air2spv-fragment-position-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv fragment [[position]] binding (unit)', air2spv_fpos_test,
    workdir : meson.current_source_dir() / '..')
endif

# Display diagnostics helpers (standalone — header-only pure functions: format names, gamma region summarizer, identity check).
if fs.is_file(meson.current_source_dir() / 'display-diag-unit.c')
  display_diag_unit_test = executable('display-diag-unit',
    'display-diag-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('display diagnostics helpers (unit)', display_diag_unit_test)
endif

# Gamma-region read: the owner task of the display mapping is asked before the last 0x44 task and before any scan (no Vulkan device).
if fs.is_file(meson.current_source_dir() / 'display-diag-gamma-owner-unit.c')
  display_diag_gamma_owner_test = executable('display-diag-gamma-owner-unit',
    'display-diag-gamma-owner-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep],
    build_by_default : true,
  )
  test('display diagnostics: gamma region read prefers the mapping owner task (unit)', display_diag_gamma_owner_test)
endif
