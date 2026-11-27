# g7: BGRA8 tile imageblock + dynamic blend constants

Branch `research/g7-bgra8-blendconst` (base 30ccf5e). Labels: [V] verified in code or a host test, [I] inferred, [N] not verified.
Status: **implemented** and **host-test-verified on lavapipe** (Mesa software, no Adreno). **Nothing here is device-verified.**
Written only from this repository's code, its own tests and the public Vulkan / SPIR-V specifications.

## 1. BGRA8 tile imageblock (`src/protocol/arm64_tile.c`)

Before: a tile dispatch was skipped unless every imageblock attachment was `R16G16B16A16_SFLOAT` + STORAGE + view + not linear. A device log showed the skip with
vkformat 44 (`B8G8R8A8_UNORM`). [V] The kernel's imageblock is a SPIR-V storage image declared `Rgba16f` (`cs_imageblock_image`, translate_function.c), and a
storage-image descriptor's view format must match the declared SPIR-V image format, so widening the check alone would be invalid.

Now:
- `lagfx_a64_tile_rt_classify()` (arm64_tile.h, pure) decides RGBA16F (unchanged) / BGRA8 / refused, with the cause (`skip_rt_why`):
  0 no attachment, 1 format not supported (RGBA8 etc. stay refused), 2 no STORAGE usage, 3 no view / linear,
  **4 BGRA8 but the device lacks shaderStorageImageRead/WriteWithoutFormat, 5 BGRA8 format has no STORAGE_READ/WRITE_WITHOUT_FORMAT bit
  (VkFormatProperties3, queried once; a pre-1.3 device cannot be asked and is refused as 5).**
- Translator: `lagfx_air2spv_set_imageblock_unformatted(mask)` (translate_function.c/.h, thread-local like the simd setting). Attachments in `mask` become
  `OpTypeImage ... Sampled=2 Format=Unknown` with `StorageImageReadWithoutFormat` + `StorageImageWriteWithoutFormat`. Mask 0 (default) emits exactly what it did.
- The tile pipeline keeps a copy of the kernel's AIR bitcode and builds the BGRA8 variant on first use per attachment mask (`tile_variant_module`), cached,
  failures cached. The variant must bind identically to the base translation (same set-0 bindings / kinds / attachment names) or it is refused
  (`bgra8_variant_failed`, dispatch skipped as `skip_pipe_build`). Pipelines are cached per (workgroup size, pin, variant mask).
- Device: `shaderStorageImageReadWithoutFormat` is now requested when available (instance.c); `vk->have_storage_img_rw_nofmt` = read AND write enabled.
- Channel order / normalisation: with a view of the BGRA8 format and an unformatted image, Vulkan presents the texel to the shader as the format's (R, G, B, A)
  components and does the unorm8 <-> float conversion (round to nearest, clamp [0,1]) [I: Vulkan spec, texel input/output operations; checked by the lavapipe test
  below, NOT on Adreno]. So **no swizzle is emitted**: kernel float4 (r,g,b,a) = logical RGBA. A compatible `R8G8B8A8` view (needs MUTABLE_FORMAT on the image and an
  explicit swizzle) was NOT implemented.
- Barriers / layouts: unchanged (attachments are in GENERAL around the dispatch, memory barriers before/after) [V in code reading; the same code runs for RGBA16F].
- Counters: `tile.skip_rt_why[0..5]`, `tile.bgra8_dispatches / bgra8_variants / bgra8_variant_failed`; diag lines `g7-gpu-tile-rt` (extended) and `g7-gpu-tile-bgra8`.

Tests (`tests/arm64-tile-unit.c`, meson test "arm64 tile ..."):
- `test_rt_classify`: RGBA16F accepted without device features; BGRA8 accepted only with all conditions; RGBA8 / sRGB refused (why 1); each cause 2..5 distinct.
- `test_translate_bgra`: the Rgba16f translation of tile_simd_blur_15 is byte-identical to the base commit (size 4100, FNV-1a 0x964e8c4e55e4c739, measured on 30ccf5e);
  the variant has one Unknown-format image and capabilities 55 + 56 and passes `spirv-val --target-env vulkan1.3`; setting is per call.
- Block (5) in `test_tile` (lavapipe): BGRA8 target cleared to (0.2, 0.4, 0.6, 0.8), kernel output compared per channel with the CPU model in BGRA byte order (+-1 for
  unorm rounding), unsaturated so R/B swaps are visible; variant cached across dispatches; with `have_storage_img_rw_nofmt` forced off the attachment is refused (why 4).
  On a device without support the exec part degrades to checking the refusal counter (prints `SKIP-EXEC`).
- Existing RGBA8 attachment test (why[1]) and all RGBA16F tests unchanged and passing.

## 2. Blend constants (`src/protocol/arm64_pipeline.c`, `arm64_exec_run.c`, `vulkan/pipeline_build.c`)

Identification of the command: tag 101 (= 0x65, `SetBlendColor`, 16 bytes = 4 x f32) [V: `render_inner_ops.c` table, same numbering/sizes as the already
implemented tags 104/107/108/117/130; also listed in docs/arm64-opcode-comparison.md]. Component order r,g,b,a [I: from the trace print order in op_set_blend_color].
It had not been observed on a device in this repository's evidence [N].

- `pdesc` factors 11..14 (Metal BlendColor, OneMinusBlendColor, BlendAlpha, OneMinusBlendAlpha) are now mapped (the table already had them) instead of making the
  pipeline fall back to the fixed source-over (`stub_blend`); 15.. (source-1) still falls back. `desc.blend_const_dyn` is set (also for attachment 1).
- `pipeline_build.c`: `VK_DYNAMIC_STATE_BLEND_CONSTANTS` when `blend_const_dyn`. Also serialised in the pipeline bundle field list.
- Exec: tag 101 stores the colour in the encoder state (NaN -> 0), reset to (0,0,0,0) by a new pass (memset of `es`); `bind_for_draw` records
  `vkCmdSetBlendConstants` before EVERY draw of such a pipeline (a pipeline bind may leave the value undefined). Counters `run.blend_color_set`, `run.blend_const_draws`;
  diag line `g7-gpu-blendc`.
- Test `arm64-pipe-unit` `test_blend_constants` (lavapipe): src ZERO / dst BlendColor (rgb) and BlendAlpha (alpha) on the green clear: centre pixel = (0, 0.5*255, 0, 0.25*255)
  for colour (1, .5, 1, .25); another colour in the next exec; default (no tag) gives 0. Fails with the old `< 11u` check (assertion on `blend_decoded`).
- NOT covered: capture/replay files do not record the blend colour; source-1 factors; the legacy ring path (`handlers/compute/render_inner_ops.c`) still only traces it.

## Not verified on device
Adreno support for B8G8R8A8 storage / without-format access, the real MTLB tile kernels with BGRA8 attachments, the actual occurrence and payload order of tag 101,
and the write-back of a compute-written BGRA8 attachment to guest memory (generic pass-end path, unchanged) [N].
