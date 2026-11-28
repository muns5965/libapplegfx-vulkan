/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — '422f' (kCVPixelFormatType_422YpCbCr8BiPlanarFullRange) guest surfaces as two device images.
 * src/vulkan/planar422.h
 *
 * A bi-planar 4:2:2 surface is TWO planes in one guest allocation: Y (1 byte per pixel, width x height) and CbCr (interleaved Cb, Cr: 2 bytes per chroma sample, ceil(width / 2) samples
 * per row, FULL height: 4:2:2 subsamples horizontally only). Each plane has its own byte offset and row pitch. The guest converts YCbCr <-> RGB in its own shaders and uses the planes
 * as separate textures, so the mirror is two images (R8_UNORM for Y, R8G8_UNORM for CbCr) and NO Vulkan YCbCr conversion: the stored bytes are moved unchanged.
 *
 * This module takes the plane geometry as INPUT (lagfx_p422_layout_t): reading it out of the guest's surface descriptor is a separate step (the descriptor's plane fields are not
 * known yet), and guest memory is reached through a byte-offset read / write callback (surface-relative), so the same code serves an in-memory test buffer and the guest page list.
 * Row padding (pitch beyond the payload bytes of a row) and everything outside the two planes is never read into an image and never written back: write-back touches payload bytes only.
 *
 * Each plane has its own dirty flag: "the GPU wrote it" (lagfx_p422_mark_gpu_written / lagfx_p422_fill) and the download of a plane that is not dirty is skipped. upload() takes
 * the guest's bytes for the requested planes only, clears their dirty flag (the guest's newer bytes replace what the GPU owed) and leaves the other plane alone.
 *
 * Threading: none; the caller serializes. Every operation is synchronous (one-shot command buffer, queue wait).
 */
#ifndef LAGFX_VULKAN_PLANAR422_H
#define LAGFX_VULKAN_PLANAR422_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_P422_FOURCC 0x34323266u   /* '422f' */
enum { LAGFX_P422_Y = 0, LAGFX_P422_CBCR = 1 };
#define LAGFX_P422_PLANE_Y    1u
#define LAGFX_P422_PLANE_CBCR 2u
#define LAGFX_P422_PLANES_ALL 3u

typedef struct {
    uint32_t width, height;         /* luma size in pixels (1 .. 8192) */
    uint64_t y_off, y_pitch;        /* surface-relative byte offset of Y row 0, bytes per Y row (>= width) */
    uint64_t c_off, c_pitch;        /* the same for the CbCr plane (pitch >= 2 * ceil(width / 2)) */
    uint64_t alloc;                 /* bytes of the surface the guest gave (both planes must lie inside) */
} lagfx_p422_layout_t;

/* Payload bytes of one row of a plane, and the plane's size in rows. */
static inline uint32_t lagfx_p422_chroma_width(uint32_t w) { return (w + 1u) / 2u; }
static inline uint64_t lagfx_p422_row_bytes(const lagfx_p422_layout_t *l, uint32_t plane) { return plane == LAGFX_P422_Y ? l->width : 2ull * lagfx_p422_chroma_width(l->width); }

/* 0 = usable; otherwise a LAGFX_P422_BAD_* code (and, when why != NULL, a sentence in it). Checks: size 1..8192; pitch >= payload row; both planes inside alloc (offset + pitch * (rows - 1) +
 * payload, overflow-checked); the two planes' byte ranges do not overlap. */
enum { LAGFX_P422_OK = 0, LAGFX_P422_BAD_SIZE, LAGFX_P422_BAD_PITCH, LAGFX_P422_BAD_BOUNDS, LAGFX_P422_BAD_OVERLAP };
int lagfx_p422_layout_check(const lagfx_p422_layout_t *l, char *why, size_t why_cap);

/* Guest memory, surface-relative: copy `len` bytes at byte offset `off` to / from `buf`. false = the transfer failed (nothing is assumed about the bytes). */
typedef bool (*lagfx_p422_rw_fn)(void *ctx, uint64_t off, uint64_t len, void *buf, bool to_guest);

/* One plane between guest memory and a TIGHT buffer (payload row bytes per row, `height` rows), row by row: padding between rows is neither read nor written. */
bool lagfx_p422_xfer_plane(lagfx_p422_rw_fn rw, void *ctx, const lagfx_p422_layout_t *l, uint32_t plane, uint8_t *tight, bool to_guest);

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
typedef struct lagfx_p422 lagfx_p422_t;

/* The two images of `l` on the given device. NULL (and *why filled) when the layout is unusable, R8 / R8G8 UNORM cannot be sampled + transferred on this device, or an allocation fails.
 * The images are left in VK_IMAGE_LAYOUT_GENERAL (what the exec mirror uses everywhere); contents are undefined until uploaded or filled. */
lagfx_p422_t *lagfx_p422_create(VkPhysicalDevice pd, VkDevice dev, VkQueue q, uint32_t queue_family, const lagfx_p422_layout_t *l, char *why, size_t why_cap);
void lagfx_p422_free(lagfx_p422_t *p);                       /* the device must be idle; NULL ok */

VkImage lagfx_p422_image(const lagfx_p422_t *p, uint32_t plane);
VkImageView lagfx_p422_view(const lagfx_p422_t *p, uint32_t plane);
bool lagfx_p422_has_storage(const lagfx_p422_t *p, uint32_t plane);   /* the device offers STORAGE for that plane's format (the image was made with it) */
const lagfx_p422_layout_t *lagfx_p422_layout(const lagfx_p422_t *p);

/* guest -> image for the planes in `mask`; their dirty flags are cleared. false = a plane transfer failed (the planes done before it stay uploaded). */
bool lagfx_p422_upload(lagfx_p422_t *p, lagfx_p422_rw_fn rw, void *ctx, uint32_t mask);
/* image -> guest for the planes in `mask` that are DIRTY (`force` = all of `mask`); payload bytes only. Returns the mask of planes written; a failed plane stays dirty. */
uint32_t lagfx_p422_download(lagfx_p422_t *p, lagfx_p422_rw_fn rw, void *ctx, uint32_t mask, bool force);
void lagfx_p422_mark_gpu_written(lagfx_p422_t *p, uint32_t mask);
uint32_t lagfx_p422_dirty(const lagfx_p422_t *p);
/* Fill a plane on the GPU with `v` (Y: v[0]; CbCr: v[0] = Cb, v[1] = Cr; 0..1) and mark it dirty: the stand-in for a guest render into that plane. */
bool lagfx_p422_fill(lagfx_p422_t *p, uint32_t plane, const float v[2]);
/* The guest re-mapped the surface: same size -> only the layout (offsets / pitches / alloc) is replaced, images and dirty flags are kept; another size -> the images are re-made (contents
 * undefined, dirty cleared). false = the layout is unusable or re-making failed (the old state is kept). */
bool lagfx_p422_remap(lagfx_p422_t *p, const lagfx_p422_layout_t *l, char *why, size_t why_cap);
#endif /* LAGFX_HAVE_VULKAN */
#endif /* LAGFX_VULKAN_PLANAR422_H */
