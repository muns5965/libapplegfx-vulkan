# Metal tile shaders and VK_QCOM_tile_shading

Status: research + a default-OFF capability probe. **No tile-shading command is recorded anywhere in the library; the Metal tile path is, and stays, the compute emulation of `src/protocol/arm64_tile.c`.**
Branch `research/g7-qcom-tile` (from `research/g7-integ5` @ `3ca2128`); `research/g7-qcom-probe` (from `dacc194`) adds the standalone device probe `tools/qcom-tile-probe` and rewrites section 7 (still no tile-shading command in the library).

Evidence tags: **[V]** verified against a Khronos / Apple primary source named in section 9, or against this repository's code or the device log; **[I]** inferred (reasoned from [V] facts, not stated by a source); **[N]** not verified, no way to verify offline (lavapipe has none of the extensions). Nothing tagged [N] is implemented.

## 1. Sources and licensing

Implementation basis, all permissive or specification text:

- Vulkan registry `vk.xml` (Khronos, Apache-2.0 / MIT): struct members, enum values, command signatures of `VK_QCOM_tile_shading` (#310, revision 2), `VK_QCOM_tile_properties` (#485), `VK_QCOM_tile_memory_heap` (#548).
- Khronos feature proposals for `VK_QCOM_tile_shading` and `VK_QCOM_tile_memory_heap`; the SPIRV-Registry page `SPV_QCOM_tile_shading` (revision 1).
- Metal Shading Language specification (Apple, public PDF): sections 2.11, 4.5, 5.1.9, 5.6.3 - 5.6.5.
- `qualcomm/qhwi`: its README and `LICENSE.txt` state **BSD-3-Clause-Clear**, which grants no patent licence ("NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY THIS LICENSE"). Reading its sample source was refused by the host in the first session (`robots_disallowed`); in the probe session the README became reachable and the sample sources were deliberately not opened (section 7.1). **No qhwi code was read or used**; the only things taken from it are the README facts that a `qcom_tile_shading` and a `qcom_tile_memory_heap` example exist (Android only) and which extensions it lists. Anything from it would be API-usage understanding only, never copied.
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

Never changed: the default device creation (flag unset -> no extra extension, no extra struct), pipeline building, the dispatch path, the decode. **Not implemented ([N], inert / absent):** any `VkRenderPassTileShadingCreateInfoQCOM`, per-tile bracket, `vkCmdDispatchTileQCOM`, `TileShadingQCOM` SPIR-V emission, tile-memory-heap use. They cannot be run or validated offline, so they are not in the library; `tools/qcom-tile-probe` (section 7) records and runs them in a standalone program.

Tests (`tests/qcom-tile-unit.c`, meson test "VK_QCOM_tile_shading opt-in probe / decision / fallback"): flag parsing; every decision reason on injected records (Adreno 840 record and degraded variants); enable-mask never includes the never-enabled features; sType numbers; line content and bounded truncation; on lavapipe: flag unset = nothing requested, flag=0 ignores inject, flag=1 = `no_ext_tile_shading` fail-closed, flag=1 + inject = decision enable, first `vkCreateDevice` refused, retry succeeds, no QCOM extension in the device, fallback recorded. `arm64-tile-unit` additionally asserts the counters and that the `g7-gpu-qcomtile` + `use[..]` line appears in the diag dump, and that `skip_rt_why` names a non-Rgba16f attachment.

## 7. On-device experiments: `tools/qcom-tile-probe`

Branch `research/g7-qcom-probe` (from `research/g7-qcom-tile` @ `dacc194`). E0 is the existing app with `LAGFX_QCOM_TILE=1` (below). E1 - E6 are implemented as one standalone Vulkan program that does not boot a VM, does not link the library, and links only the Vulkan loader and libm: `tools/qcom-tile-probe/qcom_tile_probe.c` plus the header-only SPIR-V emitter `qp_spv.h`. The next device run only executes it.

Status is recorded at three levels per item (see 7.3). **Device-verified is NO for every tile-shading path**; nothing in this section has run on an Adreno.

### 7.1 Basis and what was fetched

- [V] `vk.xml` (Khronos `main`, fetched this session) for every struct, enum and command of `VK_QCOM_tile_shading` (#310, revision 2) and `VK_QCOM_tile_properties` (#485): `VkRenderPassTileShadingCreateInfoQCOM {sType 1000309002, flags, VkExtent2D tileApronSize}`, `VkPerTileBeginInfoQCOM` / `VkPerTileEndInfoQCOM` / `VkDispatchTileInfoQCOM` (1000309003/4/5, `{sType, pNext}` only), `VkTilePropertiesQCOM {sType 1000484001, VkExtent3D tileSize, VkExtent2D apronSize, VkOffset2D origin}`, commands `vkGetDynamicRenderingTilePropertiesQCOM`, `vkCmdDispatchTileQCOM`, `vkCmdBeginPerTileExecutionQCOM`, `vkCmdEndPerTileExecutionQCOM`, access bits `VK_ACCESS_2_SHADER_TILE_ATTACHMENT_READ/WRITE_BIT_QCOM` (bit 51 / 52).
- [V] Khronos proposal `proposals/VK_QCOM_tile_shading.adoc` (fetched): the per-tile execution model, "commands in a begin/end block run once per tile in undefined order", the compute-shader rules (tile colour attachment stores allowed, depth/stencil/input stores not), the barrier rules (`VK_DEPENDENCY_BY_REGION_BIT` becomes tile-sized; `COMPUTE_SHADER` and `DRAW_INDIRECT` become framebuffer-space stages; the two access bits), the `vkCmdDispatchTileQCOM` description (start pixel of an invocation is `TileOffsetQCOM + GlobalInvocationId * TileShadingRateQCOM`; the shader must declare `TileShadingRateQCOM`).
- [V] `SPV_QCOM_tile_shading` revision 1 (SPIRV-Registry, fetched): capability 4495, storage class 4491, execution modes 4489 / 4490, BuiltIns 4492 (`TileOffsetQCOM`, framebuffer coordinates of the top-left texel of the active tile), 4493 (`TileDimensionQCOM`, uvec3), 4494 (`TileApronSizeQCOM`).
- The installed header (Vulkan 1.3.275) has the `VK_QCOM_tile_properties` structs but **not** `VK_QCOM_tile_shading`, so the probe declares every QCOM struct locally from vk.xml (with size `_Static_assert`s for LP64) and loads the four QCOM commands and the 1.3 core commands through `vkGetDeviceProcAddr`. The same source therefore builds against any NDK header set that has the Vulkan 1.3 core types.
- `qualcomm/qhwi`: **the README is reachable now** through `raw.githubusercontent.com` (previous session: refused). It lists `VK_QCOM_TILE_PROPERTIES` / `VK_QCOM_TILE_SHADING` / `VK_QCOM_TILE_MEMORY_HEAP` among the used extensions, a CMake option `QHWI_ENABLE_QCOM_TILE_SHADING`, and examples `qcom_tile_shading` and `qcom_tile_memory_heap` (Android only). `gh api` for the repository is refused ("GitHub access to this repository is not enabled for this session"). Its sample sources were **deliberately not opened** (BSD-3-Clause-Clear carries no patent grant); the probe is written from the Khronos texts only. Nothing was learned from qhwi beyond the README lines above.

### 7.2 Running it

Host (any Vulkan device; lavapipe):

```
meson setup build && meson compile -C build                    # builds build/tools/qcom-tile-probe
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json build/tools/qcom-tile-probe
build/tools/qcom-tile-probe --selftest                         # CPU only: half conversion, models, generator
mkdir -p /tmp/spv && build/tools/qcom-tile-probe --dump-spirv /tmp/spv   # then: spirv-val --target-env vulkan1.3 /tmp/spv/*.twin.spv
```

Android (NDK r27c, arm64; no NDK was present on the build host, so this exact command is **not build-verified**; the same sources compile with gcc against the 1.3.275 loader headers):

```
NDK=$ANDROID_NDK_HOME
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang -std=c11 -O2 -Wall -D_GNU_SOURCE \
    -Itools/qcom-tile-probe tools/qcom-tile-probe/qcom_tile_probe.c -o qcom-tile-probe -lvulkan -lm
adb push qcom-tile-probe /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/qcom-tile-probe
adb shell /data/local/tmp/qcom-tile-probe | tee qcomprobe.log          # all of E1-E6, about a minute
adb shell /data/local/tmp/qcom-tile-probe --only E1,E2 | tee e12.log    # a subset
adb shell /data/local/tmp/qcom-tile-probe --no-risky ...                # skip the two out-of-validity cases (7.5)
adb shell /data/local/tmp/qcom-tile-probe --rp-flags 3 ...              # render pass flags ENABLE|PER_TILE_EXECUTION instead of ENABLE
adb shell /data/local/tmp/qcom-tile-probe --list                        # physical devices
grep '^qcomprobe' qcomprobe.log                                         # only the machine-parsable lines (plus qcomprobe-detail rows)
```

Options: `--only E1,E3`, `--device N`, `--timeout S` (watchdog seconds per experiment, default 60), `--wait-ms MS` (fence wait per submit, default 8000), `--rp-flags N`, `--no-risky`, `--no-isolate`, `--strict` (exit 1 on any FAIL), `--selftest`, `--dump-spirv DIR`, `--mock-tile` (analysis self-test on a device without the extension; every line carries `mock=1`; never a device result).

Output contract. One line per experiment: `qcomprobe: E<n> status=PASS|FAIL|UNSUPPORTED <key=value ...>`; the rest of the line is space-separated `key=value` pairs. `PASS` = the path ran on the device and equals the CPU model; `FAIL` = attempted and refused (the `VkResult` name and number are in the line), crashed (`reason=child_crashed signal=N`), timed out (`reason=watchdog_timeout`, exit 3) or produced different pixels; `UNSUPPORTED` = the extension, feature, command or subgroup capability needed is absent, nothing was attempted (`reason=...`). Other lines: `qcomprobe: info ...` (device record: extensions, the 14 tile-shading features, tile properties, subgroup size / min / max, device-create result), `qcomprobe-detail: E<n> ...` (one row per case), and the last line `qcomprobe: summary E1=... pass= fail= unsupported= isolated=1 device_lost_any=`. Exit status 0 unless setup failed (2), a watchdog fired (3), or `--strict` and a FAIL (1).

No experiment can hang or take the others down: the process is a parent that runs each experiment in its own child process (`--child`), every child arms `alarm()` with a watchdog line and `_exit(3)`, every submit waits on a bounded fence (`--wait-ms`), a timeout or `VK_ERROR_DEVICE_LOST` marks the device lost and the remaining GPU work in that child is reported `FAIL reason=device_lost_earlier`, and a crash of a child is a `FAIL reason=child_crashed signal=N` for that experiment only. (Found while testing: the E3 "no STORAGE usage" rows write a storage descriptor for a view without STORAGE usage, which crashes lavapipe with SIGSEGV; each such row runs in its own child, so on lavapipe the mock run reports `result=CRASH(signal=11)` rows and still prints E3.)

The probe creates its device the way the library's `LAGFX_QCOM_TILE=1` path would: extensions `VK_QCOM_tile_properties` + `VK_QCOM_tile_shading`, and only the reported subset of {tileShading, colour attachments, per-tile dispatch, dispatch-tile, input/sampled attachments, atomics} + `tileProperties`; `VK_KHR_dynamic_rendering_local_read` when listed. If the device refuses that chain it retries without it, logs both `VkResult`s (`info device_create attempt=0 ... result=`) and reports E1 - E5 `UNSUPPORTED reason=device_create_refused_qcom_chain`. Needs Vulkan 1.3 (dynamic rendering, synchronization2, 1.3 entry points); the Adreno 840 reports 1.4.295.

### 7.3 Experiments

Status columns: **I** implemented in `tools/qcom-tile-probe`; **B** build-verified (gcc `-Wall -Wextra` against the 1.3.275 headers, run on lavapipe, plus `spirv-val` of the validatable modules); **D** device-verified. D is NO everywhere. NDK cross-compile: not done (no NDK on the host; section 7.7 gives the exact CI step).

| # | Question | What the probe does | CPU model / pass rule | I | B | D |
|---|---|---|---|---|---|---|
| E0 | Does the driver accept the extension pair + features at `vkCreateDevice`? | The existing app with `LAGFX_QCOM_TILE=1` (unchanged, see section 6). The probe's `info device_create` lines answer the same question without the app. | `g7-gpu-qcomtile: ... decision=enable enabled=1 create_result=0 fallback=none` or `fallback=device_create_failed` + `create_result`. | YES (section 6) | YES (unit) | NO |
| E1 | Real hardware tile size / apron / origin for several render areas and formats (gap 2) | `vkGetDynamicRenderingTilePropertiesQCOM` for R8G8B8A8, B8G8R8A8, R16G16B16A16F x 9 render areas (16x16 ... 1920x1080, two with a non-zero offset) x with and without the tile-shading chain in `VkRenderingInfo`. | PASS = every query `VK_SUCCESS` and tile > 0. Reported: `tile_min/tile_max`, `distinct`, `metal32_fits_all` (tile >= 32x32 in both dimensions), `gran_multiple` (tile is a multiple of `tileGranularity`), `apron_nonzero`, `origin_nonzero`. | YES | YES (UNSUPPORTED path on lavapipe; query code not runnable) | NO |
| E2 | `TileOffsetQCOM` / `TileDimensionQCOM` values and the storage tile attachment coordinate convention (gap in section 4) | Tile-shading pass (clear -1), per-tile bracket, one `vkCmdDispatch` of an 8x8 kernel that writes `(TileOffset.x, TileOffset.y, TileDimension.x, TileDimension.y)` into the pixel `TileOffset + localId + 8 * workgroupId`, bounded by tile, render area and push-constant rectangle. Variants: attachment layout GENERAL / COLOR_ATTACHMENT_OPTIMAL (descriptor stays GENERAL), tile image declared Arrayed (coordinate uvec3), render-pass flags ENABLE|PER_TILE_EXECUTION, and (only if the absolute form fails) a tile-relative store `pixel - TileOffset`. | PASS = at least one variant covers every pixel exactly once and each pixel's recorded tile contains it. `coord_convention=framebuffer_absolute` when the absolute form passes, `tile_relative` when only the relative one does. Also `tile_builtin=WxH` and `tiles_seen`. | YES | YES (generator, spirv-val of the twin; run path UNSUPPORTED on lavapipe) | NO |
| E3 | Which colour attachment formats the tile path accepts, with and without STORAGE usage (the `skip_rt` gate) | R8G8B8A8_UNORM, B8G8R8A8_UNORM (storage image format Unknown, needs `shaderStorageImageWriteWithoutFormat`), R16G16B16A16_SFLOAT, R32G32B32A32_SFLOAT, A2B10G10R10_UNORM x {STORAGE, no STORAGE}: image, view, shader module, pipeline, descriptor, per-tile dispatch writing the pattern `((x&15)/d, (y&15)/d, ((x>>4)&15)/d, 1)` (`d` = 15 for unorm, 16 for float), readback. Each row prints `storage_feature` / `color_att_feature` (format properties) and the first failing stage with its `VkResult`. | Row result `ACCEPTED_PASS` (equal to the model; tolerance 0.004 unorm8, 0.002 RGB10A2, exact for the floats), `ACCEPTED_BAD_PIXELS`, `REFUSED` (stage + VkResult), `FAILED_RUN`, `CRASH(signal=N)`, `UNSUPPORTED_PREREQ`. Experiment PASS = the RGBA16F + STORAGE row passes (so the matrix is meaningful); the per-row results are the answer. The no-STORAGE rows are outside the core validity rules (a storage descriptor on a view without STORAGE usage) and tell only what this driver tolerates; `--no-risky` skips them. | YES | YES for the analysis (mock run: all five formats pass the model on lavapipe, plain kernels) | NO |
| E4 | Per-tile read / modify / write on a tile attachment | Pattern written by a plain compute pass, then a tile pass with `LOAD_OP_LOAD` over the full area and over a sub-area (offset 8,8), per-tile `vkCmdDispatch` of `out = (in.g*0.5 + 0.125, in.r*0.5 + 0.25, in.b, in.a)` (exact on multiples of 1/16 in half precision). RGBA16F and RGBA8. | Equality with the CPU model on every pixel (RGBA16F bit-exact; RGBA8 tolerance 0.005); pixels outside the render area must keep the pattern. Experiment PASS = RGBA16F full area passes. | YES | YES for the analysis (mock) | NO |
| E5 | Draw -> per-tile dispatch -> draw in ONE render pass instance equals the split-pass result (gap 5) | Reference: render (clear, scissored coordinate-pattern draw), end, plain compute RMW over the image, render again with LOAD (second scissored draw). Tile variants, one pass instance, tile shading ENABLE, two draws outside the bracket and the dispatch between them, `VK_ACCESS_2_SHADER_TILE_ATTACHMENT_*` by-region barriers (only if `dynamicRenderingLocalRead`): `tile_bracket_dispatch` (`vkCmdDispatch` in the bracket), `tile_bracket_dispatchtile_rate1` (`vkCmdDispatchTileQCOM` in the bracket, shader with `TileShadingRateQCOM 1,1,1` + LocalSize), `tile_nobracket_dispatchtile_rate1` (no bracket), `..._rate1_nolocalsize` (rate mode without LocalSize), `..._rate2x2` (rate 2x2, each invocation writes 4 pixels). | Pixel-exact CPU model of clear / scissored draws / RMW (all values dyadic, so exact) for the reference and every variant; the row also prints `bytes_differing_from_split`. Experiment PASS = no executed variant FAILs and at least one tile variant matches. `split_matches_model` must be 1 (it is 1 on lavapipe). On a device without the extension the reference still runs: `status=UNSUPPORTED split_reference=PASS`. | YES | YES for the reference + model (lavapipe, with validation layers: no error from these paths); tile variants not runnable | NO |
| E6 | Logical SIMD32 on the device subgroup (64 here) and a region-limit case, with the existing emulation idea | Kernel (LocalSize 64x1): `v = ((3x + 5y) mod 61) + 1`; logical-32 butterfly reduction with `OpGroupNonUniformShuffleXor` for 16, 8, 4, 2, 1 (stays inside the aligned 32-lane half of a 64-lane subgroup) and an "up by one" shuffle clamped at logical lane 0 (`OpGroupNonUniformShuffle` to `lane == lane & ~31 ? lane : lane-1`), i.e. the remapped lane id / size / shuffle of `simd_emul`; writes `(sum, up, physicalLane, subgroupSize)`. The group operations run outside the store guard, so lanes whose pixel is clipped still take part (the Metal rule that a clipped lane stays in the simdgroup). Region-limit: a push-constant rectangle (5,3) 90x40 inside a 128x48 area, not aligned to 32 lanes; pixels outside must keep the clear value. Two parts: plain compute (the emulation as it is today) and the per-tile dispatch. | Model: sum of `v` over the 32 logical lanes of the row segment and the shifted value, computed from the lane the device reports; `subgroup_size_seen` is the observed `SubgroupSize` (must be >= logical width). PASS needs subgroup size >= 32 (otherwise `UNSUPPORTED reason=device_subgroup_N_below_32...` with the generator self-test result at logical width N, which is what runs on lavapipe: plain part PASS at width 8) and every executed part equal to the model. Reports `min/max` subgroup size (`VkPhysicalDeviceVulkan13Properties`) so whether 32 can be pinned is visible. | YES | YES for the plain part at logical width 8 (lavapipe) and the mock tile part; not the 64-lane case | NO |
| E7 | Tile memory heap properties and a tile-bound image (old E6) | Not implemented (needs `VK_QCOM_tile_memory_heap` structs; no feature/property values exist in the log). | - | NO | NO | NO |

Not covered, on purpose: any use of tile attachments by the library, the second flag `LAGFX_QCOM_TILE_NATIVE=1` (skipped: it could only be inert until E2 - E5 give results, and an inert flag adds a code path with nothing to test), tile aprons, depth/stencil/input tile attachments, per-tile draws, the fragment stage.

### 7.4 SPIR-V

[V] token values, section 7.1. The modules are emitted at run time by `qp_spv.h` (no assembler is needed on the phone), as SPIR-V 1.3. `OpExtension "SPV_QCOM_tile_shading"`, `OpCapability TileShadingQCOM` (4495), the tile image as `OpTypeImage` 2D storage (`Sampled = 2`, formats Rgba8 / Rgba16f / Rgba32f / Rgb10A2, or Unknown with the WithoutFormat capabilities) in storage class 4491 with `DescriptorSet 0` / `Binding 0`, builtins 4492 (uvec2) / 4493 (uvec3), execution mode 4490 (three literals) for the `vkCmdDispatchTileQCOM` kernels. One push-constant block of eight uints (render area rectangle + region rectangle). Entry points list only Input / Output variables (SPIR-V 1.3 rule).

Validation: the installed `spirv-val` (SPIRV-Tools v2025.1) rejects capability 4495 (`Invalid capability`), so the tile modules cannot be validated directly. `--dump-spirv DIR` therefore writes, next to each tile module, a `*.twin.spv` that has the identical structure but core tokens in place of the QCOM ones (no capability / extension / rate mode, UniformConstant storage image, `WorkgroupId` / `NumWorkgroups` in place of the two builtins, a LocalSize added where the real module deliberately has none). All twins and all plain modules pass `spirv-val --target-env vulkan1.3`; the test `tests/qcom-tile-probe-check.py` also asserts that exactly the real tile modules carry capability 4495. What this does not prove: that the driver accepts the real tokens in these positions ([N] until the device run).

### 7.5 What the device run can and cannot show; unknowns the variants exist for

- Whether the storage tile attachment image must be declared Arrayed, which attachment layout the descriptor needs, whether `ENABLE` alone or `ENABLE|PER_TILE_EXECUTION` is right for a primary command buffer, whether `vkCmdDispatchTileQCOM` needs the bracket, whether the rate-mode kernel needs a LocalSize: the spec texts leave these open, each is one variant (E2 variants, E5 variants) and the row says which worked.
- Risky cases, off with `--no-risky`: (a) E3 rows without STORAGE usage (storage descriptor on a view that lacks STORAGE usage), (b) E2 `rel_general` and `abs_general_pertile_flag` (tile-relative store, which writes outside the tile if the convention is absolute; every write is still bounded by the tile and the render area in the shader, so no out-of-bounds access happens), (c) `tile_nobracket_dispatchtile_rate1`. None can hang the probe; a crash is reported as FAIL for that experiment (or CRASH for that E3 row) and the next experiment runs in a new process.
- Expected outputs on the phone are not predicted here beyond the line format; the Adreno 840 record of section 2 only says the features exist (`tileShading=1 colorAtt=1 perTileDispatch=1 dispatchTile=1 atomicOps=1 apron=0`, granularity 16x16, rate 8x8).

Expected lavapipe output (a device with no QCOM extensions; this is the run the test checks; times and device name vary):

```
qcomprobe: E1 status=UNSUPPORTED reason=VK_QCOM_tile_properties_not_listed
qcomprobe: E2 status=UNSUPPORTED reason=extension_not_listed
qcomprobe: E3 status=UNSUPPORTED reason=extension_not_listed
qcomprobe: E4 status=UNSUPPORTED reason=extension_not_listed
qcomprobe-detail: E5 variant=split_reference barriers=plain_pipeline_barriers result=PASS
qcomprobe: E5 status=UNSUPPORTED split_reference=PASS tile_bracket_dispatch=UNSUPPORTED ... split_matches_model=1 variants_run=1 passed=1 failed=0 reason=extension_not_listed
qcomprobe: E6 status=UNSUPPORTED subgroup=8 ... plain=PASS tile=UNSUPPORTED ... reason=device_subgroup_8_below_32_selftest_of_generator_only
qcomprobe: summary E1=UNSUPPORTED E2=UNSUPPORTED E3=UNSUPPORTED E4=UNSUPPORTED E5=UNSUPPORTED E6=UNSUPPORTED pass=0 fail=0 unsupported=6 isolated=1 ...
```

On the phone each line is PASS, FAIL or UNSUPPORTED with the key=value facts; a line whose experiment printed `status=PASS` on a `mock=1` run is not a device result.

### 7.6 Decision rules after the device run

1. **E1**: if `metal32_fits_all=0` (hardware tile smaller than Metal's 32x32 threads-per-tile) a kernel that treats "my threadgroup is my tile" does not map one to one; the native path must express reach per hardware tile or stay on the emulation. If `FAIL` with `VK_ERROR_UNKNOWN`, tile properties are not usable for our pass shapes: stop.
2. **E2** `FAIL` or `coord_convention=unknown`: no native path; fix nothing in the library. `framebuffer_absolute`: the translator's `WorkgroupId*WorkgroupSize` add is replaced by `TileOffsetQCOM` plus local id; `tile_relative`: the add is dropped. Record which E2 variant passed (layout, Arrayed, render-pass flags): the library must use exactly that.
3. **E3**: formats with `ACCEPTED_PASS` and STORAGE usage are the formats the `skip_rt` gate may lift; only if a format passes without STORAGE usage may the STORAGE-usage requirement go away (a driver that merely tolerated it is not a reason). `REFUSED` / `ACCEPTED_BAD_PIXELS` keep the gate for that format.
4. **E4** and **E5**: only if E4 PASS (RGBA16F) and E5 has a passing tile variant equal to the split reference is draw / dispatch / draw in one instance usable; the passing variant fixes the vehicle (bracket + `vkCmdDispatch` keeps today's SpecId 0/1/2 scheme; `vkCmdDispatchTileQCOM` only if the kernel's thread layout is rate-based). Any `bytes_differing_from_split > 0` with a model match means the model, not the split result, is the spec: investigate before using.
5. **E6**: PASS keeps the logical-32 emulation correct under tile shading (gap 4) and shows the region-limit guard can be done in the shader against the render-area rectangle; `lane_layout_bad > 0` means workgroup invocations are not laid out lane-linearly and the emulation must not assume it. `subgroup_size_seen` < 32 would invalidate the emulation itself.
6. **Go / no-go**: build a native path behind a second flag only if E1, E2 and E4 pass, E5 has a passing tile variant, E6 passes, and the measured GPU time / bandwidth gain of one instance over the split path (not measured by this probe: it reads back the result but does not time it) is real. Otherwise the compute emulation stays. A native path is never the default.

### 7.7 Build and CI

`meson compile` builds `qcom-tile-probe` when the Vulkan loader is found (target in `tools/meson.build`); the meson test `qcom-tile-probe (selftest, spirv-val, lavapipe run: E1-E5 UNSUPPORTED, exit 0)` runs `tests/qcom-tile-probe-check.py`: CPU selftest, SPIR-V generation + `spirv-val` of the twins and plain modules, the full run (E1 - E5 must be UNSUPPORTED on a device without the extension, E5's split reference and E6's plain part must be PASS, exit 0) and the mock run (E3 / E4 PASS on plain kernels, E6 tile part PASS). On a device that lists the extension the test only checks the line format. `.github/workflows/ci.yml` runs `meson test`, so the probe test is covered by the existing `linux-meson` job without a workflow change. **Workflow change not made (described only):** to also cross-compile the probe for Android, add to job `linux-meson` after the step "Compile":

```yaml
      - name: qcom-tile-probe cross-compile (Android NDK, arm64, link only)
        run: |
          NDK="${ANDROID_NDK_LATEST_HOME:-$ANDROID_NDK_HOME}"
          CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
          "$CC" -std=c11 -O2 -Wall -Werror -D_GNU_SOURCE -Itools/qcom-tile-probe \
            tools/qcom-tile-probe/qcom_tile_probe.c -o build/qcom-tile-probe-android -lvulkan -lm
          file build/qcom-tile-probe-android | grep -q 'ARM aarch64'
```

([N]: that the hosted runner image ships an NDK in `ANDROID_NDK_LATEST_HOME`, and that r27c is the one in use, was not checked; the README says r27c is what the project uses.)

## 8. Open questions

Hardware tile size for the repo's pass shapes (E1). Storage tile attachment coordinate convention and descriptor layout (E2). Accepted formats (E3). Per-tile ordering with draws (E5). Interaction with local-read / raster-order (the probe enables local-read when listed; the library's raster-order attachment access is not enabled by the probe: [N]). Logical SIMD32 under tile shading (E6). Explicit imageblock modelling (E7, maybe never).

## 9. Primary sources used

- Vulkan registry `vk.xml` (KhronosGroup/Vulkan-Headers, `registry/vk.xml`): extension entries #310, #485, #548.
- docs.vulkan.org: feature proposals `VK_QCOM_tile_shading`, `VK_QCOM_tile_memory_heap`; ref page `VK_QCOM_tile_shading`.
- SPIRV-Registry: `SPV_QCOM_tile_shading` (revision 1, 2025-04-15): capability 4495, storage class 4491, execution modes 4489/4490, BuiltIns 4492-4494.
- Apple, Metal Shading Language Specification: 2.11 Imageblocks, 4.5 Threadgroup Imageblock Address Space, 5.1.9 Tile Functions, 5.6.3 - 5.6.5.
- Device log `g7_lagfx_1795760879033.txt` (lines 15 - 44, `g7-gpu-tile` lines).
