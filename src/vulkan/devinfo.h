/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — read-only device identity / extension / feature diagnostic.
 * src/vulkan/devinfo.h
 *
 * Once per process (from lagfx_vk_init) the WARN log gets lines tagged `g7-vkdev:` (each <= LAGFX_VKDEV_LINE_MAX characters incl. the tag):
 *   g7-vkdev: id ...        name / type / vendor / device / api / driverVersion / driverName / driverInfo / driverID / conformance
 *   g7-vkdev: exts n=<N> part=<i>/<k> <name>/<specVersion> ...      the FULL vkEnumerateDeviceExtensionProperties list, sorted, split over lines
 *   g7-vkdev: shader_tile_image ... | qcom_tile_shading ... | filter_cubic ... | dyn_local_read ... | raster_order ... | subgroup ...
 *   g7-vkdev: fmt mtl=80 vk=44 ... / fmt mtl=115 vk=97 ...      optimal/linear tiling format features of the two formats the Metal 80/115 formats map to
 * Feature/property structs of an extension are chained ONLY when the device lists that extension (or, for core features, the API version); nothing is
 * enabled, created or written: vkGet* queries only. The whole output is capped (LAGFX_VKDEV_MAX_LINES); the default build output is otherwise unchanged.
 */
#ifndef LIBAPPLEGFX_VULKAN_DEVINFO_H
#define LIBAPPLEGFX_VULKAN_DEVINFO_H

#include <stddef.h>

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

#define LAGFX_VKDEV_LINE_MAX 700u
#define LAGFX_VKDEV_MAX_LINES 40u

typedef void (*lagfx_vkdev_emit_fn)(void *user, const char *line);

/* Produces every line through `emit` (no logging, no global state); returns the number of lines. */
size_t lagfx_vk_devinfo_collect(VkPhysicalDevice pd, lagfx_vkdev_emit_fn emit, void *user);

/* Logs the lines at WARN level, once per process (later calls return 0). Returns the number of lines logged. */
size_t lagfx_vk_devinfo_log_once(VkPhysicalDevice pd);
#endif

#endif /* LIBAPPLEGFX_VULKAN_DEVINFO_H */
