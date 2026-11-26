/*
 * libapplegfx-vulkan — ARM64 render replay CAPTURE: on-disk format (version 1) + loader for an offline replay tool.
 * src/protocol/arm64_capture.h
 *
 * Extends the LAGFX_ARM64_DUMP capture (arm64_dump.h). OFF by default; needs LAGFX_ARM64_EXEC=1 (the data comes from the executor's Vulkan mirror), LAGFX_ARM64_DUMP=1
 * and an open LAGFX_ARM64_DUMP_DIR, plus
 *   LAGFX_ARM64_DUMP_DATA=1            enable draw-data capture
 *   LAGFX_ARM64_DUMP_DATA_PNG=1        also write a PNG of each colour attachment AFTER the pass (8-bit conversion of the raw data; formats it cannot convert are skipped)
 *   LAGFX_ARM64_DUMP_DATA_MAX_OBJ      bytes saved per buffer / image snapshot   (default 8 MiB; more is truncated, see `trunc`)
 *   LAGFX_ARM64_DUMP_DATA_MAX_TOTAL    new blob bytes written over the whole run (default 128 MiB; then no blob is written, records still list the draws with blob=0)
 *   LAGFX_ARM64_DUMP_DATA_MAX_DRAWS    draws captured per process               (default 256)
 * Everything lands in LAGFX_ARM64_DUMP_DIR next to the existing dump (index.txt is appended to; the existing `draw` / `obj` / `fn` / `pso` lines are untouched).
 * Counters: diag2.exec.pipe.cap_* (draws, passes, snapshots, bytes, truncated, skipped by cap / failed).
 *
 * ===== FORMAT =========================================================================================================================================
 * index.txt is line oriented text. A capture line starts with the token `cap_<kind>` followed by space separated `key=value` tokens (no spaces inside a value;
 * lists use ',' and ':'). Numbers are decimal unless the key says hex (hash, bits). Floats are printed with %.9g and are exact for float32. Unknown keys and
 * unknown `cap_*` kinds MUST be ignored by readers (additive evolution); incompatible changes bump `version`. Lines of other kinds (obj, fn, pso, draw, spv,
 * mapped) are not part of this format.
 * All blobs are raw little-endian bytes in files named in `cap_blob` lines. Identical content shares one file (name = cap_<fnv1a64 hex>.bin); a blob cut by a cap is
 * saved as a prefix (`trunc=1`, `bytes` < `full`). `blob=0` always means "no data" (not captured / cap reached / readback failed).
 *
 *   cap_header version=1 byte_order=le
 *       first capture line of each process run (a dir may hold several runs; ids restart at 1 after each header: a reader keeps only the LAST run or
 *       treats each header as a section).
 *   cap_blob id= file= bytes= full= trunc=0|1 hash=<hex>
 *       id>=1. `bytes` = size of the file, `full` = size the data had before the cap.
 *   cap_pass id= exec= task= w= h= ncolor= depth=0|1 depth_ref= depth_vkfmt= stencil=0|1 depth_load= depth_clear= stencil_load= stencil_clear=
 *       One Render pass (tag 26 .. next tag 26 / end of exec). w,h = render area. Load actions: Metal MTLLoadAction (0 DontCare, 1 Load, 2 Clear). The executor ALWAYS
 *       stores (store action is not honoured). Depth / stencil CONTENTS are not captured (state only).
 *   cap_att pass= slot= ref= gen= vkfmt= mtlfmt= w= h= bpp= load= clear=r,g,b,a pre=<img id|0> post=<img id|0> post_png=<file|->
 *       Colour attachment `slot` (the fragment output location). pre = contents immediately BEFORE the pass (what a Load action / framebuffer fetch / blending sees; for
 *       load=2 (Clear) the pre image is still the old content, the clear value is `clear`), post = contents after the pass (raw, the attachment's own format: for
 *       RGhA surfaces / RGBA16Float this IS FP16 raw). Both are `cap_img` ids with layout=tight.
 *   cap_img id= task= ref= gen= vkfmt= mtlfmt= w= h= d= mips= layers= bpp= layout=tight|mirror tex_base= tex_stride= block_h= miptab=<off:row:w:h:d,...> blob= len= pass= use=pre|post|tex
 *       A texture snapshot. vkfmt = VkFormat number, mtlfmt = Metal pixel format number from the guest descriptor (0 unknown), gen = mirror object generation.
 *       layout=tight: level 0 / layer 0 only, rows packed (row pitch = w*bpp bytes, h rows; depth d=1). `len` = bytes of the blob.
 *       layout=mirror: ALL levels / layers exactly as stored in the host mirror's guest layout (= what the guest sees: pitch-linear per mip, NOT swizzled; Apple's texture
 *         layout table): the byte of (layer s, mip l, row y, texel x) is at tex_base + tex_stride*s + mip[l].off + y*mip[l].row + x*bpp (3D: + z*row*h). `miptab` lists
 *         mip[l] = off:row:w:h:d for l < mips (mips = number of levels; layers = array length, 1 for 3D). blob length = the mirror object's length (rounded to 16 KiB);
 *         when cut by the cap only a prefix is present (trunc in cap_blob): readers must treat missing bytes as zero. The content is read from the GPU image
 *         (not the stale CPU mirror), for the regions the executor itself would write back.
 *       pass = the pass the snapshot was taken for (an image used by several passes is snapshotted again per pass).
 *   cap_buf id= task= ref= gen= length= off= len= blob= pass=
 *       A buffer snapshot: the object has `length` bytes (rounded to 16 KiB as mirrored); the blob holds bytes [off, off+len) of it (off=0,len=length unless cut by the cap:
 *       then a window that starts at the lowest offset any draw of the pass binds, 16-byte aligned down).
 *   cap_samp id= ref= gen= min=<0 nearest,1 linear> mag= mip=<0 nearest,1 linear|none> mip_enabled=0|1 addr_u=<VkSamplerAddressMode> addr_v= addr_w= lod_min= lod_max= lod_bias=
 *            aniso= compare_enable= compare_op= border=<VkBorderColor> unnorm=0|1
 *       Sampler state as the executor built it (Vulkan values; this is the decoded descriptor; the raw guest descriptor is the `obj` line of the same ref in the dump). lod_min /
 *       lod_max are the effective clamps (including setFragmentSamplerState:lodMinClamp: variants).
 *   cap_draw n= pass= task= tag= prim=<0 point,1 line,2 line strip,3 triangle,4 triangle strip> indexed=0|1 count= instances= first_vertex= base_vertex= base_instance=
 *            index_start= index_type=<0 u16,1 u32> index_off= index_buf=<buf id|0> pipe_ref= pipe_gen= pipe_hash=<hex> vs_spv=<blob id|0> fs_spv=<blob id|0>
 *            vtx_buf_slot= vtx_buf=<buf id|0> vtx_off= vtx_stride= vtx_in=<loc:comp,...> vtx_attrs=<mtlvertexformat:offset,...>
 *            vp=<x,y,w,h,znear,zfar> sc=<x,y,w,h> cull=<0 none,1 front,2 back> winding=<0 cw,1 ccw> bias=<const,slope,clamp>
 *            blend_set= blend=<enable> blend_rgb=<src,dst,op> blend_a=<src,dst,op> write_mask= ncolor_fmts= color_fmts=<vkfmt,...>(by slot, 0 = unused) nbind=
 *            depth_test= depth_write= depth_cmp=<Metal MTLCompareFunction> stencil_test= sref=<front,back>
 *       One executed draw (a draw the executor skipped is not captured). Blend values are VULKAN enums (VkBlendFactor / VkBlendOp / VkColorComponentFlags) as decoded by the
 *       pipeline resolver (colour attachment 0; blend_set=0 means the executor's fixed premultiplied source-over). Depth state is the Metal encoder state (0 = default).
 *       vs_spv / fs_spv: the SPIR-V the executor built the pipeline from (bindings of set 0 as in cap_bind; fragment bindings are already offset by 16).
 *       Vertex data: Vulkan binding 0 = vtx_buf at vtx_off (vtx_stride per vertex, attributes vtx_in / vtx_attrs as in lagfx_pipeline_desc_t), same as the executor binds it.
 *       Index data: index_buf + index_off, `count` indices starting at index_start (after any indirect-argument decoding: the values here are the FINAL draw arguments).
 *       Attachments of the draw's pass: the `cap_att` lines with the same pass id.
 *   cap_bind draw= binding= kind=buffer|image|sampler|input|storage_image stage=vs|fs idx= writable=0|1 buf=<buf id> off= range= img=<img id> samp=<samp id>
 *       One set-0 descriptor of the draw's pipeline (binding = Vulkan binding, idx = Metal table index of that stage, off = bound byte offset, range = descriptor range
 *       actually bound). Exactly one of buf / img / samp is present (input: the attachment index is idx, no data: the contents are cap_att pre + earlier draws of the pass).
 *       An unresolved binding has 0 (the executor skips such draws, so normally all are set).
 *
 * ===== SYNC ASSUMPTIONS (read before trusting the data) ================================================================================================
 *  1. One exec = one command buffer. Buffers / images are snapshotted with GPU copies recorded OUTSIDE the render pass: the attachments' `pre` copy right before
 *     vkCmdBeginRendering, the `post` copy after the pass has ended, every sampled / bound buffer or texture right after the pass has ended. The data is read on the host only
 *     after the exec's fence signalled (the capture finishes in the same place that does the guest write-backs; with a PENDING exec the files appear when it completes).
 *  2. A resource bound by a draw is therefore captured in the state it had at the END of its pass. This equals the state at draw time unless a draw of the same pass writes it
 *     (storage buffers / storage images: bind `writable=1`) -- then the replayed draw sees the pass-end value (an UNVERIFIED simplification). Sampling an attachment of the
 *     same pass is rejected by the executor, so textures are exact.
 *  3. Earlier execs' GPU results are included (the copies read the Vulkan objects, never the CPU mirror of a buffer/texture the GPU wrote). Type 11 (mapped) surfaces are read from
 *     their shared device image, not from guest pages.
 *  4. Not captured: depth / stencil contents, vertex-stage textures / samplers (tags 129/127: not executed), tile-shader state, indirect argument buffers (their decoded result is in cap_draw).
 */
#ifndef LAGFX_PROTOCOL_ARM64_CAPTURE_H
#define LAGFX_PROTOCOL_ARM64_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_CAP_VERSION 1

/* ---------------------------------------------------------------- loader (no Vulkan needed: this part is in the library on every build) ---- */

typedef struct { uint32_t id; char file[96]; uint64_t bytes, full, hash; bool trunc; } lagfx_cap_blob_t;

typedef struct {
    uint32_t id, task, ref; uint64_t gen;
    uint32_t vkfmt, mtlfmt, w, h, d, mips, layers, bpp;
    bool mirror;                                   /* layout=mirror (else tight) */
    uint64_t tex_base, tex_stride; uint32_t block_h;
    struct { uint64_t off, row; uint32_t w, h, d; } mip[16];
    uint32_t nmip_tab;                             /* entries of mip[] present in miptab */
    uint32_t blob; uint64_t len; uint32_t pass; char use[8];
} lagfx_cap_img_t;

typedef struct { uint32_t id, task, ref; uint64_t gen, length, off, len; uint32_t blob, pass; } lagfx_cap_buf_t;

typedef struct {
    uint32_t id, ref; uint64_t gen;
    uint32_t min_filter, mag_filter, mip_filter, mip_enabled, addr_u, addr_v, addr_w, aniso, compare_enable, compare_op, border, unnorm;
    float lod_min, lod_max, lod_bias;
} lagfx_cap_samp_t;

typedef struct {
    uint32_t pass, slot, ref; uint64_t gen;
    uint32_t vkfmt, mtlfmt, w, h, bpp, load; float clear[4];
    uint32_t pre, post; char post_png[96];
} lagfx_cap_att_t;

typedef struct {
    uint32_t id, exec, task, w, h, ncolor;
    bool depth, stencil; uint32_t depth_ref, depth_vkfmt, depth_load, stencil_load; float depth_clear; uint32_t stencil_clear;
} lagfx_cap_pass_t;

typedef struct {
    uint32_t draw, binding; char kind[16]; char stage[4]; uint32_t idx, writable, buf, img, samp; uint64_t off, range;
} lagfx_cap_bind_t;

typedef struct {
    uint32_t n, pass, task, tag, prim, indexed;
    uint64_t count, instances, first_vertex, base_instance; int64_t base_vertex;
    uint32_t index_start, index_type; uint64_t index_off; uint32_t index_buf;
    uint32_t pipe_ref; uint64_t pipe_gen, pipe_hash; uint32_t vs_spv, fs_spv;
    uint32_t vtx_buf_slot, vtx_buf; uint64_t vtx_off; uint32_t vtx_stride;
    uint32_t n_vtx_in, vtx_in_loc[8], vtx_in_comp[8], n_vtx_attr, vtx_attr_fmt[8], vtx_attr_off[8];
    float vp[6]; uint32_t sc[4];
    uint32_t cull, winding; float bias[3];
    uint32_t blend_set, blend, blend_rgb[3], blend_a[3], write_mask, ncolor_fmts, color_fmts[4];
    uint32_t depth_test, depth_write, depth_cmp, stencil_test, sref[2];
    uint32_t nbind, first_bind;                    /* binds[first_bind .. first_bind + nbind) of lagfx_cap_t belong to this draw */
} lagfx_cap_draw_t;

typedef struct lagfx_cap {
    char dir[512];
    uint32_t version;
    lagfx_cap_blob_t *blobs; size_t nblobs;
    lagfx_cap_pass_t *passes; size_t npasses;
    lagfx_cap_att_t *atts; size_t natts;
    lagfx_cap_img_t *imgs; size_t nimgs;
    lagfx_cap_buf_t *bufs; size_t nbufs;
    lagfx_cap_samp_t *samps; size_t nsamps;
    lagfx_cap_draw_t *draws; size_t ndraws;
    lagfx_cap_bind_t *binds; size_t nbinds;
    uint64_t lines_unknown;                        /* cap_* lines of an unknown kind (ignored) */
} lagfx_cap_t;

/* Parses <dir>/index.txt. The LAST cap_header of the file starts the run that is loaded (earlier runs of the same directory are skipped). NULL + *err (static string)
 * if the directory / index cannot be read or there is no cap_header. Blobs are not read here. */
lagfx_cap_t *lagfx_cap_load(const char *dir, const char **err);
void lagfx_cap_free(lagfx_cap_t *c);
/* Lookups by id (ids are dense from 1 but do not rely on it); NULL when absent / 0. */
const lagfx_cap_blob_t *lagfx_cap_find_blob(const lagfx_cap_t *c, uint32_t id);
const lagfx_cap_img_t *lagfx_cap_find_img(const lagfx_cap_t *c, uint32_t id);
const lagfx_cap_buf_t *lagfx_cap_find_buf(const lagfx_cap_t *c, uint32_t id);
const lagfx_cap_samp_t *lagfx_cap_find_samp(const lagfx_cap_t *c, uint32_t id);
const lagfx_cap_pass_t *lagfx_cap_find_pass(const lagfx_cap_t *c, uint32_t id);
/* Reads blob `id` completely into a malloc'd buffer (caller frees); *len = the file size (may be < the blob's `full`). NULL if blob 0 / unknown / unreadable. */
uint8_t *lagfx_cap_read_blob(const lagfx_cap_t *c, uint32_t id, size_t *len);

/* ---------------------------------------------------------------- writer side (used by arm64_exec_run.c) ---- */
#include "state.h"
bool lagfx_a64_capture_enabled(void);   /* LAGFX_ARM64_DUMP=1 && LAGFX_ARM64_DUMP_DATA=1, read at every call */
void lagfx_a64_capture_reset(void);     /* tests / a new run: forgets the counters and the header marker */

#endif
