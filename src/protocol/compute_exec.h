/*
 * libapplegfx-vulkan — common Vulkan compute execution base (independent of any guest record format).
 *
 * What an independent Metal compute encoder needs on the Vulkan side, separated from the parsing of the guest's compute segment (whose record format is not yet known; see
 * seg_census.h): a compute pipeline over a SPIR-V kernel with an explicit binding list, descriptor binding of buffers / images / samplers, dispatch by threadgroups or by
 * threads, and the memory ordering around it. arm64_tile.c has the same steps fused with the tile-shader state; this module is the stand-alone form.
 *
 * Metal dispatchThreads(N, tg) runs EXACTLY N threads; Vulkan runs whole workgroups: lagfx_cx_threads_to_groups reports when the grid is not a multiple of the threadgroup
 * (nonuniform = true), and then the kernel must bound-check gl_GlobalInvocationID against N (supplied by the caller, e.g. through push constants). Not done here: that is
 * a translator job once the AIR of such a kernel is available.
 */
#ifndef LAGFX_PROTOCOL_COMPUTE_EXEC_H
#define LAGFX_PROTOCOL_COMPUTE_EXEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define LAGFX_CX_MAX_BIND 32u
#define LAGFX_CX_SET_POOL 64u    /* descriptor sets per pipeline between two lagfx_cx_recycle calls */

typedef struct { uint32_t binding; VkDescriptorType type; } lagfx_cx_bind_t;
typedef struct {
    uint32_t binding;
    VkBuffer buffer; VkDeviceSize offset, range;                    /* storage / uniform buffer */
    VkImageView view; VkImageLayout layout; VkSampler sampler;      /* (storage / sampled) image, sampler, or combined */
} lagfx_cx_res_t;

typedef struct lagfx_cx_pipe lagfx_cx_pipe_t;

/* NULL on failure (shader module / layout / pipeline creation). entry NULL = "main". push_bytes = size of the push-constant block (0 = none). */
lagfx_cx_pipe_t *lagfx_cx_pipe_create(VkDevice dev, const uint32_t *spv, size_t spv_bytes, const char *entry, const lagfx_cx_bind_t *bind, uint32_t nbind, uint32_t push_bytes);
/* The same with specialization constants and an up-front check against the device. spec[i] = {SpecId, value} (SpecId 0..2 = the workgroup size; 3 + n = the length in ELEMENTS of the
 * `[[threadgroup(n)]]` argument n). Refused (NULL): a duplicate SpecId; a threadgroup argument of the kernel (lagfx_spv_reflect_threadgroup_args) without an explicit value - the
 * in-module default is never used silently; with `lim` non-NULL, a total Workgroup-storage size (static threadgroup variables + the specialized threadgroup arguments) above
 * maxComputeSharedMemorySize, or a workgroup size above maxComputeWorkGroupInvocations / maxComputeWorkGroupSize (when SpecId 0..2 are given), or a Workgroup variable whose size
 * cannot be determined. `lim` is also installed as lagfx_cx_set_limits would. */
lagfx_cx_pipe_t *lagfx_cx_pipe_create_ex(VkDevice dev, const uint32_t *spv, size_t spv_bytes, const char *entry, const lagfx_cx_bind_t *bind, uint32_t nbind, uint32_t push_bytes,
                                         const uint32_t (*spec)[2], uint32_t nspec, const VkPhysicalDeviceLimits *lim);
/* Descriptor types this module binds: STORAGE_BUFFER, UNIFORM_BUFFER, STORAGE_IMAGE, SAMPLED_IMAGE, SAMPLER, COMBINED_IMAGE_SAMPLER; any other type fails lagfx_cx_pipe_create.
 * Optional: the device limits, checked at record time (workgroup count per axis, push-constant size). Without them only the zero checks apply. Buffer ranges: a buffer binding
 * needs a non-null VkBuffer and an offset that satisfies the caller's alignment (minStorageBufferOffsetAlignment / minUniformBufferOffsetAlignment, checked when the limits are
 * set); the buffer's size is the caller's to respect (this module does not know it). Image bindings need a non-null view (sampler bindings a non-null sampler); the image LAYOUT
 * and its transition are the caller's responsibility (the layout named in the binding must be the one the image is in when the dispatch executes). Descriptor sets are reused
 * only after lagfx_cx_recycle, which the caller calls once the GPU has COMPLETED the recorded work. */
void lagfx_cx_set_limits(lagfx_cx_pipe_t *p, const VkPhysicalDeviceLimits *lim);
void lagfx_cx_pipe_destroy(lagfx_cx_pipe_t *p);

/* Record: barrier(all -> compute), bind pipeline + a freshly allocated descriptor set written from res[] (every layout binding must be present exactly once), push
 * constants, vkCmdDispatch(gx,gy,gz), barrier(compute -> all). false (nothing recorded) when a binding is missing / duplicated / unknown, the set pool is exhausted
 * (call lagfx_cx_recycle once the work has completed) or a group count is 0. */
bool lagfx_cx_record(lagfx_cx_pipe_t *p, VkCommandBuffer cb, const lagfx_cx_res_t *res, uint32_t nres, const void *push, uint32_t push_bytes, uint32_t gx, uint32_t gy, uint32_t gz);
void lagfx_cx_recycle(lagfx_cx_pipe_t *p);   /* the recorded work has completed: all descriptor sets become reusable */

/* dispatchThreads -> workgroup counts (ceil). Returns false for a zero extent. *nonuniform = some axis has a partial last group. */
bool lagfx_cx_threads_to_groups(const uint32_t threads[3], const uint32_t tg[3], uint32_t groups[3], bool *nonuniform);
#endif
