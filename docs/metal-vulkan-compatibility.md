# Metal → Vulkan compatibility matrix

[English](metal-vulkan-compatibility.md) · [한국어](metal-vulkan-compatibility.ko.md) · [README](../README.md)

Baseline: branch `research/android-pfn-shift`, source revision `6a29b1d` (2026-11-26).
Upstream baseline: [MattJackson/libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan) at `7574eac`.

This matrix is derived from the source and commit history of this fork, not from the upstream README's claims.
It exists to keep the supported surface honest: recognising an opcode, logging it, or returning a stamp is **not** Metal support.
Update this file in the same commit or PR that changes a feature or its status.

## 1. How to read this document

### Status

| Status | Meaning |
| --- | --- |
| Implemented & Verified | Code exists and a test for this fork passed (unit test run, or Android device test). The scope of the test is stated. |
| Implemented, Untested | Code exists and performs the described work; no test of it has been run for this fork. |
| Partially Implemented | Part of the feature works; the rest is missing or log-only. |
| Stub / Placeholder | The command is parsed, logged or acknowledged. No Vulkan work and no state change is made. |
| Not Implemented | No handling beyond the generic "unknown opcode" fall-through, or an explicit not-implemented return. |
| Experimental | Implemented for a limited setup that is still being investigated; behaviour may be wrong or change. |

Upstream-reported results (for example, live macOS guest rendering on lavapipe) are noted in the Evidence column. They are not repeated by this fork and do not by themselves raise a status to *Verified*.

### Origin

| Origin | Meaning |
| --- | --- |
| Upstream | Present in upstream at `7574eac` (Matthew Jackson). Not written by this fork. |
| Fork | Added by this fork (commits after `7574eac`). |
| Fork modification | Upstream feature changed or extended by this fork. |
| Planned | Not implemented. Listed so the gap is visible. |

### Kind

| Kind | Meaning |
| --- | --- |
| Vulkan | Produces Vulkan objects or commands (pipeline, image, draw, copy, …). |
| Protocol | Decodes or answers the PVG wire protocol (rings, stamps, info replies, registry) without a Vulkan equivalent. |

### Evidence sources

- **Android device** — developer-reported tests on Galaxy S26 Ultra / Adreno 840 at library revision `7574eac` (see README). Standalone test harness in Henge, not a live PVG session.
- **Unit run 2026-11-26** — `meson test` on x86-64 Linux, gcc 13.3, **no Vulkan loader** (74 tests: 72 passed, 0 failed, 2 skipped). Code under `LAGFX_HAVE_VULKAN` was not compiled or run in that build, so it proves protocol and parsing logic only.
- **Upstream-reported** — claimed in the upstream README or changelog; not reproduced here.
- **None** — no test known for this fork.

## 2. Matrix

### 2.1 Shader

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| Metal AIR → SPIR-V translation (vertex, fragment) | Upstream | Vulkan | Implemented & Verified | Android device: 11 login-screen modules / 66 functions translated, SPIR-V matches host SHA-256 and passes `spirv-val` (vulkan1.2). Unit run: all `air2spv` tests pass. |
| `.metallib` extraction, entry-point rewrite, signature transform | Upstream | Vulkan | Implemented & Verified | Exercised by the same translation corpus. |
| Fork change to the translator | Fork modification | Vulkan | Implemented, Untested | `LAGFX_NO_PROCESS` build option removes the `system()`/`popen()` debug and `llc` lookup calls (Android runner forbids process creation). A 4-line change in `shader_translate.c`. |
| GPU execution of translated shaders | Upstream | Vulkan | Partially Implemented | Android device: `ColorFill` and `TextureCopy` fragment shaders drew correctly (4,096/4,096 px each). `UberComposite`, `Vfx`, `Xgc` and the login vertex shaders: translation only, GPU execution **not tested**. |
| Stock shader catalog (clear/blit helpers) | Upstream | Vulkan | Implemented, Untested | Catalog tests ran without Vulkan; the Vulkan-dependent parts self-skipped. SPIR-V blobs are committed. |
| Compute shaders (MSL kernels → SPIR-V compute) | — | Vulkan | Not Implemented | No `vkCreateComputePipelines` or `vkCmdDispatch` in the tree. |
| Tessellation, mesh, ray tracing | — | Vulkan | Not Implemented | Patch draw opcodes are log-only (see 2.6). |

### 2.2 Render pipeline

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| `SetRenderPipelineState` (0x74) → `VkPipeline` build and per-task cache | Upstream | Vulkan | Implemented, Untested | Builds from translated vertex+fragment SPIR-V. Android device tests use a standalone harness, not this live path. |
| Premultiplied source-over blending | Upstream | Vulkan | Implemented, Untested | Android device "alpha blending" test used a GLSL pipeline, not the guest path. |
| Fixed-function state setters: `SetCullMode`, `SetFrontFacingWinding`, `SetDepthBias`, `SetDepthClipMode`, `SetTriangleFillMode`, `SetStencilRef`, `SetBlendColor`, `SetDepthStencilState`, `SetVisibilityResultMode`, `SetLineWidth`, `SetPointSize`, … | Upstream | Vulkan | Stub / Placeholder | Handlers in `render_inner_ops.c` parse and trace only. |
| Store-action setters (`SetColorStoreAction`, `SetDepthStoreAction`, `SetStencilStoreAction`, `…Options`) | Upstream | Vulkan | Stub / Placeholder | Parse and trace only. |
| Per-draw viewport and scissor (`SetViewport` 0x82, `SetScissorRect` 0x75) | Upstream | Vulkan | Implemented, Untested | Stored per task and applied to draws. Multi-viewport/scissor (`SetViewports`, `SetScissorRects`) are log-only. |

### 2.3 Compute pipeline

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| Compute encoder commands and dispatch | — | Vulkan | Not Implemented | The table in `compute_inner_ops.c`, used for encoder types 0/1, handles **render-namespace** opcodes. No dispatch path exists. |
| `DispatchThreadsPerTile*`, tile buffers/textures | Upstream | Vulkan | Stub / Placeholder | Parse and trace only. |

### 2.4 Buffer

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| Guest task memory mapping (memfd + `mmap(MAP_FIXED)` + `mremap` alias), 3-level radix VA→GPA | Upstream | Protocol | Implemented & Verified | Unit run: `memory task`, `memory coherence`, `page-walked translation regression`. |
| Vertex and fragment buffer binding (`SetVertexBuffers` 0x7d, `SetVertexBufferOffset` 0x7e, `SetFragmentBuffers` 0x6e, `SetFragmentBufferOffset` 0x6f) | Upstream | Vulkan | Implemented, Untested | Resolved through the task page table into host buffers. |
| Host-visible upload memory: coherent-memory requirement, map/flush error propagation (3 buffer helpers, fallback UBO) | Fork modification | Vulkan | Implemented, Untested | A mock fault-injection test exists (`tests/android-memory-mock.c`). It was not run in the 2026-11-26 session (needs Vulkan headers) and no CI run is recorded. Real Vulkan allocation paths not run. |
| Vertex and index buffer `Set…WithStride`, tessellation factor buffers | Upstream | Vulkan | Stub / Placeholder | Parse and trace only. |
| Buffer ↔ buffer / buffer ↔ texture copies (blit encoder) | Upstream | Vulkan | Stub / Placeholder | See 2.7. |

### 2.5 Texture

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| Texture realization from descriptors (RGBA16F, R8_UNORM glyph atlases, bpp-aware sizes, dimension clamps) | Upstream | Vulkan | Implemented, Untested | Used by fragment texture binding. |
| `SetFragmentTextures` (0x72) | Upstream | Vulkan | Implemented, Untested | Android device: `TextureCopy` sampled a **uniform-colour** texture only. Patterned texture / UV accuracy **not tested**. |
| `SetVertexTextures` (0x81) and tile textures | Upstream | Vulkan | Stub / Placeholder | Parse and trace only. |
| Sampler states (`SetFragmentSamplerStates` 0x70, vertex/tile variants, LOD clamp) | Upstream | Vulkan | Stub / Placeholder | Handlers log the first entries and return; `TODO` for descriptor binding. A module default sampler is used by translated shaders. |
| Pixel format conversion for render targets and blits | Upstream | Vulkan | Partially Implemented | RGBA16F and 8-bit paths exist; coverage of all Metal pixel formats is **not established**. |

### 2.6 Render pass and draw

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| `RenderDescribeRenderPass` (0x1a): 584-byte descriptor parse, colour attachment resolution | Upstream | Vulkan | Implemented, Untested | Depth/stencil attachment handling and MSAA not verified. |
| Draw: `DrawPrimitives16/64` (0x01/0x00), `DrawInstancedPrimitives16` (0x03), `DrawIndexedPrimitives16/64` (0x07/0x06) | Upstream | Vulkan | Implemented, Untested | `vkCmdDraw` / `vkCmdDrawIndexed` via `lagfx_emit_pending_draw`. Upstream reports live rendering of guest geometry on lavapipe. Android: not run through live PVG. |
| Other draws: `DrawInstancedPrimitives64`, `…Base…`, `DrawIndexedInstanced…`, `…_2` variants | Upstream | Vulkan | Stub / Placeholder | Parse and trace only; not in the encoder-type 0/1 table, so they are dropped there. |
| Patch draws (`DrawPatches*`, `DrawIndexedPatches*`) | Upstream | Vulkan | Stub / Placeholder | Parse and trace only. |
| Indirect draws (`Draw*Indirect`) | Upstream | Vulkan | Stub / Placeholder | Parse and trace only. |
| `ExecuteCommandsInBuffer`, nested command buffer (0x3c) | Upstream | Vulkan | Stub / Placeholder | Recursion deferred (TODO in source). |

### 2.7 Blit

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| `CopyFromTextureToTexture` (0x12f) | Upstream | Vulkan | Implemented, Untested | `lagfx_translate_blit_copy_texture_to_texture` records `vkCmdCopyImage`. The unit test ran without Vulkan, so this path was not exercised. |
| `FillTextureWithColor` (0x141) | Upstream | Vulkan | Implemented, Untested | `lagfx_translate_blit_fill_texture_color` records `vkCmdClearColorImage`. Not exercised in the unit run. |
| `CopyFromBufferToTexture`, `CopyFromBufferToBuffer`, `CopyFromTextureToBuffer`, `FillBuffer`, `FillBufferWithPattern`, `FillTextureWithBytes`, `GenerateMipmaps`, `…WithOptions`, `…NumSliceLevel` | Upstream | Vulkan | Stub / Placeholder | `op_ack_stub`: acknowledged, nothing done. |
| `Optimize*`, `Synchronize*`, `Invalidate*`, indirect command buffer ops | Upstream | Vulkan | Stub / Placeholder | `op_ack_stub`. |

22 of the 24 blit opcodes are acknowledge-only.

### 2.8 IOSurface

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| IOSurface opcodes 0x26–0x2a: wire parsing, resource-registry register/lookup/unregister | Upstream | Protocol | Implemented, Untested | Registry unit tests pass. Layouts are partly conjectured upstream. |
| `VkImage` allocation inside the IOSurface opcode handlers | Upstream | Vulkan | Not Implemented | Header of `handlers/iosurface/iosurface.c` states the path was not reinstated. |
| `VkImage`-backed surfaces created on demand (texture realize, render target) | Upstream | Vulkan | Implemented, Untested | Android device: render-target clear/readback 4,096/4,096. |
| Sharing an IOSurface between guest tasks and host display | Upstream | Vulkan | Partially Implemented | Registry maps IDs to host metadata; zero-copy sharing not verified. |

### 2.9 Synchronization

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| Stamp cells (monotonic, `>=` compare), MSI vector 0, pending-display bitmask | Upstream | Protocol | Implemented & Verified | Unit run: stamp and deadlock tests. **Protocol-level only**: a stamp is signalled after parsing, not after GPU completion. Not a Metal feature. |
| `CmdSynchronizeResources` (0x42) | Upstream | Protocol | Not Implemented | Logs and returns `NOT_IMPLEMENTED` ("cache flush not implemented yet"). |
| `CmdSynchronizeDiscard` (0x23) | Upstream | Protocol | Stub / Placeholder | Log only. |
| Metal barriers: `RenderBarrierScope` (0x17), `RenderBarrierResources`, `TextureBarrier` | Upstream | Vulkan | Stub / Placeholder | Log or acknowledge only. Internal image-layout barriers exist inside the Vulkan helpers; they are not translated from Metal barriers. |
| Metal fences: `RenderUpdateFence`/`WaitForFence`, `BlitUpdateFence`/`WaitForFence` | Upstream | Vulkan | Stub / Placeholder | Trace or acknowledge only. |
| Host-side per-draw fence wait (default 30 s, `LAGFX_DRAW_FENCE_MS`) | Upstream | Vulkan | Implemented, Untested | Internal serialization, not a translation of Metal fences. |

### 2.10 Display

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| Display create/free, `lagfx_display_read_frame` readback | Upstream | Vulkan | Implemented, Untested | Android device: render-target readback only; no display session. |
| `CmdDisplaySwapMapping` (0x12), scanout blit and composite | Upstream | Vulkan | Implemented, Untested | |
| `CmdDisplaySetProperties` (0x11) mode commit | Upstream | Protocol | Stub / Placeholder | Log-ack; `TODO: Stage 30 mode commit`. |
| Fence-error handling before releasing submitted display resources | Fork modification | Vulkan | Implemented, Untested | Commit `8f36a76`. |
| MMIO `0x122c` read ("cap gate") | Fork modification | Protocol | Experimental | Returns 9 in x86 mode (unchanged) and 1 in arm64 mode, where Apple Henge answers the number of displays. Unit-tested only. |

### 2.11 Cursor

| Feature | Origin | Kind | Status | Evidence / notes |
| --- | --- | --- | --- | --- |
| Cursor glyph upload and overlay draw (`vkCmdDraw`) | Upstream | Vulkan | Implemented, Untested | |
| Host-visible/coherent memory selection, non-coherent flush, layout and error-status tracking | Fork modification | Vulkan | Implemented, Untested | Commits `a746845`, `dcd6ae4`, `f59bad4`, `f139872`, `8f36a76`. Mock-level only. |
| Cursor position update from MMIO `0x1220` (`cursor_moved`) | — | Protocol | Not Implemented | Listed as unimplemented in `docs/arm64-vmapple-correspondence.md`. |

## 3. Capability replies versus Vulkan support

Info replies (`CmdGetDeviceInfo` 0x0a, `GetDeviceInfo2` 0x3a, compute info 0x0b, and the encoder-type-4 info family 0x1c2–0x1d0) tell the guest what the virtual GPU claims to support. They are **protocol answers**, implemented from observed guest behaviour. A reported capability does not mean the Vulkan backend can perform it.

| Reply | Origin | Notes |
| --- | --- | --- |
| x86 device-info table (`CmdGetDeviceInfo`) | Upstream | Values chosen so the macOS guest proceeds; not derived from the Vulkan device. |
| arm64 device-info table | Fork | Apple Henge `devices/pvg_lite.c` table copied verbatim. Of its 39 keys, 26 differ in value from the x86 table and 13 are equal; 3 keys exist only in the x86 table (see the correspondence document). No Vulkan capability check is made. |
| Pipeline/host-resource info replies (0x1c2–0x1d0) | Upstream | Non-zero replies keep the guest from aborting in `MetalShader::CopyPipelineState`. |

## 4. Known gaps summary

- Compute dispatch, tessellation, indirect and nested command buffers: not implemented.
- 22 of 24 blit opcodes, all Metal fences and barriers, most fixed-function state setters, vertex textures and sampler binding: log-only.
- `CmdSynchronizeResources` returns not-implemented.
- Pixel-format coverage and depth/stencil attachments are unverified on real hardware; MSAA is implemented from the decoder only, without a guest capture (section 5).
- No test of the live PVG path on Android exists. arm64 mode is not in any APK (see README, *Testing status*).

See also: [arm64-vmapple-correspondence.md](arm64-vmapple-correspondence.md).

## 5. Render compatibility: framebuffer fetch, formats, MSAA

Everything below was checked on lavapipe and the Khronos validation layer 1.3.275 only. That layer predates VK_KHR_dynamic_rendering_local_read and reports false errors for its barriers, so it cannot judge them; no result here predicts an Adreno driver.

- **Framebuffer fetch order (issue 11).** A fragment that reads the previous colour needs the earlier fragment of the same pixel to be finished. Without VK_EXT/ARM_rasterization_order_attachment_access (pipeline colour-blend flag 0x1) Vulkan gives that guarantee only between draws (a framebuffer-local barrier). When the device lacks the feature, a draw whose primitives or instances can overlap is recorded one primitive (or instance) at a time with that barrier in between (`fbf_draw_split`, cap 16384 sub-draws, counted as `fbf_split_*`). Triangle strips keep the per-triangle winding. Lavapipe renders one draw in order anyway, so the missing guarantee cannot show as a wrong pixel there: the test asserts the recorded sub-draw counts and that pixels equal the single-draw reference. A shader whose input-attachment numeric class (float/uint/sint) differs from the attachment format is refused and counted (`fbf_class_mismatch`).
- **Formats, usage, blend (issue 13).** `src/vulkan/format_caps.c` asks vkGetPhysicalDeviceFormatProperties2 and vkGetPhysicalDeviceImageFormatProperties2 for the usage mask of every mirror image. A refused combination drops optional usages one at a time and counts them (`img_trimmed`, `dropped[]`); a missing required usage counts a refusal. Blending is switched off and counted (`blend_off_format`) for a format without COLOR_ATTACHMENT_BLEND. A target that cannot be a colour attachment is counted (`target_not_attachable`).
- **MSAA (issue 12), implemented from the decoder, no guest capture.** Evidence [V]: the dumps in hand (/tmp/x231501, /tmp/vm2, /tmp/run3/z/dump) hold only BGRA8 (80) and RGBA16F (115) attachments with one mip, no tag 30..36, 102..106 or 150..152, and no multisample texture type, so NO guest capture of a multisampled pass exists. [I] fields read: texture descriptor type 4 (2D multisample, Metal numbering) with the sample count u16 at description +18 (one mip, one layer; type 8 and other combinations are refused and counted `ms_tex_bad_shape`); pass colour `store_action` (+0x16) and `resolve_texture_ref` (+0x04) with Metal's table (0 DontCare, 1 Store, 2 MultisampleResolve, 3 StoreAndMultisampleResolve, 4 and above unknown, counted); deferred tags 102 / 150 (x86-era payload layout, unverified on ARM64). [N] not decoded: the pipeline rasterSampleCount (the pipeline's count is taken from the pass attachments), alpha-to-coverage, sample mask, tag 30 / 32 payloads, depth / stencil resolve. Implemented (`src/protocol/arm64_msaa.*`, `src/vulkan/msaa.*`): the image is made with the granted count (`lagfx_msaa_plan_mode`: format caps and sampleCounts; EXACT by default, see the policy below), the pipeline gets `raster_samples` from the pass, the resolve is a dynamic-rendering resolve attachment per store action. Refused and counted, never guessed (all of them are printed under `unsupported[...]` in the `g7-gpu-msaa` line): framebuffer fetch / tile shaders / blits / capture of a multisampled attachment, shader sampling of a multisampled texture (the AIR spelling of texture2d_ms reads, get_num_samples, sample_id and sample_mask is only detected by name in `src/air2spv/msaa_air_scan.c`, counted as `air_ms_ops` / `air_sample_builtins`, not translated), mixed sample counts in one pass, depth / stencil resolve. Tests (lavapipe 1|4 samples + validation layer): `vulkan-msaa-coverage-unit` (CPU reference of the standard sample locations, per-sample fetch, resolve by store action and vkCmdResolveImage, BGRA8 and RGBA16F), `arm64-msaa-guest-unit` (guest descriptors through the exec path). Status: implemented, build-verified on lavapipe; device-verified NO. The next run must capture a real multisampled pass (raw tag 26 record, texture descriptor with its layout length, tags 102 / 150, the pipeline descriptor blob, AIR bitcode with texture2d_ms reads) and the `g7-gpu-msaa` line. The device-info MSAASamples value is lowered to the host maximum (floor 1) and counted (`devinfo_msaa_clamped`).
  **Policy: exact (default) versus compat (opt-in).** [V] code, tested on lavapipe; sample-count reduction (4x -> 2x -> 1x) and a 1x image copy as a "resolve" are NOT exact MSAA (different coverage pattern, no per-sample averaging), so they are not the default.
  - *exact* (default; `LAGFX_MSAA_COMPAT` unset or anything but `1`): the guest-requested sample count is used only if the device / format / usage / extent offers exactly that count (`lagfx_msaa_plan_mode`, a power of two up to 16 and the whole usage mask granted by vkGetPhysicalDeviceImageFormatProperties2). Otherwise the multisampled texture is refused (`ms_exact_refused`, `ms_tex_refused`; one `g7-gpu-msaa: mode=exact refused ...` warning, first 8 only), the texture does not exist for the guest, and a pass that names any refused or inconsistent multisampled attachment (a colour slot, depth, or a required resolve target: missing, not a texture of the same shape/format, or the resolve action without one) is rejected AS A WHOLE: no attachment is bound, nothing is drawn or written, `ms_pass_refused` is counted once and the specific `<slot>: <reason>` is logged (rate limited; `lagfx_a64_msaa_last_reject` in tests; `lagfx_a64_msaa_pass_gate`). Before this change a second colour attachment that was refused was dropped and the rest of the pass still drew (fail-before test: 2-colour pass, second refused). Nothing is approximated. A pass naming no multisampled attachment is unchanged. A pass never renders into an image whose host count differs from the guest's (`lagfx_a64_msaa_attach_ok`).
  - *compat* (`LAGFX_MSAA_COMPAT=1`, default OFF, read at every use): clamp down to the largest supported power of two (`ms_compat_clamped`) and, when no count > 1 works, a 1x image whose "resolve" is a copy (`ms_compat_1x`, `resolves_by_copy`). Logged (first 8) and counted separately from exact use. The `g7-gpu-msaa` line starts with `mode=exact` or `mode=compat`.
  - The pipeline `rasterizationSamples` is the pass's attachment count in both modes, and the pipeline cache key (formats, depth/stencil formats, topology and shader variant, **sample count**, layout key; `pipeline_for` in `arm64_exec_run.c`) includes it: the same shader and colour format at 1x and 4x are two VkPipelines (test below; without the sample count in the key the validation layer reports VUID-vkCmdDraw-multisampledRenderToSingleSampled-07285, checked by removing it).
  - Store actions [I] (Metal documentation; no guest capture): 0 DontCare -> `DONT_CARE`; 1 Store -> `STORE`; 2 MultisampleResolve -> resolve attachment + `DONT_CARE` store op (Metal leaves the multisampled contents undefined after it; Vulkan's `DONT_CARE` allows but does not guarantee that, so a later Load of that texture is NOT relied on); 3 StoreAndMultisampleResolve -> resolve attachment + `STORE` (the multisampled contents are kept: a later pass that Loads the texture sees them, tested); 4 Unknown, 5 CustomSampleDepthStore and others -> `STORE`, no resolve, counted `store_unknown`. A resolve action without a usable resolve texture stores and is counted `resolve_no_target`.
  - Not implemented, counted, and printed under `unsupported[...]` in the `g7-gpu-msaa` line: depth / stencil resolve (stored only: `ds_resolve`), shader reads of a multisampled texture (`texture2d_ms`, `get_num_samples`: `sampled_ms` for the draw, `air_ms_ops` for the translator), `[[sample_id]]` / `[[sample_mask]]` (`air_sample_id_mask`, seen in a translated module, never translated), framebuffer fetch, tile shaders, blits and capture of multisampled attachments.
  - Device-info MSAASamples (CmdGetDeviceInfo2 TLV key 0x01) [I]: the field is a plain maximum count (the guest gate is `value >= N`; 0 aborts SkyLight), so it cannot express a gap. The host keeps its real per-format sample-count masks and advertises the largest power of two (at most the library's 4 and at most 16) that EVERY common colour and depth format grants in exact mode (`lagfx_msaa_devinfo_max`), floor 1; a reduction is counted `devinfo_clamped`. Gap case (lavapipe 1|4): value 4, the 2x texture a guest may still request is refused in exact mode (`devinfo_gap`, `ms_exact_refused`). Tests with injected masks 1|4, 1|2|4, 1|2|4|8, 1 (`arm64-msaa-guest-unit`, `vulkan-msaa-unit`).
  - Host-side machinery, implemented and lavapipe-tested but NOT wired to the guest protocol or the AIR translator (`src/vulkan/msaa_host.*`, `tests/vulkan-msaa-host-unit.c`; the AIR spellings and the wire fields are not in any capture [N], nothing is bound to a guessed name, the refusal counters above stay in place): (a) multisample texel fetch: generated SPIR-V `OpImageFetch` with the `Sample` operand on an `OpTypeImage ... MS=1` image plus `OpImageQuerySamples`, a 2D sampled view helper that refuses 1x images and images without SAMPLED (`msfetch_views` / `msfetch_refused`), every sample of every pixel equal to the CPU coverage at 4x; (b) `SampleId` (capability SampleRateShading, pipeline sampleShadingEnable + minSampleShading, device feature `sampleRateShading`; a device without it refuses, `sample_shading_refused`) and `SampleMask` input/output, per-sample colour tests; (c) alpha-to-coverage and sample shading plumbed from the injectable `lagfx_msaa_inject` flag through the executor's pipeline build (`a2c_pipes`, `sample_shading_pipes`), checked against a CPU coverage model with documented tolerance (the spec leaves the pattern implementation defined: written samples are a subset of covered, 0 at alpha 0, all at alpha 1, within +-1 of alpha * N on fully covered pixels, monotonic in alpha, flag off = no effect); (d) depth / stencil resolve: probing of `VkPhysicalDeviceDepthStencilResolveProperties`, mode choice (`lagfx_msaa_dsres_choose`: Sample0 / Min / Max, stencil DepthResolved only with the same mode; a mode the device does not list or mixed modes without independentResolve are refused and counted `dsres_refused`, never replaced) and a dynamic-rendering resolve attachment helper; lavapipe offers only SAMPLE_ZERO (depth 0x1, stencil 0x1), so only SAMPLE_ZERO is run against a CPU reduction, MIN and MAX are covered by injected caps. The pass path still stores depth / stencil without resolving (`ds_resolve`).

  | Item | Implemented | Build-verified (lavapipe 1\|4 + validation layer, injected devices) | Device-verified |
  |---|---|---|---|
  | Exact mode (guest count or counted refusal), 1x and 4x, BGRA8 + RGBA16F | yes | yes (`arm64-msaa-guest-unit`, `vulkan-msaa-coverage-unit`) | NO |
  | Exact 2x / 8x accepted when the device reports them | yes | planner only, injected devices (lavapipe has no 2x / 8x); NOT rendered | NO |
  | Exact refusal of 2x / 3x / 8x on 1\|4 and of 4x on a 1-only device | yes | yes | NO |
  | Compat clamp-down and 1x + copy (opt-in) | yes | yes (8x -> 4x renders and resolves; 2x -> 1x copy); 2x -> 2x unverified | NO |
  | Pipeline cache key with the sample count (1x and 4x -> 2 VkPipelines) | yes | yes (fails without it) | NO |
  | Store action 2 / 3 resolve targets, action 3 keeps the multisampled contents | yes | yes (4x BGRA8, RGBA16F) | NO |
  | Content kept after action 2 | not promised | not asserted | NO |
  | Whole-pass rejection when any named MS attachment or required resolve target is refused / inconsistent (exact only) | yes | yes (2-colour with second refused, depth refused, resolve target refused: no draws, no writes; compat twins unchanged) | NO |
  | Device-info MSAASamples = largest exact grant of common colour + depth formats (plain max; gaps not expressible) | yes | yes (injected masks 1\|4, 1\|2\|4, 1\|2\|4\|8, 1) | NO |
  | MS texel fetch (`OpImageFetch` + Sample), host side | library function, not wired | yes (4x vs CPU coverage, spirv-val) | NO |
  | `SampleId` / `SampleMask`, sample shading, host side | library function, not wired | yes (4x per-sample colour; needs `sampleRateShading`) | NO |
  | Alpha-to-coverage, host side | pipeline flag from an injectable hook only | yes (CPU model, +-1 sample tolerance) | NO |
  | Depth / stencil resolve | mode choice + attachment helper, pass path NOT wired (counted `ds_resolve`) | SAMPLE_ZERO run on lavapipe, MIN / MAX / stencil by injected caps | NO |
  | AIR `texture2d_ms` reads, `get_num_samples`, `[[sample_id]]`, `[[sample_mask]]` translation | NO (counted `air_ms_ops`, `air_sample_id_mask`; spelling unknown [N]) | detection by name only | NO |
  | Guest wire fields (sample count, store action, resolve ref, tags 102 / 150) | read from the decoder | tests write the bytes the decoder reads | NO (no capture) |
