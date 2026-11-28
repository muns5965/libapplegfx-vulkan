/*
 * libapplegfx-vulkan — Protocol state machine definition
 * src/protocol/state.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Core protocol struct and shared types used by all dispatchers/handlers.
 */

#ifndef LAGFX_PROTOCOL_STATE_H
#define LAGFX_PROTOCOL_STATE_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Resource registry must be defined before protocol struct uses it.
 * Included here to avoid circular deps when state.h is included by handlers. */
#include "resource_registry.h"
#include "../vulkan/msaa_stats.h"


/* === Constants =================================================== */
#define LAGFX_MAX_TASKS        64u     /* Max concurrent tasks (slots in tasks[]) */
#define LAGFX_MAX_TASK_ID      512u    /* Valid task ids are < this (Apple Henge's task table is 512 wide); a task is found by id, never by slot index. */

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif
#define LAGFX_MAX_FIFOS        32u     /* Max child FIFOs */
/* Per-dispatcher scratch buffer size — large enough to hold any single
 * ring command (header + payload). Matches the legacy
 * LAGFX_*_MAX_CMD_BYTES values that lived in each dispatcher and the
 * exec_cmdbuf static \`buf[4096]\`. */
#define LAGFX_MAX_RING_READ    4096u
/* LAGFX_MAX_DISPLAYS (defined in device.h) is 4 in the live build —
 * the framebuffer + display-pipe / paravirt-event matched-class
 * counts published by the kext top out around 4 attached displays
 * before the test image hits its `ioreg` cap. Channels here cover
 * root (0) + compute (1-4) + display vchans (5..15); per
 * state-machines/FIFORingDescriptor.md the kext can provision up
 * to 8 displays (5..12) with hot-plug headroom. 16 channels keeps
 * the per-channel array dense without burning more than ~256 KiB
 * of scratch. */
#define LAGFX_A64_TEXWHY_COUNT 20     /* lagfx_a64_texwhy_t entries (arm64_resources.h); a static assert there keeps the two in step */
#define LAGFX_MAX_CHANNELS     16     /* Root + 4 compute + up to 11 display vchans */

/* Stamp slot mapping (per waitForStamp-mechanism-summary.md) */
enum {
    SLOT_ROOT_CHANNEL       = 0,   // ch 0 → root channel
    SLOT_COMPUTE_1          = 1,   // ch 1 → compute vchan
    SLOT_COMPUTE_2          = 2,   // ch 2 → compute vchan
    SLOT_COMPUTE_3          = 3,   // (unused by macOS)
    SLOT_COMPUTE_4          = 4,   // ch 4 → compute vchan
    SLOT_DISPLAY_PIPE_0     = 5,   // display pipe 0
};

/* Register shadow indices (BAR0+0x1000..0x1FFF → idx 0..15) */
enum {
    REG_STATUS_CONTROL      = 0,   // BAR0+0x1000 - ring_armed
    REG_RING_SIZE           = 1,   // BAR0+0x1004 - command ring size
    REG_WRITE_PTR           = 2,   // BAR0+0x1008 - doorbell (primary)
    REG_FIFO_FAULT_OFFSET   = 3,   // BAR0+0x100c - fault offset (read-only)
    REG_START_OFFSET        = 4,   // BAR0+0x1010 - ring start offset
    REG_SHARED_PAGE_PFN     = 5,   // BAR0+0x101c - shared page PFN
    REG_STAMP_BITMASK       = 7,   // BAR0+0x1020 - stamp bitmask (read)
    REG_BASE_PFN            = 8,   // BAR0+0x1030 - ring base PFN
};

/* === Wire Format Structures ====================================== */
/* lagfx_cmd_header_t defined in opcodes.h with derived fields.
 * The wire-format-only version is kept here for documentation. */

// 12-byte command header on wire (per-command-buffer-format.md §2.1)
typedef struct {
    uint16_t opcode;        // Outer opcode (0x00..0x44)
    uint32_t length;        // Total command size including this header
    uint32_t stamp;         // Stamp to ack on completion
    uint8_t  arg_count_8b;  // Number of 64-bit arguments after payload
    uint16_t payload_size;  // Payload bytes after 12-byte header
} __attribute__((packed)) lagfx_cmd_header_wire_t;

// Inner opcode (inside CmdExecIndirect2, 8-byte header)
typedef struct {
    uint32_t length;        // Total inner command size
    uint32_t stamp;         // Stamp to ack
    uint16_t opcode;        // Inner opcode (Render/Blit/Compute domain-specific)
} __attribute__((packed)) lagfx_inner_cmd_header_t;

/* === Render pass description (per-task) ==========================
 *
 * Stores the parsed PGCmdDescribeRenderPass (0x1a, 584 B) payload.
 * Populated when op_render_describe_render_pass fires; consumed by
 * Stage 70c to construct VkRenderingInfo at vkCmdBeginRendering.
 */
typedef struct {
    bool       valid;           /* True if RenderDescribeRenderPass parsed */
    /* VkFormat values stored as raw u32 so the struct is visible
     * across translation units that don't pull in <vulkan/vulkan.h>.
     * Cast to VkFormat at use site (under LAGFX_HAVE_VULKAN). */
    uint32_t   color_format;    /* VkFormat: first attachment format */
    uint32_t   depth_format;    /* VkFormat: depth/stencil, VK_FORMAT_UNDEFINED if none */
    float      clear_color[4];  /* RGBA clear values (0..1) */
    float      clear_depth;     /* Depth clear value (0..1) */
    uint32_t   render_area_x;   /* Render area origin x */
    uint32_t   render_area_y;   /* Render area origin y */
    uint32_t   render_area_w;   /* Render area extent width */
    uint32_t   render_area_h;   /* Render area extent height */
    uint32_t   view_count;      /* Number of color attachments (usually 1) */
    /* M2 per-pass RT: the IOSurface/object ref this pass renders INTO (the color
     * attachment target), parsed from the 0x1a attachment descriptor. 0 = the
     * default scanout (display->rt). A non-zero target means the pass renders an
     * intermediate layer (e.g. the wallpaper) into that surface, which a later
     * composite samples — so we route the pass's draws into that surface's
     * VkImage instead of the single scanout. */
    uint32_t   target_ref;
} lagfx_render_pass_desc_t;

/* === Pending draw description (per-task) =========================
 *
 * Stores the parsed Draw family opcode payload for per-task state.
 * Populated by op_draw_* handlers when they fire; consumed later at
 * vkCmdDraw/vkCmdDrawIndexed time to bind primitive type, counts, etc.
 *
 * MTLPrimitiveType → VkPrimitiveTopology mapping (for Stage 70f):
 *   MTLPrimitiveType=0 (Point)        → VK_PRIMITIVE_TOPOLOGY_POINT_LIST
 *   MTLPrimitiveType=1 (Line)         → VK_PRIMITIVE_TOPOLOGY_LINE_LIST
 *   MTLPrimitiveType=2 (LineStrip)    → VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
 *   MTLPrimitiveType=3 (Triangle)     → VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
 *   MTLPrimitiveType=4 (TriangleStrip)→ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
 * Cites: Apple Metal documentation; values from MTLPrimitiveType enum.
 *
 * Wire format variants (from render-decoder-handlers.md):
 *   - DrawPrimitives16 (0x01, line 55): (vertex_start, vertex_count) — u32×2 = 8 B
 *     Encoder: drawPrimitives:vertexStart:vertexCount:
 *   - DrawInstancedPrimitives16 (0x03, line 57): (field0, field1) — u32×2 = 8 B
 *     Encoder: drawPrimitives:vertexStart:vertexCount:instanceCount:
 *     Note: Metal selector has 4 args but payload only fits 2 u32; wire format
 *     may pack differently than documented C++ struct field names.
 */
typedef struct {
    bool       valid;           /* True if a Draw opcode was parsed for this task */
    uint32_t   primitive_type;  /* MTLPrimitiveType (0=Point,1=Line,2=LineStrip,3=Triangle,4=TriangleStrip) */
    uint32_t   index_count;     /* or vertex_count for unindexed draws */
    uint32_t   instance_count;  /* Instance count (default 1 if not specified) */
    int32_t    base_vertex;     /* Vertex offset (signed, default 0) */
    uint32_t   first_instance;  /* First instance index (default 0) */
    uint32_t   index_buffer_ref;/* Index buffer reference (0 if unindexed draw) */
    /* Byte offset of the index data inside the index buffer. Decoded from the
     * 0x07 wire per Apple's decodeDrawIndexedPrimitives16WithIterator:
     * {prim:u16, indexType:u16, bufRef:u32, indexCount:u16, offset:u16}. */
    uint32_t   index_buffer_offset;
    uint32_t   index_type;      /* MTLIndexType: 0 = UInt16, 1 = UInt32 */
    bool       indexed;         /* 1 = use index_buffer_ref, 0 = vertex-only draw */
    /* GOAL-M2x: set at vertex upload when attr1 texcoords are PIXEL-space
     * (max |uv| > 2) — the CA glyph pipelines sample with Metal coord::pixel
     * samplers; the descriptor bind then uses the unnormalized VkSampler. */
    bool       pixel_texcoords;
} lagfx_pending_draw_t;

/* === Per-task binding slots (Stage 70d) ==========================
 *
 * Apple's binding model: "set N at index I" where I is a per-stage slot (vertex/fragment 0..31).
 * Vulkan needs descriptor sets — Stage 70d just CAPTURES the bindings, not descriptor-set wiring.
 *
 * Binding opcodes (from render-decoder-handlers.md):
 *   - SetVertexBufferOffset (0x7e, line 133): PGCmdSetBufferOffset (12 B) → updates offset only
 *     Wire: [offset:u64][padding:u32][index:u32] — payload at offsets 0,8,12 per spec
 *   - SetVertexBuffers (0x7d, line 132): PGCmdSetBuffers + N×PGCmdSetBufferEntry → updates ref+offset
 *     Wire head: [count:u32][firstIndex:u32]; Entry: [ref:u32][offset:u64] = 12 B each
 *   - SetFragmentBufferOffset (0x6f, line 108): PGCmdSetBufferOffset (12 B) → updates offset only
 *     Wire: same as 0x7e but for fragment stage
 *   - SetFragmentBuffers (0x6e, line 107): PGCmdSetBuffers + N×PGCmdSetBufferEntry → updates ref+offset
 *     Wire head: [count:u32][firstIndex:u32]; Entry: same as vertex buffers
 *   - SetFragmentTextures (0x72, line 111): PGCmdSetTextures + N×u32 ref → updates texture slot only
 *     Wire head: [count:u32][firstIndex:u32]; Entry: [ref:u32] = 4 B each
 */
#define LAGFX_MAX_BINDING_SLOTS 32

typedef struct {
    uint32_t ref;           /* Resource registry reference (0 = unbound) */
    uint64_t offset;        /* SetXBufferOffset payload — byte offset into the buffer */
    bool     valid;         /* True if slot is bound (ref != 0) */
} lagfx_binding_slot_t;

typedef struct {
    lagfx_binding_slot_t vertex_buffers[LAGFX_MAX_BINDING_SLOTS];
    lagfx_binding_slot_t fragment_buffers[LAGFX_MAX_BINDING_SLOTS];
    lagfx_binding_slot_t vertex_textures[LAGFX_MAX_BINDING_SLOTS];
    lagfx_binding_slot_t fragment_textures[LAGFX_MAX_BINDING_SLOTS];
} lagfx_bindings_t;

/* Stage 65d Option 3: shader modules selected for this task.
 * Currently always copied from device's bundled triangle SPVs
 * — pending proper metallib capture. VkShaderModule handles are
 * stored as uintptr_t so this struct stays visible across translation
 * units that don't pull in <vulkan/vulkan.h>. Cast at use site. */
typedef struct {
    bool       valid;
    uintptr_t  vertex_shader;    /* VkShaderModule, NULL if not set */
    uintptr_t  fragment_shader;  /* VkShaderModule, NULL if not set */
    uint32_t   reference;        /* SetRenderPipelineState ref value (debug) */
    /* Phase 6a: when this pipeline came from translated AIR (vs the
     * substitute triangle SPVs), the entry-point names differ. Our
     * translator emits "main"; the substitute SPVs use
     * "triangle_vertex" / "triangle_fragment". `translated == true`
     * routes the draw site to the right pName, and signals that the
     * shader_module handles need vkDestroyShaderModule on overwrite
     * (substitute modules live on the device and must NOT be freed). */
    bool       translated;
    /* Phase 6a descriptor layout: when the translated shaders declare
     * [[buffer]]/texture descriptors (DescriptorSet 0), these hold the
     * VkDescriptorSetLayout + VkPipelineLayout built from SPIR-V reflection
     * (lagfx_build_pipeline_layout_from_spv). Both NULL for a resource-free
     * pipeline (use the device empty layout) or the substitute path. Stored
     * as uintptr_t to keep this struct free of <vulkan/vulkan.h>. */
    uintptr_t  descriptor_set_layout; /* VkDescriptorSetLayout, NULL if none */
    uintptr_t  pipeline_layout;       /* VkPipelineLayout, NULL if none */
    /* Stage 85b: reflected set-0 bindings so the draw site can populate a
     * matching descriptor set from the guest's bound resources. kind is
     * lagfx_spv_binding_kind_t (1=sampled image, 2=sampler, 3=storage buffer).
     * n_spv_bindings==0 ⇒ resource-free. */
    /* 32 (was 16): the Xgc login-panel fragment reflects ~20+ set-0
     * bindings; the old cap silently truncated the list, leaving the
     * tail descriptors unwritten (undef samples). Matches the
     * translator's LAGFX_MAX_VERTEX_ARGS raise to 32. */
    uint8_t    spv_binding_no[32];
    uint8_t    spv_binding_kind[32];
    uint64_t   spv_cxs_raw[32];       /* sampler bindings that are a constexpr sampler of the shader: the raw AIR sampler word (0 = none); the draw binds the VkSampler decoded from it */
    /* Metal resource index per reflected binding (from the AIR arg metadata's
     * air.location_index — lagfx_air_arg_bindings). THE authoritative
     * binding→guest-slot map: e.g. UberCompositeVertex's mvp_matrix is
     * [[buffer(1)]], VfxVertex's is [[buffer(2)]] — no fixed convention, so
     * content/skip heuristics can't substitute. -1 = unknown → legacy
     * heuristics apply. */
    int16_t    spv_binding_metal[32];
    uint8_t    n_spv_bindings;
    /* Vertex stage-in attributes reflected from the translated VERTEX shader
     * (Location-decorated Inputs). The host builds a non-empty vertex-input
     * state + binds the guest's vertex buffer from these; without them the
     * vertex shader reads unbound attributes → 0 positions → degenerate draws.
     * vtx_in_comp[i] = component count (1..4) for Location i; n_vtx_inputs = count. */
    uint8_t    vtx_in_loc[8];
    uint8_t    vtx_in_comp[8];
    uint8_t    n_vtx_inputs;
    /* Per-pipeline vertex-buffer-layout stride, decoded from the serialized
     * MTLVertexDescriptor in the PSO blob (the `04 <stride:u32> 02` layout token).
     * 0 = unknown → callers fall back to the round8(sum-of-attr-sizes) heuristic.
     * The round8 heuristic is WRONG for the CoreAnimation composites (real stride
     * 48, tight sum rounds to 24 → every other vertex misread → horizontal smear).
     * Ground truth: pso 0x2c/0x23 = 48, pso 0x14 = 24. */
    uint32_t   vtx_stride;
    /* GOAL-M2z: real per-attribute {MTLVertexFormat, offset} decoded from the
     * PSO's MTLVertexDescriptor (sorted by offset). 0 attrs = not decoded. */
    uint32_t   vtx_attr_fmt[8];
    uint32_t   vtx_attr_off[8];
    uint8_t    n_vtx_attrs;
    /* B1 (M0): cached VkPipeline built from this pending_pipeline + the RT
     * formats it was built for. Reused across draws (was rebuilt + LEAKED every
     * draw → OOM on long runs). Destroyed when the shaders change (op_0x74
     * overwrite) or the task is torn down. */
    uintptr_t  cached_pipeline;       /* VkPipeline, NULL if none */
    uint32_t   cached_color_fmt;      /* render_pass_desc formats the cache was built for */
    uint32_t   cached_depth_fmt;
} lagfx_pending_pipeline_t;

/* === Task Entry ================================================== */
typedef struct {
    uint32_t id;                /* Task ID (from CmdDefineTask2 / CmdDefineHostTask) */
    uint64_t root_page_pfn;     // Root page PFN for VA→GPA translation
                                // (set by CmdDefineHostTask 0x38)
    uint64_t base_va;           // Guest VA or shell-returned ptr low bits
    uint32_t length;            // Task address space size in bytes
    void *shell_task;           // Opaque handle passed to shell (if any)
    /* Resource heap (set by CmdSetResourceHeap 0x33). Per kext disasm
     * the kext provides a (heap_pfn, heap_size) tuple per task; we
     * record it as a hint for later resource lookups. */
    uint32_t heap_pfn;
    uint32_t heap_size;
    /* CmdSetObjectAndPlacement-list registration (compute 0x44, per
     * devices/pvg_lite.c): guest VA of the object list / placement list and
     * their lengths. Kept so later resource lookup can use them. */
    uint64_t objects_addr;
    uint32_t objects_len;
    uint64_t placement_addr;
    uint32_t placement_len;
    bool lists_set;
    bool live;                  /* Slot is in use */
    
    /* Render pass description from PGCmdDescribeRenderPass (0x1a).
     * Per-task state: each task has its own render pass descriptor. */
    lagfx_render_pass_desc_t render_pass_desc;

    /* Scissor rect (0x75) — the authoritative draw-region size. 0x1a
     * render_area is genuinely 0 for real passes (Metal leaves
     * renderTargetWidth unset), so the scissor is the correct source for
     * per-pass surface dimensions (e.g. 1280x1024 wallpaper pass). */
    uint32_t scissor_w;
    uint32_t scissor_h;

    /* KICKOFF-shading-throughput: the CURRENT guest scissor rect (full
     * MTLScissorRect from 0x75, unguarded — scissor_w/h above keeps its
     * per-pass-surface-size role with its 16px floor). Forwarded to
     * vkCmdSetScissor per draw; sc_have reset at 0x1a render-pass begin
     * (Metal resets encoder scissor/viewport to full-RT defaults). The
     * missing scissor let full-target wash triangles shade the ENTIRE
     * 1920x1080/3840x2160 RT instead of the guest's tiny region (160x29
     * observed) — the "20-30s 3-vertex draw" throughput collapse. */
    uint32_t sc_x, sc_y, sc_w, sc_h;
    uint8_t  sc_have;

    /* Viewport rect (0x82 SetViewport, MTLViewport = 6× f64). originX/Y +
     * width/height are the layer's DESTINATION rect in the render target /
     * scanout — the guest-declared placement for compositing each per-pass
     * layer (clock/UI land at their real offset, not a centered stretch).
     * valid=0 until the guest sets one. */
    uint32_t vp_x, vp_y, vp_w, vp_h;
    uint8_t  vp_valid;
    /* Set by 0x82 and cleared at 0x1a render-pass begin: the viewport is
     * CURRENT for this pass and must be applied to the draw's
     * vkCmdSetViewport (the guest's pixel->NDC vertex matrices target the
     * viewport rect, e.g. 2/160 x -2/64 for a 160x64 layer — mapping that
     * NDC onto the full RT stretched small layers across the whole
     * 1920x1080 target). vp_valid stays sticky for ASMBLIT placement. */
    uint8_t  vp_current;

    /* KICKOFF-scanout-composite-reliability: screen-space bounding box of the
     * draw's DECODED vertex positions (float x@0,y@4 per vertex). The guest
     * puts each UI element's on-screen placement in the VERTICES (the scissor
     * origin is (0,0)); this bbox is the element's real dst rect on the
     * scanout. Used by the poor-man's tile compositor to place a sampled
     * content tile (clock/username text) at its true position. Set in
     * lagfx_upload_guest_vertex_buffer; valid=0 when no plausible positions. */
    uint32_t vtx_bbox_x, vtx_bbox_y, vtx_bbox_w, vtx_bbox_h;
    uint8_t  vtx_bbox_valid;

    /* Pending draw description — populated by Draw opcode handlers (0x01, 0x03, 0x06, 0x07).
     * Per-task state: each task has its own pending draw descriptor. */
    lagfx_pending_draw_t pending_draw;

    /* Descriptor-set bindings from SetVertexBuffer/FragmentBuffer/Texture opcodes (Stage 70d).
     * Stores per-stage binding slots indexed by slot number (0..31). Populated by binding handlers:
     *   - 0x7e SetVertexBufferOffset, 0x7d SetVertexBuffers → vertex_buffers[]
     *   - 0x6f SetFragmentBufferOffset, 0x6e SetFragmentBuffers → fragment_buffers[]
     *   - 0x81 SetVertexTextures, 0x72 SetFragmentTextures → texture arrays (TODO: add handlers) */
    lagfx_bindings_t bindings;

#ifdef LAGFX_HAVE_VULKAN
    /* Stage 65d Option 3: shader modules selected for this task. */
    lagfx_pending_pipeline_t pending_pipeline;
#endif

    /* Phase 6b — active-object registry populated by 0x25
     * CmdSetObjectAndPlacementList. Per the live wire data captured
     * 2026-05-28 (see paravirt-re/library/apv-object-entry-parser-
     * scoping-2026-05-17.md), 0x25's payload is 8 bytes:
     * `{count: u32, objectId: u32}`. The kext publishes which
     * objectIds are in the active set; op_0x74 SetRenderPipelineState
     * later resolves these IDs to pipeline-state metallib bytes via
     * the existing Phase B heap-VA resolver.
     *
     * Small bounded array — Apple boot path publishes ~10s of unique
     * objectIds total per task in observed traffic. */
    struct {
        uint32_t object_ids[64];   /* dedup'd set of active objectIds */
        uint32_t count;
    } active_objects;

    /* KICKOFF-shading-throughput (LAGFX_DUMP_SLOW_BINDS): RAM snapshot of
     * the most recent translated draw's storage-buffer bind windows,
     * captured during descriptor-set build and flushed to /tmp by
     * draw_emit only when that draw's fence wait exceeded the threshold —
     * so the runaway draws' exact inputs are preserved without last-
     * draw-wins races. Arena is 32 × 64 KiB, lazily allocated only when
     * the env gate is set. */
    uint8_t  *slowsnap;             /* lazily allocated window arena */
    uint8_t   slowsnap_bindno[32];  /* SPIR-V binding number per window */
    uint32_t  slowsnap_n;           /* windows captured for the last draw */
} lagfx_task_entry_t;

/* === FIFO Entry ================================================== */
typedef struct {
    uint32_t id;                /* Child FIFO ID (from CmdDefineChildFIFO) */
    uint64_t ring_base_gpa;     // Ring base GPA for this child FIFO
    uint32_t ring_size;         // Ring size in bytes
    uint32_t entry_count;       // Number of entries in ring
    bool live;                  // Whether this FIFO is active
} lagfx_fifo_entry_t;

/* === Display Child Ring ========================================== */
typedef struct {
    uint64_t ring_base_gpa;     // Ring base GPA (separate from parent ring)
    uint32_t ring_size;         // Ring size
    uint32_t entry_count;       // Entry count
    bool live;                  // Active flag
} lagfx_display_child_ring_t;

/* === Handler Status ============================================== */
/* Defined in opcodes.h to avoid duplication when including state.h. */

/* === Protocol State Structure ==================================== */
typedef struct lagfx_protocol {
    uint32_t magic;             // LAGFX_PROTOCOL_MAGIC for validation

    /* Device reference */
    void *dev;                  // Back-reference to device (opaque here)

    /* Register shadow (BAR0+0x1000..0x1FFF → idx 0..15) */
    uint32_t reg[16];           // Shadowed register values

    /* Ring geometry (set by MMIO, preserved across resets) */
    uint64_t ring_base_pfn;     // Command ring base PFN (from BAR0+0x1030)
    uint64_t ring_base_gpa;     // Computed: (pfn << 12) + start_offset
    uint32_t ring_size;         // Ring size in bytes (BAR0+0x1004)
    uint32_t ring_start_offset; // Offset within base page (BAR0+0x1010)
    uint64_t ring_shared_page_pfn;  // Shared page PFN for descriptors
    uint32_t page_size;         // Page size (observed: 0x1000)
    uint8_t  pfn_diag_done;     // the one-time ring address comparison has been logged
    uint32_t protocol_mode;           // LAGFX_PROTO_X86 (0) or LAGFX_PROTO_ARM64_VMAPPLE (1), from the device descriptor.
    uint32_t ring_page_shift;         // PFN to address shift. 0 = default 12 (4 KiB). 14 = 16 KiB pages (from the device descriptor).

    /* Ring pointers (updated by doorbells) */
    uint32_t read_ptr;          // Last processed write pointer (root channel only)
    uint32_t write_ptr;         // Current write pointer from doorbell (BAR0+0x1008)

    /* Per-channel tracking */
    uint8_t current_chan_id;    // Set by channel_door_dispatcher on entry

    /* Ring armed flag (set by STATUS_CONTROL @ 0x1000) */
    bool ring_armed;            // Whether ring is enabled for processing

    /* === Stamp management ==========================================
     *
     * \`pending_stamps_bitmask\` and \`pending_displays_bitmask\` are
     * touched from two threads in the live deployment:
     *
     *   - The QEMU MMIO read thread, when macOS reads BAR0+0x1014
     *     (display) or BAR0+0x1018 (stamp). doorbell_handle_read
     *     does an xchg-and-clear: capture the current mask, return
     *     it to the guest, then zero it out so the next read sees
     *     only freshly-pending interrupts.
     *
     *   - The drain thread invoked from the doorbell dispatch path
     *     (channel_*_dispatcher), which does a bit-set via
     *     \`|=\` whenever it completes a command on a given slot
     *     before raising the IRQ.
     *
     * QEMU serialises both paths through the BQL today, so the data
     * race is latent — but the field shape was uint32_t, the
     * compiler is free to tear reads, and a future BQL-relaxed lane
     * would expose it. Switch to _Atomic uint32_t and update every
     * touch to atomic_* with the documented memory orderings:
     *
     *   bit-set on completion → memory_order_release (synchronises
     *     stamp-cell DMA write + IRQ raise with the reader)
     *   xchg-and-clear on read → memory_order_acquire (sees the
     *     drainer's stamp-cell DMA before returning the mask)
     *
     * last_completed_stamp stays non-atomic — it's pure diagnostics
     * (no consumer reads it cross-thread). */
    _Atomic uint32_t pending_stamps_bitmask;  // Bits set for slots needing IRQ
    uint32_t last_completed_stamp;             // Most recent stamp value acked (diag only)
    _Atomic uint32_t pending_displays_bitmask; // Display interrupt bitmask
    /* Display doorbell counters (diagnostics only, relaxed atomics: touched by vCPU threads and the shell's signal thread, never reset by device reset).
     * db_reads/db_nonzero/db_bit[]: MMIO reads of 0x1014 and the bits of the returned mask; sig_total/sig_bit[]: lagfx_device_signal_display() calls
     * and the cause bits they raised. */
    _Atomic uint64_t db_reads, db_nonzero, db_bit[4], sig_total, sig_bit[4];

    /* Task table (per CmdDefineTask2) */
    lagfx_task_entry_t tasks[LAGFX_MAX_TASKS];

    /* FIFO table (per CmdDefineChildFIFO) */
    lagfx_fifo_entry_t fifos[LAGFX_MAX_FIFOS];

    /* Display child rings */
    lagfx_display_child_ring_t display_child_rings[16];  // Up to 16 displays

    /* Resource registry (per CmdMapMemory2, CmdDeleteResource, etc.) */
    lagfx_resource_registry_t resources;


    /* M2c: 0x3b per-ref backing/residency updates (wire: {u32 seq, u32 ref,
     * u32 5, u32 6, u64 addr}). Latest addr per ref; consulted (gated
     * LAGFX_M2_BACKUPD) by the vertex-upload resolve in preference to the
     * (stale for some refs) placement-table PFN walk. Per-protocol state
     * (standing rule: no file-statics in handlers). */
    struct {
        uint32_t ref;
        uint64_t addr;
        uint8_t  valid;
    } backing_update[64];
    uint32_t backing_update_n;

    /* M2c WHITEFB: lazily-created opaque-white fallback texture
     * (lagfx_vk_iosurface_t*, stored as uintptr_t — vulkan.h-free header).
     * Bound for unresolved SAMPLED_IMAGEs under LAGFX_M2_WHITEFB so the
     * translated composite fragments' alpha-discard never fires. */
    uintptr_t white_fb_ios;

    /* Per-frame scanout-assembly blit queue. Draws into per-pass surfaces
     * enqueue their surface (draw order, dedup); the display present blits
     * the queue in order into display->rt and clears it, so a later
     * near-empty surface can never wipe an earlier content-bearing one.
     * Entries are lagfx_vk_iosurface_t* as uintptr_t (vulkan.h-free). */
    uintptr_t frame_blit_queue[16];
    uint32_t  frame_blit_n;

    /* KICKOFF-scanout-composite-reliability: poor-man's tile-compositor
     * overlay. A scanout draw that samples a finished content tile (clock,
     * username text) enqueues {tile IOSurface, screen dst rect from the
     * draw's vertex bbox}. The whole set is RE-BLITTED into display->rt at
     * the end of EVERY draw so a later full-screen draw can't wipe it (the
     * per-draw blit alone was overwritten before present). Reset at present.
     * ios stored as uintptr_t (vulkan.h-free), like frame_blit_queue. */
    struct { uintptr_t ios; uint32_t x, y, w, h; } tile_overlay[16];
    uint32_t  tile_overlay_n;

    /* 0x39 MapMemoryImmediate big regions (>=4 MiB). The wallpaper IOSurface's
     * pixels are written by the guest into one of these guest-physical regions,
     * NOT the placement descriptor its ref resolves to (which reads black). The
     * texture-realize path scans these for the photo-rich one. */
    struct { uint64_t gpa; uint64_t size; } big_maps[16];
    uint32_t big_maps_n;

    /* One-shot / capped diagnostic flags and counters (protocol-owned so
     * handlers stay free of file-static state). */
    struct {
        uint32_t unknown_blit_ops;
        uint32_t unknown_render_ops;
        uint8_t  first_draw_dumped;
        uint8_t  resource_buf_dumped;
        uint8_t  desc_table_dumped;
        uint32_t submit_probe_count;
        uint8_t  bench_done;
    } diag;

    /* Diagnostics only (no behaviour depends on it): global command sequence, per-source/opcode counters, last 0x44 task per channel, timing.
     * See src/protocol/diag.c. Times are CLOCK_MONOTONIC nanoseconds. */
    struct {
        uint64_t seq;                    /* every command seen by a drain loop, all channels */
        uint64_t t0_ns, last_stats_ns, last_cmd_ns, last_mmio_ns;
        uint64_t last_poll_ns;           /* last lagfx_arm64_poll_completions() entry (relaxed atomic; diagnostics only) */
        uint64_t mmio_reads, mmio_writes;
        uint64_t src_cmds[3];            /* 0 root, 1 compute, 2 display */
        uint64_t op_count[3][256];       /* opcode & 0xff */
        uint64_t exec_20, exec_43_45, unknown_cmds;
        uint64_t display_submits, display_submit_skipped_null, display_submit_ignored_index, display_submit_rendered;
        /* ARM64 display ops (compute dispatcher, ops 1/2/8), each stage counted separately; see lagfx_a64_res_display_note().
         * display_submit_rendered above belongs to the legacy/compact vchan_display_submit path only and stays 0 on ARM64. */
        struct {
            uint64_t op_setpage, op_ack, op_swap;      /* op 1 / 2 / 8 seen (names as in the Henge shell: set-page, ack, swap) */
            uint64_t swap_short;                       /* op 8 whose payload is too short to name a mapping: not looked up */
            uint64_t surface_resolved, surface_unresolved;   /* the swap's mapping id has / has no mirror surface (no exec ever drew into it: no GPU content is possible) */
            uint64_t gpu_content;                      /* resolved and the mirror holds pixels the GPU produced or the guest uploaded */
            uint64_t written_back, gpu_unwritten;      /* of those: the guest's pages are current when the command is handed over / the guest still holds older pixels */
            uint64_t image_probes, image_probe_unavail, image_probe_nonblack_last;   /* g7-gpu-image: sampled GPU-image readbacks at swaps 1,2,4,8..; unavailable (busy / unsupported); non-black pixel count of the last one */
            uint64_t handed_to_shell, not_handed;      /* display_command callback called / no callback or no payload */
        } disp;
        uint64_t irq_raised;
        uint64_t ignored_known;          /* known-and-ignored commands (arm64 0x0a/0x0c): not "unknown" */
        /* arm64 0x43/0x45 decode-only counters (src/protocol/arm64_exec_decode.c). Nothing is executed. */
        struct {
            uint64_t received, decoded, failed;                    /* decoded + failed == received */
            uint64_t bad_size, read_failed, bad_seg, no_task;      /* causes of `failed` (an exec can count in several) */
            uint64_t bufs, bytes_read, bytes_capped, seg_clamped, tail_bytes, truncated;
            uint64_t seg_t[6], seg_other, seg_cont, seg_more;      /* seg_t[5] counts protection headers skipped, not segments */
            uint64_t res, res_flag[3], res_flag_hi, obj_missing, obj_kind[32];
            uint64_t inner, inner_bad, tag_hist[4][512], tag_over; /* by segment type 0,1,2,4 then tag; tag >= 512 in tag_over */
            uint32_t tags_logged;
            /* ARM64 render execution (src/protocol/arm64_exec_run.c, LAGFX_ARM64_EXEC=1 only; all zero otherwise). */
            struct {
                uint64_t execs, passes, draws, draws_skipped, submits, submit_failed, submit_pending, pending_leaked;
                uint64_t busy_skipped, pending_unqueued, seg_blit, held;   /* execs refused while the previous one was in flight / PENDING without a completion queue / blit segments recorded */
                uint64_t seg_render, seg_unsupported;              /* render segments executed / segments of other types skipped */
                uint64_t no_vulkan, bad_rec, no_pass, no_target, no_target_pass, no_target_draw /* no_target = pass-stage error EVENTS (no_target_pass: a target / attachment / begin failure; several can occur in one pass, and a pass may still run on another attachment) + draws skipped because their pass was not rendering (no_target_draw). Neither is a count of failed passes */, no_pipeline, no_pipeline_why[4], no_vertex_buf, no_index_buf, prim_unsupported;
                uint64_t multi_attach, blend_indep_mismatch, extra_attach_ignored, load_uninit_cleared, pipeline_unresolved, obj_unsupported, obj_unsupported_linear;
                /* depth / stencil / raster state (arm64_depth_stencil.c, arm64_exec_run.c) */
                uint64_t dss_decoded, dss_bad, dss_bound, dss_unresolved;   /* tag-4 objects decoded / rejected; tag 104 applied / ref not a decodable object (state then stays as it was) */
                uint64_t ds_passes, ds_depth_only_passes, ds_attach_unsupported, ds_split_ignored, ds_no_device_support;   /* passes with a depth or stencil attachment; without any colour; one that could not be attached (format / texture / device); stencil in a texture other than the depth one (stencil dropped); device without core Vulkan 1.3 dynamic state */
                uint64_t blend_color_set, blend_const_draws, ds_draws, stencil_ref_set, cull_set, winding_set, bias_set, bias_clamp_dropped, viewport_depth_set, viewport_degenerate, viewport_empty_metal, viewport_inexpressible;   /* viewport_degenerate = empty_metal + inexpressible (the one diag.c prints); empty_metal: a viewport that draws nothing in Metal either (empty, or beyond Vulkan's limits and off the target); inexpressible: a drawable viewport Vulkan cannot express (S05); draws that ran with depth or stencil testing on; state records applied; a bias clamp dropped because the device has no depthBiasClamp */
                uint64_t mapped_unsup_site[12], seg_unsup_type[8], ext_storage_img_why[4], ext_storage_img_log_overflow;   /* ext_storage_img_why: 0 linear, 1 usage missing + format lacks STORAGE_IMAGE feature, 2 usage missing + format supports it, 3 view failed; log_overflow: new (cause, format) beyond the 16 logged */   /* mapped_unsupported by site / unsupported exec segments by type (1 compute, 4 information) */
                uint64_t mapped_created, mapped_shared, mapped_uploaded, mapped_written_back, mapped_unsupported, mapped_no_desc, mapped_bytes_up, mapped_bytes_down, mapped_pages_skipped, swap_events, swap_flushed, swap_deferred, swap_failed, swap_dropped, mapped_remapped, swap_partial;   /* type 11 mapped textures (guest display surfaces) */
                uint64_t wb_host_barriers_recorded, wb_down_pending_cleared_on_fail, wb_down_skipped_not_recorded, wb_finish_desc_unreadable_owed, wb_retry_owed, wb_retry_exhausted, wb_after_refused_pass;   /* mapped write-back sync (g7-wb-sync): HOST_READ barriers recorded after host-read copies; pending image->staging copies cleared because their exec's submit/fence failed; finishes skipped because this exec recorded no copy for the surface; unreadable descriptors at finish (surface set owed again); wb_after_refused_pass: surfaces copied down although a pass that targeted them in the same exec was refused (render_begin failed: its clear/draws never ran, so the guest gets stale pixels) */
                uint64_t tag_done[512], tag_partial[512], tag_unsup[512];   /* by inner tag; partial = recorded but not applied to Vulkan */
                uint64_t tag_over;                                   /* tags >= 512, any outcome */
                /* arm64_exec_run.c extensions: draw variants / sampler fidelity / misaligned buffer binds (printed as g7-gpu-ext) */
                uint64_t ext_draw_instanced, ext_draw_base, ext_draw_wide, ext_draw_indirect, ext_draw_indirect_bad, ext_draw_zero, ext_noop_hint, ext_noop_fence, ext_barrier;
                uint64_t ext_samp_compare, ext_samp_unnorm, ext_samp_mirror_clamp, ext_samp_lod_dropped, ext_samp_lod_binds;
                uint64_t ext_samp_addr_unknown, ext_samp_mip_unknown;   /* sampler object creations (same event as sampler_cache.h rt_sampler_objs_*, not draws or guest requests) with an address field 6/7 / mip field 3, outside the public Metal enum range: mapped to the compat fallback */
                uint64_t ext_gather_why[5]; /* ext_gather_failed by cause: 0 buffer without host map, 1 range > A64_GATHER_MAX, 2 gather copy count full (A64_GATHER_CACHE), 3 scratch allocation, 4 per-exec byte budget (A64_GATHER_BUDGET) */
                uint64_t ext_gather_gpu, ext_gather_gpu_refreshed, ext_gather_host_stale, ext_gather_gpu_lost;   /* copies made by the GPU (a write to the buffer was recorded earlier in the exec) / cached copies refreshed in place / pending write but no GPU copy possible (multisampled pass): host copy, may be stale / pass could not be resumed after the copy (draw skipped) */
                uint64_t ext_gather_bytes, ext_gather_reused; /* bytes copied by gathers / binds served from a covering copy at a nonzero offset */
                uint64_t ext_gathered, ext_gather_failed, ext_range_clamped, ext_storage_img_unsupported, ext_vs_tex_binds, ext_unnorm_lod0, ext_unnorm_skipped, ext_wsb_skipped;
                uint64_t ext_cxs_binds, ext_tex3d_binds, ext_tex_dim_mismatch;   /* constexpr-sampler descriptors bound from the decoded AIR word / Dim3D sampled-image descriptors bound / draws skipped because the texture's dimensionality is not the shader's (2D vs 3D) */
                /* Unnormalized-coordinate samplers, per USE (arm64_exec_run.c, src/air2spv/spv_unnorm.c; printed as g7-gpu-unnorm). Variant counters count shader-module builds (one per distinct draw situation of a pipeline), not draws. */
                uint64_t ext_tex_mips_base_only, ext_tex_layers_base_only;   /* sampled-texture descriptors per draw that STILL drop data: mips = the texture has >1 levels but the view is level 0 only (the whole-chain view could not be made, or the draw has an unnormalized-coordinates sampler in that stage: Vulkan wants one level / one layer for it); layers = a plain 2D (non-array) shader reads a texture with >1 array layers (layer 0 only; also the unnormalized-sampler case) */
                uint64_t ext_tex_mip_views, ext_tex_array_views;             /* sampled-texture descriptors per draw bound to a view over more than level 0 / to a 2D_ARRAY view (nothing dropped) */
                uint64_t ext_unnorm_unused;                       /* draws with an unnormalized sampler bound whose shader never samples through it: the shader is used unchanged, the draw is NOT skipped */
                uint64_t ext_unnorm_variants, ext_unnorm_variant_fail;   /* rewritten fragment / vertex modules built / module creation or allocation failed (draw skipped, counted in ext_unnorm_skipped) */
                uint64_t ext_unnorm_use_implicit0, ext_unnorm_use_bias_dropped, ext_unnorm_use_offset_folded, ext_unnorm_use_explicit_kept, ext_unnorm_use_grad_kept, ext_unnorm_use_gather_kept, ext_unnorm_use_unresolved;   /* per sample instruction, counted when a variant is built */
                uint64_t ext_unnorm_unsup[5];                     /* sample uses Vulkan forbids with an unnormalized sampler (draw skipped): 0 Dref, 1 Proj, 2 sparse, 3 image 3D / cube / arrayed / multisampled, 4 other (QueryLod, ConstOffsets, no SPIR-V to analyse) */
                uint64_t ext_cubic_requested, ext_cubic_used, ext_cubic_fallback[6];   /* LAGFX_CUBIC=1 only: sampler descriptors bound per draw whose guest filter field was 2 / of those given VK_FILTER_CUBIC_EXT / fallbacks to LINEAR by lagfx_cubic_why_t (index; 0 and NO_OPTIN stay 0) */
                uint64_t ext_samp_filter_unknown;                 /* sampler object creations whose min / mag filter field has its upper bit set (value 2/3: outside MTLSamplerMinMagFilter 0..1; INFER: could be a bicubic code): mapped to LINEAR, counted */
                uint64_t ext_tex_prepared;   /* textures a pass bound for sampling that the mirror did not hold yet (not in the resource list): created before the pass opened */
            } run;
            /* ARM64 pipeline resolution, binding and capture (src/protocol/arm64_pipeline.c, arm64_dump.c). */
            struct {
                uint64_t resolved, resolve_failed;
                uint64_t rf_no_blob, rf_bad_tag, rf_no_refs, rf_no_lib, rf_no_stage, rf_module, rf_translate, rf_layout, rf_multi_stream, rf_vertex_res, rf_bindings, rf_no_vulkan;
                uint64_t rf_root[6], rf_root_stage;   /* pipeline refused because the descriptor root (tag 1 vertex / tag 2 fragment) is malformed / missing / the same ref twice / names a non-function object (index = lagfx_a64_root_status_t), or names a function of the wrong stage */
                uint64_t xlate_air2spv;
                uint64_t discard_demote, discard_kill, discard_dropped;   /* air.discard_fragment lowered as OpDemoteToHelperInvocation / as OpKill / dropped inside a loop plan (S06) */
                uint64_t link_mismatch_fixed, link_missing_zeroed, link_unlinked;   /* fragment stage linker (air2spv): input read as the vertex type + converted / input with no vertex output zeroed / left unresolved */
                uint64_t rf_missing_cap, stub_missing_cap[32];   /* a translated module needs a device capability that is not enabled (vulkan/caps.h order): pipeline not built */
                uint64_t obj_created, obj_destroyed, obj_reused, obj_live, obj_peak_live, obj_high_water, obj_full, obj_lookup_miss, obj_deferred;   /* mirror object table (arm64_resources.c), cumulative per run: destroyed = logically released (0x25, task drop, replaced entry), deferred = of those parked behind an exec in flight, reused = created into a freed slot, high_water = slots ever used, full = refused at 4096 live, lookup_miss = texture-handle lookups of the draw path that found nothing */
                uint64_t desc_linear_stale;   /* a linear texture whose buffer changed inside the open pass: draw skipped (also counted in desc_unbound_why[4]) */
                uint64_t desc_tex_why[LAGFX_A64_TEXWHY_COUNT];   /* desc_unbound_why[3] (a sampled texture handle did not resolve) split by reason: lagfx_a64_texwhy_t order, names lagfx_a64_texwhy_name() */
                uint8_t  desc_tex_nseen[LAGFX_A64_TEXWHY_COUNT];   /* distinct unresolved textures logged per reason (first 8) */
                struct { uint32_t task, ref, type, mtl_format, w, h, mapping; } desc_tex_seen[LAGFX_A64_TEXWHY_COUNT][8];
                uint64_t desc_buf_why[7];   /* desc_unbound_why[0] by cause: 0 no Metal slot, 1 slot beyond table, 2 slot never bound in the pass, 3 ref did not resolve, 4 not a buffer, 6 offset past the end */
                uint8_t  desc_buf_nseen[7];
                struct { uint32_t ref, slot, stage; } desc_buf_seen[7][8];
                uint64_t desc_unbound_why[6];   /* desc_unbound by cause: 0 buffer missing/offset, 1 input attachment, 2 storage image, 3 sampled image missing, 4 sampled image view (linear/none), 5 sampler */
                uint64_t stub_fbfetch_why[5];   /* stub_fbfetch by reason (LAGFX_A64_FBF_*; [0] unused) */
                uint64_t stub_fbfetch, fbfetch_pipes, fbfetch_draws, blend_decoded, blend_off, color1_seen, stub_blend, stub_depth_stencil, stub_color_format_from_pass, stub_raster_state, stub_vertex_textures;
                uint64_t pipes_built, pipe_build_failed, pipe_replaced, pipe_fallback_built, topo[8];   /* pipe_fallback_built: built only by a value-preserving variant after the original failed */
                /* VkRenderPass backend, one counter per cause (indices = lagfx_legacy_refuse_t, array of 12 >= LAGFX_LR__COUNT):
                 *   legacy_shape_refused = pipelines refused (sum of legacy_pipe_why), counted once per (pipeline, formats, topology): failures are cached;
                 *   legacy_pipe_why[]    those pipelines by cause (NO_COLOR, DS_MIXED, NO_DS, PASS);
                 *   legacy_begin_why[]   passes refused by cause (ARGS, MSAA, RESOLVE, SLOT, DS, BEGIN), counted per pass attempt;
                 *   legacy_draws_dropped_pipe  DRAWS dropped for want of a pipeline on this backend (refused or build failed; every draw, not once);
                 *   legacy_gap_passes    passes begun with an unused colour slot (supported). */
                uint64_t legacy_shape_refused, legacy_pipe_why[12], legacy_begin_why[12], legacy_draws_dropped_pipe, legacy_gap_passes;
                uint64_t desc_sets, desc_unbound, desc_unaligned, desc_feedback, desc_failed, samplers, sampler_failed, point_no_size;
                uint64_t dump_objects, dump_bytes, dump_files, dump_skipped_cap, dump_truncated, dump_draws, dump_table_full;
                uint64_t cap_draws, cap_passes, cap_snaps, cap_bytes, cap_truncated, cap_skipped_filter, cap_skipped_cap, cap_failed, cap_blobs_dedup;   /* arm64_capture.c */
            } pipe;
            /* ARM64 tile shaders (src/protocol/arm64_tile.c): tile pipelines, tile binds, dispatches run as compute between two halves of the render pass */
            struct {
                uint64_t pipes, pipe_failed, pipe_no_kernel, pipe_missing_cap, pipe_bindings, pipe_unsupported_simd;   /* pipe_unsupported_simd: the kernel uses a SIMD-group / quad-group operation the translator cannot translate (its result would be undefined): not built */
                uint64_t binds_buf, binds_tex, binds_ignored;           /* 0x9d/0x9e, 0xa1 applied; samplers / threadgroup memory / dims (counted, not mapped) */
                uint64_t dispatches, groups, whole, region, region_unaligned, region_empty, rt_index_ignored, sg_pinned, sg_override, sg_emulated;
                uint64_t draws_ignored;                                 /* draw records while a tile pipeline was bound */
                uint64_t skip_no_pass, skip_no_pipe, skip_bad_rec, skip_local, skip_no_sg32, skip_rt, skip_unbound, skip_unaligned, skip_feedback, skip_desc, skip_pipe_build, resume_failed;
                uint64_t skip_rt_why[6];                                /* skip_rt by cause: 0 no such colour attachment, 1 format is neither R16G16B16A16_SFLOAT nor B8G8R8A8_UNORM, 2 no STORAGE usage, 3 no view / linear, 4 BGRA8: device lacks shaderStorageImageRead/WriteWithoutFormat, 5 BGRA8: format has no without-format storage access / not queryable (diagnostic: why an imageblock attachment was refused) */
                uint64_t bgra8_dispatches, bgra8_variants, bgra8_variant_failed;   /* dispatches that ran with a B8G8R8A8 imageblock / kernel variants built for them / variant builds that failed (the dispatch is then skipped: skip_pipe_build) */
                struct { uint32_t format, usage; uint8_t linear, valid; } skip_rt_first;   /* the first refused attachment: VkFormat, VkImageUsageFlags */
                uint64_t gather_gpu;                                    /* of the gathered copies: made by the GPU because a write to the buffer was recorded earlier in the exec */
                uint64_t gathered, gather_failed, unaligned_written;    /* read-only buffers bound through an aligned copy / copy impossible / unaligned buffers the kernel may write (skipped) */
#define LAGFX_TILE_SEEN_MAX 8u
                /* the first 8 distinct dispatch geometries and unaligned buffer binds (for the log) */
                struct lagfx_tile_geom { bool region; uint32_t ox, oy, rw, rh, tw, th, td, tgt_w, tgt_h; } geom[LAGFX_TILE_SEEN_MAX]; uint32_t n_geom;
                struct lagfx_tile_unal { uint32_t metal_index; bool written; uint64_t off, len, align; } unal[LAGFX_TILE_SEEN_MAX]; uint32_t n_unal;
            } tile;
            /* VK_QCOM_tile_shading opt-in (src/vulkan/qcom_tile.h): the tile dispatches that ran as compute emulation, and how many of them ran on a device that WAS created with the extension (the native path is not implemented: this stays the only path) */
            struct { uint64_t emul_dispatches, emul_while_enabled; } qcomtile;
            /* Format / usage / blend / MSAA / framebuffer-fetch ordering compat (src/vulkan/format_caps.c, arm64_resources.c, arm64_exec_run.c); printed as g7-gpu-fmt when any is non-zero. */
            struct {
                uint64_t img_created, img_trimmed;     /* mirror images created / of those, created with fewer usages than the mirror asked for (the device refused the combined mask or lacks a feature bit) */
                uint64_t dropped[6];                   /* trimmed usages by kind: 0 storage, 1 input attachment, 2 colour attachment, 3 sampled, 4 transfer dst, 5 transfer src */
                uint64_t refused[6];                   /* image creation refused by the capability query, by LAGFX_FMTCAP_* (why 0 = unused) */
                uint64_t blend_off_format;             /* pipeline slots built with blending OFF because the attachment format has no COLOR_ATTACHMENT_BLEND feature (VUID-vkCmdDraw-blendEnable-04727) */
                uint64_t target_not_attachable;        /* a pass named a texture whose format/usage cannot be a colour attachment on this device */
                uint64_t fbf_class_mismatch;           /* draws skipped: the fragment shader reads an input attachment as float/uint/sint but the attachment format is another class (VUID-vkCmdDraw-format-07753) */
                uint64_t fbf_split_draws, fbf_split_subdraws, fbf_split_capped, fbf_split_unsupported;   /* framebuffer-fetch draws without rasterization-order access: draws recorded one primitive at a time / the sub-draws made / draws over the cap (drawn whole) / topologies not split (drawn whole) */
                uint64_t msaa_refused;                 /* textures refused because the guest descriptor names more than one sample (PVG sample-count field is not wired to the Vulkan multisample path: see src/vulkan/msaa.h) */
                uint64_t devinfo_msaa_clamped;         /* guest device-info MSAASamples lowered because the host cannot do the advertised count */
            } fmt;
            lagfx_msaa_stats_t msaa;   /* multisample path (arm64_msaa.c, src/vulkan/msaa.c); printed as g7-gpu-msaa when any is non-zero */
        } exec;
        uint32_t last44_task[LAGFX_MAX_CHANNELS]; uint8_t last44_valid[LAGFX_MAX_CHANNELS];
        uint32_t last44_global_task; uint64_t last44_global_seq; uint8_t last44_global_valid;
    } diag2;

    /* Command counters (for diagnostics) */
    uint64_t total_cmds_seen;
    uint64_t total_cmds_completed;
    uint64_t unknown_opcode_count;

    /* === Per-dispatcher scratch buffers ============================
     *
     * Each dispatcher (root channel, compute vchan drain, display
     * vchan drain) and the inner exec walker need a 4 KiB scratch
     * to read one command (header + payload) before handing it to
     * the handler. The legacy layout used either a function-static
     * (\`exec_cmdbuf.c: static uint8_t buf[4096]\`) or a stack-local
     * (\`uint8_t cmd_buf[4096]\` in each drain loop). Static was
     * non-reentrant; stack burned 12 KiB on every BAR write.
     *
     * Single-threaded invariant: drain callbacks serialise through
     * QEMU's BQL today. The four scratches below are owned by the
     * named dispatcher and must never be used from another (incl.
     * cross-call recursion). If a future BQL-relaxed path lands,
     * scratches will need to move to thread-local — or be replaced
     * by per-channel mallocs in the CmdDefineChildChannel 0x30
     * handler. Document any such change here when made.
     */
    uint8_t scratch_ch0[LAGFX_MAX_RING_READ];
    uint8_t scratch_compute[LAGFX_MAX_RING_READ];
    uint8_t scratch_display[LAGFX_MAX_RING_READ];
    uint8_t scratch_exec[LAGFX_MAX_RING_READ];

    /* Growable heap buffer for CmdExecIndirect2 cmdbuf reads that exceed
     * the 4 KiB scratch_exec (outer length field allows up to 4 MiB). A
     * cmdbuf carries a SEQUENCE of encoder segments; truncating at 4 KiB
     * dropped every segment past the first page (blit segments serialize
     * after the compute/render segments). Owned by exec_cmdbuf.c; freed
     * in lagfx_protocol_free. */
    uint8_t  *scratch_exec_heap;
    uint32_t  scratch_exec_heap_size;

    /* ARM64 deferred stamp completion queue (src/protocol/arm64_completion.h). NULL unless LAGFX_ARM64_EXEC=1 in arm64 mode. */
    struct lagfx_arm64_cq *a64cq;
    /* ARM64 render execution state (object table + per-exec recording), NULL unless LAGFX_ARM64_EXEC=1 in arm64 mode. Owned by
     * src/protocol/arm64_exec_run.c; freed in lagfx_protocol_free / reset. */
    void *a64_exec;
    /* bounded per-draw diagnostics (src/protocol/draw_diag.c); NULL until the first draw, or when G7_DRAW_DIAG=0 (draw_diag_init then stays true) */
    struct lagfx_draw_diag *draw_diag; bool draw_diag_init;
    /* ARM64 exec resource mirror (src/protocol/arm64_resources.c); NULL until the first LAGFX_ARM64_EXEC=1 exec. Freed by
     * lagfx_protocol_free / lagfx_protocol_reset through lagfx_a64_res_destroy. */
    void     *a64_res;
    /* compute channels (bit = chan_id) whose next command is HELD in the ring (read pointer not advanced) because an exec is still in flight on the GPU:
     * channel_compute_dispatcher_redrain_held() runs them once it finished. Never set unless LAGFX_ARM64_EXEC=1 in arm64 mode. */
    uint32_t  a64_held_chan_mask;
    /* EXECUTION LOCK (distinct from the completion queue's lock; order: exec lock -> cq lock, never the reverse). A recursive owner-aware lock, zero-initialised
     * (works for protocols that are calloc'ed by tests). Held by: the compute drain (ARM64 + LAGFX_ARM64_EXEC=1), lagfx_arm64_poll_completions (try-lock),
     * the held-command redrive, cq shutdown and lagfx_a64_exec_free. It makes the ring read pointer, the held mask, the exec mirror and the in-flight exec
     * single-threaded: the vCPU thread (doorbell -> drain) and the shell timer thread (lagfx_device_poll_completions) never run them at the same time. A try-lock
     * that fails means 'someone is draining/retiring right now', and that thread polls itself. Lifetime: the shell must stop its timer before lagfx_device_free. */
    _Atomic uintptr_t a64_exec_owner;
    uint32_t  a64_exec_depth;       /* only touched by the owner */
    uint32_t  a64_drain_depth;      /* drain nesting, under the lock: no redrive from a poll made inside a drain */
    void    (*a64_redrive)(struct lagfx_protocol *p);   /* set by the compute dispatcher when it holds a command: runs the held channels (exec lock held) */

} lagfx_protocol_t;

/* The command rings' page size and PFN encoding. The shift is 12 unless the device was created with another ring_page_shift (the only accepted
   values are 12 and 14). It applies to the root, shared and channel PFNs, the ring's PFN-array indexing and in-page offsets, and the stamp cells; the
   resource-mapping lists keep 12-bit entries (Henge's pvg_lite does the same) and Vulkan alignment is untouched. A PFN that would not fit in 64 bits
   gives ~0, an address no memory callback accepts. */
static inline bool lagfx_proto_arm64(const lagfx_protocol_t *p) { return p && p->protocol_mode == 1u; }
static inline uint32_t lagfx_ring_page_shift(const lagfx_protocol_t *p) {
    return (p && p->ring_page_shift) ? p->ring_page_shift : 12u;
}
static inline uint32_t lagfx_ring_page_size(const lagfx_protocol_t *p) { return 1u << lagfx_ring_page_shift(p); }
static inline uint64_t lagfx_pfn_to_gpa(const lagfx_protocol_t *p, uint64_t pfn) {
    uint32_t sh = lagfx_ring_page_shift(p);
    if (sh >= 64u || (pfn >> (64u - sh)) != 0u) return ~(uint64_t)0;
    return pfn << sh;
}

/* Validation macro */
#define LAGFX_PROTOCOL_MAGIC 0x4C414758u  /* "LAGX" */
static inline int lagfx_protocol_is_valid(const lagfx_protocol_t *p) {
    return p != NULL && p->magic == LAGFX_PROTOCOL_MAGIC;
}

/* Accessor for last completed stamp - used by tests. */
static inline uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_completed_stamp : 0u;
}

/* Task table helpers — static inline for performance. */
static inline lagfx_task_entry_t* lagfx_protocol_find_task(lagfx_protocol_t *p, uint32_t task_id) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_TASKS; ++i) {
        if (p->tasks[i].live && p->tasks[i].id == task_id) {
            return &p->tasks[i];
        }
    }
    return NULL;
}

static inline lagfx_task_entry_t* lagfx_protocol_alloc_task_slot(lagfx_protocol_t *p) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_TASKS; ++i) {
        if (!p->tasks[i].live) {
            memset(&p->tasks[i], 0, sizeof(p->tasks[i]));
            p->tasks[i].live = false;
            return &p->tasks[i];
        }
    }
    return NULL;
}

/* FIFO table helpers. */
static inline lagfx_fifo_entry_t* lagfx_protocol_find_fifo(lagfx_protocol_t *p, uint32_t fifo_id) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_FIFOS; ++i) {
        if (p->fifos[i].live && p->fifos[i].id == fifo_id) {
            return &p->fifos[i];
        }
    }
    return NULL;
}

static inline lagfx_fifo_entry_t* lagfx_protocol_alloc_fifo_slot(lagfx_protocol_t *p) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_FIFOS; ++i) {
        if (!p->fifos[i].live) {
            memset(&p->fifos[i], 0, sizeof(p->fifos[i]));
            p->fifos[i].live = false;
            return &p->fifos[i];
        }
    }
    return NULL;
}

/* Stamp slot helpers — exported for tests. */
extern void lagfx_protocol_complete_stamp_slot(lagfx_protocol_t *p, uint32_t slot, uint32_t stamp);
extern void lagfx_protocol_complete_stamp_slot_noirq(lagfx_protocol_t *p, uint32_t slot, uint32_t stamp);
extern void lagfx_protocol_raise_completion_irq(lagfx_protocol_t *p);

/* Back-compat wrapper — completes slot 0 with given stamp value (defined in stamp.c). */
extern void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp);

#endif /* LAGFX_PROTOCOL_STATE_H */
