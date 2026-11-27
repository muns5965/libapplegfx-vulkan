# Metal tile shaders and VK_QCOM_tile_shading

Status: research + a default-OFF capability probe. **No tile-shading command is recorded anywhere in the library; the Metal tile path is, and stays, the compute emulation of `src/protocol/arm64_tile.c`.**
Branch `research/g7-qcom-tile` (from `research/g7-integ5` @ `3ca2128`).

Evidence tags: **[V]** verified against a Khronos / Apple primary source named in section 9, or against this repository's code or the device log; **[I]** inferred (reasoned from [V] facts, not stated by a source); **[N]** not verified, no way to verify offline (lavapipe has none of the extensions). Nothing tagged [N] is implemented.

## 1. Sources and licensing

Implementation basis, all permissive or specification text:

- Vulkan registry `vk.xml` (Khronos, Apache-2.0 / MIT): struct members, enum values, command signatures of `VK_QCOM_tile_shading` (#310, revision 2), `VK_QCOM_tile_properties` (#485), `VK_QCOM_tile_memory_heap` (#548).
- Khronos feature proposals for `VK_QCOM_tile_shading` and `VK_QCOM_tile_memory_heap`; the SPIRV-Registry page `SPV_QCOM_tile_shading` (revision 1).
- Metal Shading Language specification (Apple, public PDF): sections 2.11, 4.5, 5.1.9, 5.6.3 - 5.6.5.
- `qualcomm/qhwi`: its README and `LICENSE.txt` state **BSD-3-Clause-Clear**, which grants no patent licence ("NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY THIS LICENSE"). Reading its sample source was refused by the host (`robots_disallowed`), so **no qhwi code was read or used**; the only thing taken from it is that a `qcom_tile_shading` and a `qcom_tile_memory_heap` example exist (Android only). Anything from it would be API-usage understanding only, never copied.
- GPL / LGPL projects: not consulted.

## 2. What the device reported (log `g7-vkdev: qcom_tile_shading`, Adreno 840, driver 0x8034a013, Vulkan 1.4.295)

[V] `ext=1 tileShading=1 fragmentStage=1 colorAtt=1 depthAtt=1 stencilAtt=1 inputAtt=1 sampledAtt=1 perTileDraw=1 perTileDispatch=1 dispatchTile=1 apron=0 anisoApron=0 atomicOps=1 imageProcessing=0 maxApronSize=8 preferNonCoherent=1 tileGranularity=16x16 maxTileShadingRate=8x8`.
[V] `VK_QCOM_tile_shading`, `VK_QCOM_tile_properties` and `VK_QCOM_tile_memory_heap` are all listed (specVersion 1 for each in the log; the current registry says revision 2 for tile_shading, whose only change is that `tileShadingApron` became optional, which matches `apron=0`). `ext_state`: `VK_QCOM_tile_shading extension_enabled=0`.
[V] `VK_EXT_shader_tile_image` is absent. Subgroup size 64 (`subgroup size=64`), `VK_KHR_dynamic_rendering_local_read` and rasterization-order attachment access are enabled.
[V] vk.xml: `VK_QCOM_tile_shading` `depends="VK_QCOM_tile_properties"` - the pair must be enabled together.
Not in the log: the `tileProperties` and `tileMemoryHeap` feature values, the tile-memory-heap properties, and the actual hardware tile size for any render pass.

The run: `g7-gpu-tile: pipes=2 dispatches=2 groups=8 region=2 sg[emulated=2] skipped[rt=2]`, dispatch geometry `region o=0,0 r=64x64 tg=32x32x1 tgt=64x64`. [V] Two dispatches ran (logical 32-lane simdgroups inside the 64-lane subgroup) and two more were refused by the imageblock-attachment gate (not Rgba16f, no STORAGE usage, no view, or linear). The log has no `g7-gpu-fmt` line, so no image was trimmed; the cause of `rt=2` could not be told from it. This branch adds `g7-gpu-tile-rt:` (section 6) which names it next time.

## 3. What the repository decodes today

[V] from `arm64_tile.c`, `translate_function.c`, `tests/fixtures`:

| Item | Today |
|---|---|
| Tile pipeline (tag 0x33) | A Metal kernel function, translated by the air2spv COMPUTE stage. |
| Tile == | One workgroup. Workgroup size = the dispatch's threads-per-tile (SpecId 0/1/2). The grid of workgroups covers the render area or the region (`vkCmdDispatchBase`). The tile size is therefore the app's, not the hardware's: a virtual tiling of the attachment. |
| Imageblock | Only the **implicit** imageblock (the colour attachments as tile memory): `air.load/store.implicit_imageblock` becomes `OpImageRead/Write` on a set-0 `Rgba16f` storage image named `lagfx.imageblock.<rt>`. Coordinates are tile-local in Metal, so the translator adds `WorkgroupId * WorkgroupSize`. Attachments other than R16G16B16A16_SFLOAT with STORAGE usage are skipped (`skip_rt`). |
| Not decoded | Explicit (user-defined) imageblocks, `threadgroup_imageblock`, threadgroup memory of a tile function (tag 0x9c), tile samplers (0x9f/0xa0, default sampler used), render-target array index (0xa3), imageblock formats other than Rgba16f. |
| Render pass | Dynamic rendering. The instance is **ended** (attachments to GENERAL), the compute is dispatched, the instance is **resumed with LOAD ops**. On a tiler every such split writes the tile to system memory and reads it back. |
| Fixtures | `tile_simd_blur_15` (simd shuffles along a row of a 32-lane group; read through the tile pipeline end to end and compared per pixel with a CPU model on lavapipe) and `imageblock_rmw` (store, load, modify, store of the implicit imageblock, compared with the CPU). Neither uses explicit imageblocks or threadgroup memory. |
| simdgroup | Metal 32 lanes. Device 64 (cannot be pinned to 32): the translator remaps lane id / size / shuffles to logical 32-lane groups (`simd_emul`). |

## 4. Mapping table

| Metal | VK_QCOM_tile_shading | Status |
|---|---|---|
| Render pass whose encoder runs tile functions | `VkRenderPassTileShadingCreateInfoQCOM { flags = ENABLE_BIT [| PER_TILE_EXECUTION_BIT], tileApronSize }` chained into `VkRenderingInfo` (also `VkRenderPassCreateInfo(2)`, `VkCommandBufferInheritanceInfo`). Apron `(0,0)` here. | [V] struct and bits (`VK_TILE_SHADING_RENDER_PASS_ENABLE_BIT_QCOM` = bit 0, `..._PER_TILE_EXECUTION_BIT_QCOM` = bit 1). Whether our pass shapes qualify: [N]. |
| Tile function, once per tile, inline with draws | Commands recorded between `vkCmdBeginPerTileExecutionQCOM` / `vkCmdEndPerTileExecutionQCOM` run once per tile; needs `tileShadingPerTileDispatch` (=1). The order of tiles is undefined. | [V] semantics; that an ordinary `vkCmdDispatch` inside the bracket is the right vehicle for a threads-per-tile kernel: [I]. |
| `dispatchThreadsPerTile(w,h,d)` | Option A: `vkCmdDispatch` in the bracket with the shader's `LocalSize` = threads-per-tile (keeps today's SpecId 0/1/2 scheme). Option B: `vkCmdDispatchTileQCOM` (`tileShadingDispatchTile` =1): the implementation picks group count and local size from the shader's `TileShadingRateQCOM` execution mode. | A: [I]. B: [V] semantics, but it cannot be used for a kernel whose thread layout (e.g. 32-lane rows) is baked in: [I]. |
| Region dispatch (tags 0xa2/0xa3) | None. Per-tile execution covers every tile of the render area. | [I] clipping would have to be done in the shader against `TileOffsetQCOM`: [N]. |
| Implicit imageblock = colour attachments | Tile attachment variable: `OpTypeImage` (2D) in storage class `TileAttachmentQCOM` (4491) with `DescriptorSet` / `Binding`; storage attachment is backed by `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE`; colour store is allowed in compute, depth/stencil/input store is not; atomics colour only (`tileShadingAtomicOps`=1). | [V]. Format rule: "the variable's basic type must match the attachment format", so formats other than Rgba16f may become usable (would lift most of `skip_rt`): [I]. Whether the image must still be created with STORAGE usage and its descriptor layout: [N]. |
| Imageblock coordinate (tile-local) | `TileOffsetQCOM` (BuiltIn 4492, uvec2, framebuffer x,y of the tile's top-left texel), `TileDimensionQCOM` (4493), `TileApronSizeQCOM` (4494); accesses must lie within the tile and the render area. | [V] builtins. Whether storage-attachment coordinates are framebuffer-absolute or tile-relative: [N] (the sampled-attachment text says "relative to the attachment"; for storage it only says "within the tile extent"). The translator's `WorkgroupId*WorkgroupSize` add would be replaced by `TileOffsetQCOM`: [I]. |
| Colour attachment read inside a tile function after a draw | Same render pass instance, no split; `VK_ACCESS_2_SHADER_TILE_ATTACHMENT_READ/WRITE_BIT_QCOM`, `VK_DEPENDENCY_BY_REGION_BIT` becomes tile-granular. | [V] bits. Per-tile ordering draw -> dispatch -> draw: [I]. |
| Explicit imageblock (`imageblock<T, imageblock_layout_explicit>`) and `threadgroup_imageblock` | **No equivalent.** | See section 5. |
| Threadgroup memory of a tile function | Ordinary `Workgroup` memory inside the per-tile dispatch (does not persist between dispatches), or tile-memory-heap resources. | [I] |
| simdgroup = 32 lanes | Subgroup size is whatever the device uses (64 here). Tile shading does not change it. | [I] The existing logical-32 emulation stays necessary on this device. |
| Pipelines | Pipelines with `TileShadingQCOM` shaders can only be bound inside a tile-shading render pass. | [V] So the module needs two variants (default compute emulation and tile-shading), the `Rgba16f` storage-image form cannot be reused unchanged. |

SPIR-V needed for a native module [V tokens, [N] behaviour]: `OpExtension "SPV_QCOM_tile_shading"`, `OpCapability TileShadingQCOM` (4495; implies Shader), storage class `TileAttachmentQCOM` 4491, BuiltIns 4492/4493/4494, execution modes `NonCoherentTileAttachmentReadQCOM` 4489 (fragment only) and `TileShadingRateQCOM` 4490 (GLCompute only, three literals, x/y powers of two and <= `maxTileShadingRate` = 8x8).

## 5. Semantic gaps (why the mapping is partial)

1. **Imageblock layout.** [V] An imageblock is "a 2D data structure ... allocated in threadgroup memory", with an implicit layout (the colour attachments, fixed by the pass) or an explicit layout the shader declares (`[[imageblock_data(type)]]`, arbitrary struct, may alias the implicit one with `[[alias_implicit_imageblock]]`); in a kernel it is reachable only through an `imageblock<T>` object, `threadgroup_imageblock` address space, with explicit 2D coordinates; tile functions share 32 KB of threadgroup memory between imageblock and threadgroup storage. [V] QCOM tile attachments are only typed views of the render pass's own colour / depth-stencil / input attachments. So only the implicit imageblock has a counterpart. An explicit imageblock would have to live in `VK_QCOM_tile_memory_heap` resources (tile-bound images: 2D, 1 mip, 1 layer, 1 sample, restricted usages; contents persist within one submission batch; one global heap, every allocation aliases; `vkCmdBindTileMemoryQCOM` outside the pass) [V], with its own layout code generated by the translator: that mapping is [N], no feature/property values are in the log, and nothing here implements or enables it.
2. **Tile size ownership.** Metal's tile (imageblock size, threadgroup size) is chosen by the app (log: 32x32 threads per tile). QCOM's tile is chosen by the driver; software only learns it from `vkGetDynamicRenderingTilePropertiesQCOM` (`VK_QCOM_tile_properties`) and a multiple of `tileGranularity` (16x16) [V]. A kernel that treats "my threadgroup is my tile" (every current fixture does) only maps if the hardware tile can hold the Metal tile, or the kernel's reach is re-expressed per hardware tile: [N]. The compute emulation is immune because its tiling is virtual.
3. **Region dispatch** and render-target-array index have no QCOM form (section 4).
4. **simdgroup 32 vs 64.** [I] unchanged by tile shading; keep logical-32 emulation. `TileShadingRateQCOM` is a per-invocation pixel block, unrelated to simdgroup width.
5. **Order and persistence.** Metal tile functions and fragment functions of a pass see one imageblock in encoder order. QCOM per-tile blocks run once per tile in an undefined tile order and, in per-tile mode, many commands are disallowed (queries, timestamps, `vkCmdClearAttachments`, event waits, transform feedback, any work that invokes tessellation, geometry, ray-tracing or mesh stages) [V]. Draw/dispatch interleaving inside a tile: [N].
6. **Coexistence with what we already use.** Tile-shading render passes cannot be recorded with SIMULTANEOUS_USE, cannot use a fragment density map and cannot render to external-format AHBs [V]. Interaction with `VK_KHR_dynamic_rendering_local_read` and rasterization-order attachment access (both enabled here) is not specified in the sources read: [N].
7. **Why bother.** [I] The only structural gain is removing the suspend / LOAD-resume split (a full store + load of the attachment per tile dispatch on a tiler), plus possibly Rgba16f-only gating. Correctness gains: none expected.

## 6. What this branch implements (all default OFF)

| Piece | Where | Notes |
|---|---|---|
| Capability record + pure decision | `src/vulkan/qcom_tile.{h,c}` | `lagfx_qcomtile_decide(flag, dynamic_rendering, caps)`; reasons `flag_off`, `no_dynamic_rendering`, `no_ext_tile_shading`, `no_ext_tile_properties`, `no_feature`, `bad_props`, `enable`. Required features: `tileShading`, `tileShadingColorAttachments`, `tileShadingPerTileDispatch`. |
| Probe | `lagfx_qcomtile_probe` | read-only `vkGetPhysicalDevice{Features,Properties}2`; each struct is chained only if the device lists its extension. `devinfo.c` now shares the same raw layouts. |
| Enablement | `src/vulkan/instance.c`, just before `vkCreateDevice` | Only with `LAGFX_QCOM_TILE=1` (exactly) AND decision `enable`: adds `VK_QCOM_tile_properties` + `VK_QCOM_tile_shading` and a `VkPhysicalDeviceTileShadingFeaturesQCOM` with only the reported subset of {tileShading, colour attachments, per-tile dispatch, dispatch-tile, input/sampled attachments, atomics} (+ `tileProperties` if reported). Apron, depth/stencil, per-tile draw, fragment stage, image processing are never requested. If `vkCreateDevice` refuses, it is retried once with exactly the default chain; the refusal is recorded (`fallback=device_create_failed(retried_without)`, `create_result`). |
| Diagnostics | init line + diag dump | `g7-gpu-qcomtile: flag= inject= ext[..] feat[..] props[..] decision= enable_mask= enabled= create_result= chosen_path=compute_emulation qcom_native_path=not_implemented fallback=` printed once at init (all runs, flag on or off); the periodic / final diag dump repeats it with `use[emul_dispatches=N emul_while_qcom_enabled=N qcom_native_dispatches=0]`. New `g7-gpu-tile-rt:` line (only when `skip_rt` > 0): `why[no_attachment format_not_rgba16f no_storage_usage no_view_or_linear] first_refused{vkformat usage linear}`. |
| Test seam | `LAGFX_QCOM_TILE_INJECT=adreno840` (honoured only with the flag on) | replaces the probed record by the section-2 record, so a lavapipe run takes the "enable" decision, the ICD refuses the extensions, and the retry/fallback path is exercised. |

Never changed: the default device creation (flag unset -> no extra extension, no extra struct), pipeline building, the dispatch path, the decode. **Not implemented ([N], inert / absent):** any `VkRenderPassTileShadingCreateInfoQCOM`, per-tile bracket, `vkCmdDispatchTileQCOM`, `TileShadingQCOM` SPIR-V emission, tile-memory-heap use. They cannot be run or validated offline, so they are not in the tree.

Tests (`tests/qcom-tile-unit.c`, meson test "VK_QCOM_tile_shading opt-in probe / decision / fallback"): flag parsing; every decision reason on injected records (Adreno 840 record and degraded variants); enable-mask never includes the never-enabled features; sType numbers; line content and bounded truncation; on lavapipe: flag unset = nothing requested, flag=0 ignores inject, flag=1 = `no_ext_tile_shading` fail-closed, flag=1 + inject = decision enable, first `vkCreateDevice` refused, retry succeeds, no QCOM extension in the device, fallback recorded. `arm64-tile-unit` additionally asserts the counters and that the `g7-gpu-qcomtile` + `use[..]` line appears in the diag dump, and that `skip_rt_why` names a non-Rgba16f attachment.

## 7. First on-device experiments (Adreno 840)

E0 is runnable with this build; E1 - E5 need a small standalone Vulkan probe (not written: nothing about it can be checked offline).

| # | Run | Output that proves |
|---|---|---|
| E0 | The existing app with `LAGFX_QCOM_TILE=1`, same workload as the logged run. | `g7-gpu-qcomtile: flag=1 inject=0 ... decision=enable enable_mask=0x... enabled=1 create_result=0 fallback=none` proves the driver accepts the extension pair + features at device creation. `enabled=0 ... fallback=device_create_failed` plus `create_result=<VkResult>` (e.g. -7 extension not present, -8 feature not present, -3 init failed) proves refusal, and the app still works. In both cases `g7-gpu-tile` counters and the pixels must be identical to the flag-off run (`pipes=2 dispatches=2 sg[emulated=2] skipped[rt=2]`; `use[emul_while_qcom_enabled=2]` when enabled). The same line also records `tileProperties`, `tileMemoryHeap` feature values and heap properties (new facts). Also look at `g7-gpu-tile-rt:` for the cause of `rt=2`. |
| E1 | Probe: `vkGetDynamicRenderingTilePropertiesQCOM` on the real pass shape (RGBA16F target, e.g. 64x64 and a full-screen one), needs `tileProperties`=1. | `tileSize`, `apronSize`, `origin`: the hardware tile versus the Metal 32x32 threads-per-tile; decides gap 2. |
| E2 | Probe: tile-shading render pass (`ENABLE_BIT | PER_TILE_EXECUTION_BIT`, apron 0), one per-tile `vkCmdDispatch` with a hand-written SPIR-V module using `TileShadingQCOM`, a storage tile attachment on an RGBA16F colour attachment, writing `TileOffsetQCOM + LocalInvocationId` into the pixel; read back. | Pixel value = absolute x,y -> coordinates are framebuffer-absolute; pixel = local id -> tile-relative; plus the real `TileDimensionQCOM` / `TileOffsetQCOM` values. Also whether a validation-clean descriptor needs STORAGE usage / which layout. |
| E3 | Same probe on an RGBA8 / BGRA8 colour attachment without STORAGE usage. | Success proves the Rgba16f-only gate (`skip_rt`) can be lifted by a tile-attachment path; failure keeps it. |
| E4 | Probe: draw (clear + triangle) -> per-tile dispatch RMW -> draw, one pass instance; compare to the split-pass result. | Equality shows draw/dispatch order inside a tile (gap 5) and that no suspend / LOAD-resume is needed. |
| E5 | Port `tile_simd_blur_15` through the module change (capability, storage class, `TileOffsetQCOM`) with logical-32 simdgroup emulation on the 64-lane subgroup; compare with the compute-emulation output. | Pixel equality proves the simd emulation is unaffected by tile shading (gap 4). GPU time / bandwidth of E5 vs the split-pass path is the actual benefit measurement. |
| E6 | Probe: tile memory heap properties and one tile-bound RGBA16F image with `vkCmdBindTileMemoryQCOM`. | Heap size and `queueSubmitBoundary` / `tileBufferTransfers` decide whether an explicit imageblock can be modelled at all. |

Decision rule after E0 - E5: only if E2 - E5 pass and the measured gain from E5 is real is a native path worth building behind a second flag; until then, the compute emulation stays.

## 8. Open questions

Hardware tile size for the repo's pass shapes (E1). Storage tile attachment coordinate convention and descriptor layout (E2). Per-tile ordering with draws (E4). Interaction with local-read / raster-order (all of E2 - E5 run with them enabled, as the library does). Explicit imageblock modelling (E6, maybe never).

## 9. Primary sources used

- Vulkan registry `vk.xml` (KhronosGroup/Vulkan-Headers, `registry/vk.xml`): extension entries #310, #485, #548.
- docs.vulkan.org: feature proposals `VK_QCOM_tile_shading`, `VK_QCOM_tile_memory_heap`; ref page `VK_QCOM_tile_shading`.
- SPIRV-Registry: `SPV_QCOM_tile_shading` (revision 1, 2025-04-15): capability 4495, storage class 4491, execution modes 4489/4490, BuiltIns 4492-4494.
- Apple, Metal Shading Language Specification: 2.11 Imageblocks, 4.5 Threadgroup Imageblock Address Space, 5.1.9 Tile Functions, 5.6.3 - 5.6.5.
- Device log `g7_lagfx_1795760879033.txt` (lines 15 - 44, `g7-gpu-tile` lines).
