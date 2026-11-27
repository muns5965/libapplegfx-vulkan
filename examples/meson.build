# examples/meson.build — E2E examples (Phase 3.C.2)
#
# Builds auxiliary tools that drive src/air2spirv end-to-end against
# real metallib fixtures. Not wired to `meson test` — these examples
# are part of the stage-3 (LLVM SPIR-V backend) runbook and are
# invoked by shell scripts that shell out to `xcrun metal` and `llc`.

if fs.is_file(meson.current_source_dir() / 'triangle' / 'extract_retarget.c')
  executable('triangle-extract-retarget',
    'triangle/extract_retarget.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
endif

# SPV entry-point rewriter — runs after llc produces SPIR-V from the
# retargeted .bc files. Fixes up OpCapability/OpEntryPoint/CPacked/
# LinkageAttributes so vkCreateShaderModule accepts the result. See
# src/air2spirv/spv_entrypoint_rewrite.c for the full edit list.
if fs.is_file(meson.current_source_dir() / 'triangle' / 'spv_rewrite.c')
  executable('triangle-spv-rewrite',
    'triangle/spv_rewrite.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
endif

# Extract-only tool — reads a metallib and writes one .air.bc file per
# function. No retargeting is performed; the AIR bitcode is written as-
# found for offline analysis (e.g., intrinsic cataloguing). See
# triangle/extract_only.c for details.
if fs.is_file(meson.current_source_dir() / 'triangle' / 'extract_only.c')
  executable('triangle-extract-only',
    'triangle/extract_only.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
endif

# air-bcdump — bcanalyzer-like structural dump tool that exercises our
# clean-room Phase 1 + Phase 2 decoders. Used by the Phase 2 regression
# harness (paravirt-re/library/diag/air_reader_diff.py) and for ad-hoc
# inspection of captured macOS metallib bodies.
executable('air-bcdump',
  'air-bcdump.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# air-pair-translate — offline vertex + fragment pair translation as the resolver does it, with the descriptor binding tables of both stages (before/after evidence
# for a binding-layout change; exit 3 = a set+binding carries two descriptor types).
executable('air-pair-translate',
  'air-pair-translate.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# air-translate — offline .air.bc → SPIR-V via the real translator
# (per-instruction body walker + reference fallback). Used to test real
# captured macOS shaders through our pipeline without a guest / noVNC.
executable('air-translate',
  'air-translate.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# xgc-replay — offline replay of the Xgc login-material draw on lavapipe
# with the real captured uniform windows (LAGFX_DUMP_BINDS) + texture set;
# bisects which input blackens the output (KICKOFF-fragment-survival).
if vulkan_dep.found()
  executable('xgc-replay',
    'xgc-replay.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
endif

# dump-translate — re-translates the metallib blobs of an arm64 dump with the current translator (check fixes with spirv-val against real guest shaders).
executable('dump-translate',
  'dump-translate.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# dump-psolink — offline replay of the resolver's stage selection (recorded token scan vs root tags 1/2) + translation: stage_link counters per selection.
executable('dump-psolink',
  'dump-psolink.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# dump-bindings — per PSO of an arm64 dump: bindings per stage and why the resolver would stub a framebuffer-fetch pipeline.
executable('dump-bindings',
  'dump-bindings.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# dump-draws — per `draw` line of an arm64 dump: the bindings the PSO's shaders need (the resolver's own binding map) against the buffer/texture/sampler slots the draw provides.
executable('dump-draws',
  'dump-draws.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# render-replay — real Metal shaders of an arm64 dump rendered with synthetic resources on lavapipe, read back and compared with a CPU model (examples/render-replay.c).
if vulkan_dep.found()
  executable('render-replay',
    'render-replay.c',
    include_directories : [include_dir, internal_include_dir, include_directories('../src')],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep, meson.get_compiler('c').find_library('m', required : false)],
    build_by_default : true,
  )
endif

# dump-pairhash — per PSO of an arm64 dump: size + FNV-1a-64 hash of the vertex/fragment SPIR-V exactly as the resolver builds them (matches the device "pipeline-build-fail:" line).
executable('dump-pairhash',
  'dump-pairhash.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# pipeline-repro — rebuilds a failed pipeline from a failure bundle (lagfx_pipeline_repro_run) and prints the report; --matrix adds the DIAGNOSTIC variant matrix.
executable('pipeline-repro',
  'pipeline-repro.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# spv-variant — writes one DIAGNOSTIC SPIR-V variant (pipeline repro matrix e..i) of a module to a file.
executable('spv-variant',
  'spv-variant.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# reduce-rows — DRY run of the pipeline-reduce ladder: writes every row's SPIR-V + manifest.tsv (offline spirv-val / hashing of the rows the device builds).
executable('reduce-rows',
  'reduce-rows.c',
  include_directories : [include_dir, internal_include_dir, include_directories('../src')],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

# dump-vtxattrs — per PSO of an arm64 dump: decoded MTLVertexDescriptor attributes vs the vertex shader's reflected stage-in Locations.
executable('dump-vtxattrs',
  'dump-vtxattrs.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)
