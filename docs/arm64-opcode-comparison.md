# ARM64 exec: inner command coverage vs. the independent reference

Scope: every Render / Compute / Blit inner command the independent reference derivation handles, against what the ARM64 exec path
(`src/protocol/arm64_exec_run.c`, `arm64_draw_decode.c`, `arm64_resources.c`, `arm64_sampler.c`) executes.

Reference (protocol facts only: opcode numbers, record layouts, semantics; no code taken): `medfa12/apple-silicon-anyos-project`,
`vendor/pvg-vulkan/crates/pvg-vulkan-wire/src/ops/{render,render_pass,tile,blit,compute,sampler,fence,icb,info,...}.rs`,
`pvg-vulkan/src/contract/draw.rs`, `runtime/exec/mod.rs`, `runtime/blit_exec/mod.rs`. Its layouts were derived by calling the Metal methods with distinctive
arguments and reading the records back. "Payload" below = bytes after the 8-byte `{tag, size}` header (total length - 8).

Status words: **implemented** (recorded on Vulkan), **no-op** (nothing to do by design, counted done), **partial** (recorded as seen, not applied; counted
`partial`), **missing-known** (reference gives layout and semantics; not implemented, reason given), **missing-unknown** (no layout known to us).
"Device" = counts seen on a real device (`/tmp/zx/logs/g7_*`, run 20261126-145344): tag(done/partial/unsupported).

Tag numbers are decimal in the code and in `g7-gpu-run tags(...)`; the reference writes hex.

## Render segment (type 0)

| tag (hex) | command | reference payload | ours | notes |
|---|---|---|---|---|
| 0x01 (1) | drawPrimitives (compact) | u32 prim, u16 start, u16 count (8) | implemented | device 172 done / 19 partial (no pipeline) |
| 0x00 (0) | drawPrimitives (wide) | u32 prim, u64 start, u64 count (20) | **implemented (new)** | wide form = every count 64 bit |
| 0x03 (3) | instanced draw | u16 start, count, instances, prim (8) | **implemented (new)** | instance count 0 draws nothing (done, `ext_draw_zero`) |
| 0x02 (2) | instanced draw (wide) | u64 start, count, instances; u16 prim (26 of 28) | **implemented (new)** | |
| 0x05 (5) | instanced + baseInstance | u16 start, count, instances, baseInstance, prim (10 of 12) | **implemented (new)** | `vkCmdDraw` firstInstance |
| 0x04 (4) | ... wide | u64 x4, u16 prim (34 of 36) | **implemented (new)** | |
| 0x07 (7) | drawIndexedPrimitives | u16 prim, u16 indexType, u32 ref, u16 count, u16 offset(bytes) (12) | implemented | device 372 partial: all skipped for `no_pipeline` (pipeline build failures), not for the opcode |
| 0x06 (6) | indexed (wide) | u16 prim, u16 indexType, u32 ref, u64 count, u64 offset (24) | **implemented (new)** | |
| 0x09 (9) | indexed, instanceCount | as 0x07 + u16 instances (14 of 16) | **implemented (new)** | |
| 0x08 (8) | indexed, instanceCount (wide) | as 0x06 + u64 instances (32) | **implemented (new)** | |
| 0x0b (11) | indexed, instances, baseVertex, baseInstance | u16 prim, indexType, u32 ref, **u16 offset, u16 count** (offset BEFORE count), u16 instances, i16 baseVertex, u16 baseInstance (18 of 20) | **implemented (new)** | baseVertex is truncated to 16 bit by Apple's serializer (not recoverable) |
| 0x0a (10) | ... wide | u16 prim, indexType, u32 ref, u64 offset, count, instances, i64 baseVertex, u64 baseInstance (48) | **implemented (new)** | |
| 0x10 (16) | drawPrimitives indirect | u64 indirectOffset, u32 indirectRef, u16 prim (14 of 16) | **implemented (new)** | argument block {vertexCount, instanceCount, vertexStart, baseInstance} read from the mirror buffer at record time (snapshot, as the reference does); no `drawIndirectFirstInstance` feature needed |
| 0x11 (17) | drawIndexedPrimitives indirect | u16 prim, indexType, u32 indexRef, u32 indirectRef, u64 indexOffset, u64 indirectOffset (28) | **implemented (new)** | block {indexCount, instanceCount, indexStart, baseVertex i32, baseInstance}; indexStart is the first index |
| 0x0c, 0x0d, 0x0f, 0x12, 0x13 (12, 13, 15, 18, 19) | drawPatches / drawIndexedPatches (+ wide, indirect) | 24 / 56 / 32 / 68 / 36 / 48 total | missing-known | tessellation: no tess pipeline path |
| 0x14, 0x15 (20, 21) | executeCommandsInBuffer indirect / range | 24 / 28 total | missing-known | needs ICB objects (0x36, 88 bytes) and an ICB executor |
| 0x16, 0x17, 0x85 (22, 23, 133) | memoryBarrier (resources / scope), textureBarrier | scope: 4 payload; texture barrier: 0 | **partial (new)** | counted `ext_barrier`; the reference does nothing either ("render_noop_barrier"); a real in-pass barrier would matter only for same-pixel hazards |
| 0x18, 0x19 (24, 25) | updateFence / waitForFence | u32 ref, u32 stages (8) | **no-op (new)** | one in-order command buffer per exec |
| 0x1a (26) | render pass descriptor | 584 | implemented (colour 0..3) | device 112 done; depth / stencil attachments: other agent |
| 0x1b (27), 0x86 (134), 0x87 (135), 0x89 (137) | useHeap, useHeaps, useResources, useResource | count + (usage/stages) + refs | **no-op (new)** | residency hints; all mirror objects are resident. Not gated on an open pass |
| 0x1e (30) | default raster sample count | u32 (4) | partial (existing) | tags 30..36 "describe the pass that follows" |
| 0x20 (32), 0x21 (33) | sample positions, rasterization rate map | | unsupported (existing) | |
| 0x22, 0x23, 0x24 (34..36) | imageblock sample length, threadgroup memory length, tile size | u32 (4) | partial (existing) | |
| 0x65 (101) | setBlendColor | 4 x f32 (16) | implemented (host-tested, not device-verified) | stored per encoder (reset to 0,0,0,0 by a new pass); pipelines with BlendColor factors (11..14) use `VK_DYNAMIC_STATE_BLEND_CONSTANTS` and `vkCmdSetBlendConstants` is recorded before each of their draws; see docs/g7-bgra8-blendconst.md |
| 0x66, 0x67, 0x69, 0x6a, 0x78, 0x79 (102, 103, 105, 106, 120, 121) | set{Color,Depth,Stencil}StoreAction(+Options) | 8..12 | missing-known | store action is always STORE |
| 0x68 (104), 0x77 (119) | setDepthStencilState, setStencilReference | u32 ref (4) / (8) | missing-known | owned by the depth/stencil work; not seen in `/tmp/zx` logs |
| 0x6b, 0x6c, 0x6d, 0x73, 0x7c, 0x88 (107, 108, 109, 115, 124, 136) | cull mode, depth bias, depth clip, front facing, fill mode, line width | | missing-known | raster owner |
| 0x6e (110) / 0x6f (111) | setFragmentBuffer / offset | {first, count}{u32 ref, u64 off} / {u32 idx, u64 off} | implemented | device 499 / 56 done |
| 0x70 (112) | setFragmentSamplerState(s) | {first, count}{u32 ref} | implemented | device 274 done |
| 0x71 (113) | setFragmentSamplerState lodMin/Max | {first, count}{u32 ref, f32 min, f32 max} (12 per entry) | **implemented (new)** | cached sampler variants, max 4 per sampler (`ext_samp_lod_dropped`) |
| 0x72 (114) | setFragmentTexture(s) | {first, count}{u32 ref} | implemented | device 534 done; only textures the mirror already holds |
| 0x74 (116) | setRenderPipelineState | u32 ref | implemented | device 541 done / 26 partial |
| 0x75 (117) | setScissorRect | 4 x u64 (32) | implemented | device 423 done |
| 0x76 (118), 0x83 (131) | setScissorRects / setViewports | count + array | missing-known | multiple viewports |
| 0x7a, 0x7b (122, 123) | tessellation factor buffer / scale | | missing-known | no tessellation |
| 0x7d (125) / 0x7e (126) | setVertexBuffer(s) / offset | as 0x6e / 0x6f | implemented | device 865 / 270 done |
| 0x7f (127), 0x80 (128), 0x81 (129) | vertex sampler / sampler LOD / texture | as 0x70 / 0x71 / 0x72 | missing-known | pipelines whose vertex stage uses textures/samplers are rejected at resolve (`rf_vertex_res`); 0x80 is decoded but reported unsupported |
| 0x82 (130) | setViewport | 6 x f64 (48) | implemented | device 394 done |
| 0x84 (132) | setVisibilityResultMode | u64 mode, u64 offset (16) | missing-known | occlusion queries |
| 0x99, 0x9a (153, 154) | vertex amplification mode / count | | missing-known | no multiview |
| 0x9b..0xa4 (155..164) | **tile shading**: dispatchThreadsPerTile, tile threadgroup memory, **0x9d set tile buffer**, 0x9e offset, 0x9f/0xa0 sampler (+LOD), **0xa1 tile texture**, **0xa2/0xa3 dispatch in region**, 0xa4 get tile dimensions | tile.rs: 8..76 | missing-known | **seen on device: 157 x4, 161 x2, 162 x2, all unsupported.** Needs a tile-function compute pass over the render area with imageblock semantics (framebuffer fetch alone does not give it); not low risk |
| 0xa5 (165), 0xa6 (166) | vertex buffer with attribute stride (+offset variant) | {first,count}{u32 ref, u64 off, u64 stride}; {u32 idx, u64 off, u64 stride} | missing-known | the serializer emits these only when `supportsDynamicAttributeStride` is on (off by default); needs dynamic vertex-input binding stride in the pipeline (pipeline_build is owned by the other agent) |
| 0x1c, 0x1d, 0x1f, 0x25..0x64, 0x8a..0x98 | - | none | missing-unknown | no reference entry; the x86-era table of `render_inner_ops.c` has names for some but they are not verified for ARM64 |

Setter payloads such as setBytes: the reference documents that **there is no inline-data form**: `setVertexBytes` / `setFragmentBytes` write the same
records as the buffer binds (0x7d / 0x6e / 0xa5), the serializer stages the bytes and names the staging buffer ref and offset. Nothing to implement.

## Blit segment (type 2); tag = 0x12c + n, in decimal 300..323

| tag (hex) | command | ours | notes |
|---|---|---|---|
| 0x12c (300) buffer to texture, 0x12d (301) buffer to buffer, 0x12e (302) texture to buffer, 0x12f (303) texture to texture | implemented | device: 300 x5 done |
| 0x130 (304) texture to texture with options (100 total) | missing-known | |
| 0x131 (305) copyIndirectCommandBuffer | missing-known | ICB |
| 0x132 (306) fillBuffer (u32 ref, u64 loc, u64 len, u8 value) | implemented | |
| 0x133 (307) generateMipmaps | missing-known | |
| 0x134..0x137 (308..311) optimize for CPU/GPU (+ slice/level), 0x13a (314) synchronizeResource, 0x13b (315) synchronizeTexture | no-op (existing) | hints; Apple does nothing either |
| 0x138, 0x139 (312, 313) optimize / reset ICB | missing-known | ICB; counted unsupported |
| 0x13c, 0x13d (316, 317) update / wait fence | **no-op (new)** | |
| 0x13e (318) copy texture slices / levels (28 total) | missing-known | |
| **0x13f (319) fillBuffer pattern4** (u32 ref, u64 loc, u64 len, u32 pattern; 32 total) | **implemented (new)** | location must be a multiple of 4; 1..3 byte tail takes the first pattern bytes |
| 0x140 (320) fill texture with bytes, 0x141 (321) fill texture with colour | missing-known | the reference does not apply them either ("lost work"); layouts 84 / 100 total. Library side (x86 dispatch only, not this ARM64 path): 0x141 is applied by `lagfx_translate_blit_fill_texture_color` (region + layout aware, see docs/spec-audit.md "Blit fill pass"); 0x140 is counted ack-only |
| 0x142, 0x143 (322, 323) invalidate compressed texture (+ slice/level) | **no-op (new)** | compressed textures are not mirrored; the reference also treats it as a no-op |

## Compute segment (type 1): the whole segment is not executed (counted `seg_unsupported`)

| tag (hex) | command | status |
|---|---|---|
| 0xc8, 0xca (200, 202) dispatchThreadgroups / dispatchThreads (56 total), 0xc9 (201), 0xe6 (230) indirect forms | missing-known | needs compute pipelines (`MTLComputePipelineState` objects), no executor |
| 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd9, 0xda | set buffer / sampler / sampler LOD / texture / buffer offset / buffer stride variants | missing-known |
| 0xd0 setComputePipelineState, 0xd1, 0xd2 stage-in region, 0xd3 threadgroup memory length, 0xd8 imageblock size | missing-known |
| 0xd4, 0xd5 fences, 0xd6, 0xd7 barriers, 0xe3 compressed texture flush | missing-known (would be no-ops once compute runs) |
| 0xdb writeDescriptor, 0xdc..0xe2 control flow markers, 0xe4, 0xe5 executeCommands | missing-known |

## Objects the reference decodes that this path treats as unsupported

Heaps / heap textures (0x0d heap objects, `heap_texture.rs`), rate maps (0x32), ICB (0x36), texture views (7, 8, 0x1b), depth/stencil objects (4,
`depth_stencil.rs`), fences (13), info queries (0x1c2..0x1d5). Serialized object payload tag 3 (sampler) is decoded by `arm64_sampler.c` (below).

## Device evidence (run 20261126-145344, `g7-gpu-run`)

`tags(done/partial/unsupported)[1=172/19/0 7=0/372/0 26=112/0/0 110=499/0/0 111=56/0/0 112=274/0/0 114=534/0/0 116=541/26/0 117=423/0/0 125=865/0/0
126=270/0/0 130=394/0/0 157=0/0/4 161=0/0/2 162=0/0/2]`, blit `300 x5`. The only unsupported tags in these logs are the tile ones (157, 161, 162 =
0x9d, 0xa1, 0xa2). 0x68 / 0x77 do **not** appear in `/tmp/zx` (they are in the reference list and the other agent's scope). Most draws were skipped
for `no_pipeline` (372 + 19 partial), i.e. pipeline build failures (`build_failed=339`), not for missing draw opcodes. `desc unaligned=19`: draws
skipped for misaligned storage-buffer offsets (fixed below).

Of the six serialized samplers in the dump (all 36 bytes), the state words are `0x84000000` (x3), `0x84000005`, `0x04000000`, `0x04000005`: this confirms the
bit layout of `arm64_sampler.h` (bit 31 normalized coordinates, bits 0 / 2 filters) and shows that **unnormalized-coordinate samplers occur on the real
device** (2 of 6), which the previous decoder ignored.

## Descriptor / sampler / buffer alignment (task 3)

Serialized sampler record (object tag 3). Our own evidence: six runtime samplers in the dump, all 36 bytes, with state words `0x84000000` (x3), `0x84000005`,
`0x04000000`, `0x04000005` (see Device evidence). What these words show, and nothing more: bit 31 toggles, bits 0 and 2 occur together, bit 26 is set in all
six, every other bit is 0. The layout below is therefore **INFER**; the decoder in `arm64_sampler.c` implements it:

* header: u32 tag = 3, u32 size, u32 ref (our buffer `d` includes this 8-byte header, `d + 12` is the state word); lod min / lod max as f32 at `d + 20` /
  `d + 24`. The decoder reads the first 28 bytes. The 36-byte size is measured (our dump); which of the 36 bytes the guest actually writes, the flags byte
  and the unused tail are **unverified**.
* state bits (positions **unverified** against varied captures; they equal the sampler decoder in Henge's `devices/pvg_metal.m`, which uses 4-bit address /
  border fields): [0] min filter, [2] mag filter (which of the two is which is unproven, the captured words never set them apart), [4] lodAverage,
  [7:5] compare function, [10:8] / [14:12] / [18:16] S / T / R address mode, [21:20] border colour, [25:24] mip filter, [30:26] anisotropy (1..16
  verbatim), [31] normalized coordinates (consistent with the captured words, not proven by them).
* widths: 3-bit address modes and 2-bit border colours are forced by the public Metal enum ranges (six address modes, three border colours).

Gaps found against the previous `make_sampler`, all fixed in `arm64_sampler.c`:

* the default `lodMax = FLT_MAX` was `> 32` and reset **lodMin to 0** too (a sampler with `lodMinClamp > 0` and the default max lost its minimum); now only the
  max is limited (`VK_LOD_CLAMP_NONE`), NaN / inverted ranges fall back to the defaults;
* address mode and border fields were read 4 bits wide (3 / 2 per the reference); the border colour was applied even for "clamp to zero", which is always
  transparent black; it now applies only when an axis clamps to the border colour;
* anisotropy (state 30:26) was dropped; now applied when the device enables `samplerAnisotropy` (the feature is now enabled in `vk_init` when supported) and
  limited to `maxSamplerAnisotropy`;
* unnormalized coordinates were dropped (real samplers use them); now applied with Vulkan's restrictions (equal filters, level 0, clamp addressing on S/T, no
  anisotropy). Caveat: Vulkan formally permits such a sampler only with explicit-LOD instructions while the translator emits `OpImageSampleImplicitLod`;
* still **not mapped, counted** in `g7-gpu-ext sampler[...]`: compare function (no Dref sampling in the translator; a compare sampler used by a plain sample is
  invalid), lodAverage (no Vulkan equivalent), mirror-clamp-to-edge (mirror repeat used; needs `VK_KHR_sampler_mirror_clamp_to_edge`).

Buffer offsets: Metal accepts any 4-aligned buffer offset; Vulkan storage-buffer descriptors need a multiple of `minStorageBufferOffsetAlignment`. The reference
solves this by "gathering" the bytes of misaligned guest binds into an aligned staging allocation. `bind_descriptors` used to skip the draw
(`desc_unaligned`, 19 draws on the device); it now binds an aligned scratch copy of `[off, off + range)` of the mirror buffer (`ext_gathered`; at most 8 MiB per
range, 8 copies per exec, freed with the exec). Limits of that copy: it is a snapshot of the mirror at record time (earlier GPU writes of the same exec to the
buffer, and shader writes, are not seen / not written back). The descriptor range is also clamped to `maxStorageBufferRange` (`ext_range_clamped`).
Vertex and index buffer offsets need no such handling (`vkCmdBindVertexBuffers` offsets are unrestricted; index offsets must be a multiple of the index size,
which is checked).

## Counters

`g7-gpu-ext:` (stderr / log, same cadence as `g7-gpu-run`): `draw[instanced base wide indirect indirect_bad zero]`, `noop[hint fence barrier_partial]`,
`sampler[compare_ignored unnorm_applied mirror_clamp_approx lod_binds lod_dropped]`, `buf[gathered gather_failed range_clamped]`.
