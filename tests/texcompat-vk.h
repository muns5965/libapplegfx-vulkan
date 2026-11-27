/* Small Vulkan 1.3 test harness shared by the texture-compat tests (air2spv-unnorm-use-lavapipe.c, texture-compat-lavapipe.c): lavapipe device (or any CPU device), optional
 * Khronos validation layer with a debug-utils messenger that COUNTS messages (so a test can assert that a deliberately invalid control triggers a VUID and a valid case
 * triggers none), textures with mip chains (2D / 3D, any format), and a persistent full-screen-triangle pipeline that renders one fragment shader into an 8 x 8 RGBA32F
 * target. Include after <vulkan/vulkan.h>, <stdio.h>, <stdlib.h>, <string.h>. Results of this harness describe the software device only; nothing here predicts any GPU.
 * SPDX-License-Identifier: MIT */
#ifndef TEXCOMPAT_VK_H
#define TEXCOMPAT_VK_H

#include "air2spv-run-vert.h"   /* VERT_SPV (full-screen triangle), static const */

#define TCV_W 8
#define TCV_H 8
#define TCV_FAIL(...) do { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)
#define TCV_VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_); return 1; } } while (0)

static VkInstance tcv_inst; static VkPhysicalDevice tcv_pd; static VkDevice tcv_dev; static VkQueue tcv_q;
static int tcv_validation;                 /* 1 when the Khronos validation layer is active */
static char tcv_devname[256];
static VkPhysicalDeviceFeatures tcv_feat;  /* supported features */
static VkPhysicalDeviceLimits tcv_limits;
#define TCV_MSG_MAX 64
static unsigned tcv_nmsg; static char tcv_msg[TCV_MSG_MAX][400];

static VKAPI_ATTR VkBool32 VKAPI_CALL tcv_dbg(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT ty, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)ty; (void)u;
    if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        if (tcv_nmsg < TCV_MSG_MAX) snprintf(tcv_msg[tcv_nmsg], sizeof tcv_msg[0], "%s | %.300s", d->pMessageIdName ? d->pMessageIdName : "?", d->pMessage ? d->pMessage : "");
        tcv_nmsg++;
    }
    return VK_FALSE;
}
static void tcv_msgs_reset(void) { tcv_nmsg = 0; }
static unsigned tcv_msgs_with(const char *needle) { unsigned n = 0; for (unsigned i = 0; i < tcv_nmsg && i < TCV_MSG_MAX; i++) if (strstr(tcv_msg[i], needle)) n++; return n; }
static void tcv_msgs_print(void) { for (unsigned i = 0; i < tcv_nmsg && i < TCV_MSG_MAX; i++) printf("  validation: %.350s\n", tcv_msg[i]); }

/* 0 = ok, 77 = skip (no device). */
static int tcv_init(int want_validation, const char **dev_exts, uint32_t n_dev_exts) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "texcompat", .apiVersion = VK_API_VERSION_1_3 };
    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" }, *iexts[1] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    if (want_validation) {
        uint32_t nl = 0; vkEnumerateInstanceLayerProperties(&nl, NULL);
        VkLayerProperties *lp = calloc(nl ? nl : 1, sizeof *lp); vkEnumerateInstanceLayerProperties(&nl, lp);
        for (uint32_t i = 0; i < nl; i++) if (!strcmp(lp[i].layerName, layers[0])) tcv_validation = 1;
        free(lp);
        if (tcv_validation) ici.enabledLayerCount = 1, ici.ppEnabledLayerNames = layers, ici.enabledExtensionCount = 1, ici.ppEnabledExtensionNames = iexts;
    }
    if (vkCreateInstance(&ici, NULL, &tcv_inst) != VK_SUCCESS) { printf("SKIP: no Vulkan ICD\n"); return 77; }
    if (tcv_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT cm = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(tcv_inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, .pfnUserCallback = tcv_dbg };
        VkDebugUtilsMessengerEXT m; if (!cm || cm(tcv_inst, &mi, NULL, &m) != VK_SUCCESS) tcv_validation = 0;
    }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(tcv_inst, &nd, NULL);
    if (!nd) { printf("SKIP: no Vulkan device\n"); return 77; }
    VkPhysicalDevice *pds = malloc(nd * sizeof *pds); vkEnumeratePhysicalDevices(tcv_inst, &nd, pds);
    tcv_pd = pds[0];
    for (uint32_t i = 0; i < nd; i++) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { tcv_pd = pds[i]; break; } }
    free(pds);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(tcv_pd, &pp);
    snprintf(tcv_devname, sizeof tcv_devname, "%s", pp.deviceName); tcv_limits = pp.limits;
    if (pp.apiVersion < VK_API_VERSION_1_3) { printf("SKIP: device is not Vulkan 1.3 (dynamic rendering)\n"); return 77; }
    vkGetPhysicalDeviceFeatures(tcv_pd, &tcv_feat);
    VkPhysicalDeviceVulkan12Features p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 pf2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &p12 };
    vkGetPhysicalDeviceFeatures2(tcv_pd, &pf2);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .samplerMirrorClampToEdge = p12.samplerMirrorClampToEdge };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12, .features = { .samplerAnisotropy = tcv_feat.samplerAnisotropy } };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .enabledExtensionCount = n_dev_exts, .ppEnabledExtensionNames = dev_exts };
    if (vkCreateDevice(tcv_pd, &dci, NULL, &tcv_dev) != VK_SUCCESS) { printf("SKIP: vkCreateDevice failed\n"); return 77; }
    vkGetDeviceQueue(tcv_dev, 0, 0, &tcv_q);
    return 0;
}

static uint32_t tcv_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(tcv_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xffffffffu;
}

static int tcv_submit_wait(VkCommandBuffer cmd) {
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    TCV_VK(vkQueueSubmit(tcv_q, 1, &si, VK_NULL_HANDLE)); TCV_VK(vkQueueWaitIdle(tcv_q));
    return 0;
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } tcv_tex_t;

/* data = all mip levels, level 0 first, each tightly packed (width/height/depth halve, minimum 1). swz may be NULL. */
static int tcv_tex_create(tcv_tex_t *t, int is3d, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t texel_bytes, const void *data, const VkComponentMapping *swz) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, d }, .mipLevels = mips, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    TCV_VK(vkCreateImage(tcv_dev, &ici, NULL, &t->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(tcv_dev, t->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = tcv_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    TCV_VK(vkAllocateMemory(tcv_dev, &mai, NULL, &t->mem)); TCV_VK(vkBindImageMemory(tcv_dev, t->img, t->mem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 } };
    if (swz) vci.components = *swz;
    TCV_VK(vkCreateImageView(tcv_dev, &vci, NULL, &t->view));
    size_t total = 0; uint32_t lw = w, lh = h, ld = d;
    for (uint32_t l = 0; l < mips; l++) { total += (size_t)lw * lh * ld * texel_bytes; lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1; ld = ld > 1 ? ld >> 1 : 1; }
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkBuffer sb; TCV_VK(vkCreateBuffer(tcv_dev, &bci, NULL, &sb)); vkGetBufferMemoryRequirements(tcv_dev, sb, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = tcv_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory smem; TCV_VK(vkAllocateMemory(tcv_dev, &mai, NULL, &smem)); TCV_VK(vkBindBufferMemory(tcv_dev, sb, smem, 0));
    void *p; TCV_VK(vkMapMemory(tcv_dev, smem, 0, VK_WHOLE_SIZE, 0, &p)); memcpy(p, data, total); vkUnmapMemory(tcv_dev, smem);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool; TCV_VK(vkCreateCommandPool(tcv_dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; TCV_VK(vkAllocateCommandBuffers(tcv_dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    TCV_VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    size_t off = 0; lw = w, lh = h, ld = d;
    for (uint32_t l = 0; l < mips; l++) {
        VkBufferImageCopy bic = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .imageExtent = { lw, lh, ld } };
        vkCmdCopyBufferToImage(cmd, sb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        off += (size_t)lw * lh * ld * texel_bytes; lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1; ld = ld > 1 ? ld >> 1 : 1;
    }
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    TCV_VK(vkEndCommandBuffer(cmd));
    if (tcv_submit_wait(cmd)) return 1;
    vkDestroyCommandPool(tcv_dev, pool, NULL); vkDestroyBuffer(tcv_dev, sb, NULL); vkFreeMemory(tcv_dev, smem, NULL);
    return 0;
}
static void tcv_tex_destroy(tcv_tex_t *t) { vkDestroyImageView(tcv_dev, t->view, NULL); vkDestroyImage(tcv_dev, t->img, NULL); vkFreeMemory(tcv_dev, t->mem, NULL); memset(t, 0, sizeof *t); }

/* ---- persistent pipeline: one fragment shader, descriptor set 0 with the given bindings (image = SAMPLED_IMAGE, sampler = SAMPLER), optional fragment push constants ---- */
#define TCV_MAXB 8
typedef struct {
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; VkShaderModule vs, fs; VkDescriptorPool pool; VkDescriptorSet set; VkCommandPool cpool;
    uint32_t nb, binding[TCV_MAXB]; int is_sampler[TCV_MAXB]; uint32_t push_bytes;
    VkImage rimg; VkDeviceMemory rmem; VkImageView rview; VkBuffer buf; VkDeviceMemory bmem;
} tcv_pipe_t;

static int tcv_pipe_create(tcv_pipe_t *P, const uint32_t *fs, size_t fs_bytes, const uint32_t *bindings, const int *is_sampler, uint32_t nb, uint32_t push_bytes) {
    memset(P, 0, sizeof *P);
    P->nb = nb, P->push_bytes = push_bytes;
    VkDescriptorSetLayoutBinding lb[TCV_MAXB];
    for (uint32_t i = 0; i < nb; i++) {
        P->binding[i] = bindings[i], P->is_sampler[i] = is_sampler[i];
        lb[i] = (VkDescriptorSetLayoutBinding){ .binding = bindings[i], .descriptorType = is_sampler[i] ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    }
    VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = lb };
    TCV_VK(vkCreateDescriptorSetLayout(tcv_dev, &dlci, NULL, &P->dsl));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TCV_MAXB }, { VK_DESCRIPTOR_TYPE_SAMPLER, TCV_MAXB } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
    TCV_VK(vkCreateDescriptorPool(tcv_dev, &dpi, NULL, &P->pool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = P->pool, .descriptorSetCount = 1, .pSetLayouts = &P->dsl };
    TCV_VK(vkAllocateDescriptorSets(tcv_dev, &dai, &P->set));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes ? push_bytes : 4 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &P->dsl, .pushConstantRangeCount = push_bytes ? 1u : 0u, .pPushConstantRanges = &pcr };
    TCV_VK(vkCreatePipelineLayout(tcv_dev, &plci, NULL, &P->pl));
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof VERT_SPV, .pCode = VERT_SPV };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fs_bytes, .pCode = fs };
    TCV_VK(vkCreateShaderModule(tcv_dev, &smv, NULL, &P->vs)); TCV_VK(vkCreateShaderModule(tcv_dev, &smf, NULL, &P->fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = P->fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, TCV_W, TCV_H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { TCV_W, TCV_H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkFormat cf = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = P->pl };
    TCV_VK(vkCreateGraphicsPipelines(tcv_dev, VK_NULL_HANDLE, 1, &gpi, NULL, &P->pipe));
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    TCV_VK(vkCreateCommandPool(tcv_dev, &cpi, NULL, &P->cpool));
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = cf, .extent = { TCV_W, TCV_H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    TCV_VK(vkCreateImage(tcv_dev, &imi, NULL, &P->rimg));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(tcv_dev, P->rimg, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = tcv_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    TCV_VK(vkAllocateMemory(tcv_dev, &mai, NULL, &P->rmem)); TCV_VK(vkBindImageMemory(tcv_dev, P->rimg, P->rmem, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = P->rimg, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = cf, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    TCV_VK(vkCreateImageView(tcv_dev, &vci, NULL, &P->rview));
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = TCV_W * TCV_H * 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    TCV_VK(vkCreateBuffer(tcv_dev, &bci, NULL, &P->buf)); vkGetBufferMemoryRequirements(tcv_dev, P->buf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = tcv_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    TCV_VK(vkAllocateMemory(tcv_dev, &mai, NULL, &P->bmem)); TCV_VK(vkBindBufferMemory(tcv_dev, P->buf, P->bmem, 0));
    return 0;
}
static void tcv_pipe_destroy(tcv_pipe_t *P) {
    vkDestroyPipeline(tcv_dev, P->pipe, NULL); vkDestroyPipelineLayout(tcv_dev, P->pl, NULL); vkDestroyDescriptorPool(tcv_dev, P->pool, NULL); vkDestroyDescriptorSetLayout(tcv_dev, P->dsl, NULL);
    vkDestroyShaderModule(tcv_dev, P->vs, NULL); vkDestroyShaderModule(tcv_dev, P->fs, NULL); vkDestroyCommandPool(tcv_dev, P->cpool, NULL);
    vkDestroyBuffer(tcv_dev, P->buf, NULL); vkFreeMemory(tcv_dev, P->bmem, NULL); vkDestroyImageView(tcv_dev, P->rview, NULL); vkDestroyImage(tcv_dev, P->rimg, NULL); vkFreeMemory(tcv_dev, P->rmem, NULL);
}

/* One draw: views[i] / smps[i] feed binding i (the one that matches is_sampler). push = push_bytes bytes (or NULL). out = TCV_W * TCV_H * 4 floats (cleared to -7 first). */
static int tcv_draw(tcv_pipe_t *P, const VkImageView *views, const VkSampler *smps, const void *push, float *out) {
    VkDescriptorImageInfo dii[TCV_MAXB]; VkWriteDescriptorSet wr[TCV_MAXB];
    for (uint32_t i = 0; i < P->nb; i++) {
        dii[i] = P->is_sampler[i] ? (VkDescriptorImageInfo){ .sampler = smps[i] } : (VkDescriptorImageInfo){ .imageView = views[i], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = P->set, .dstBinding = P->binding[i], .descriptorCount = 1,
            .descriptorType = P->is_sampler[i] ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &dii[i] };
    }
    vkUpdateDescriptorSets(tcv_dev, P->nb, wr, 0, NULL);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = P->cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; TCV_VK(vkAllocateCommandBuffers(tcv_dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    TCV_VK(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier c1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = P->rimg,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &c1);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = P->rview, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { -7.0f, -7.0f, -7.0f, -7.0f } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { TCV_W, TCV_H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P->pl, 0, 1, &P->set, 0, NULL);
    if (P->push_bytes) vkCmdPushConstants(cmd, P->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, P->push_bytes, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier c2 = c1; c2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; c2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    c2.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; c2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &c2);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TCV_W, TCV_H, 1 } };
    vkCmdCopyImageToBuffer(cmd, P->rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P->buf, 1, &bic);
    TCV_VK(vkEndCommandBuffer(cmd));
    if (tcv_submit_wait(cmd)) return 1;
    float *px; TCV_VK(vkMapMemory(tcv_dev, P->bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
    memcpy(out, px, TCV_W * TCV_H * 16);
    vkUnmapMemory(tcv_dev, P->bmem);
    vkFreeCommandBuffers(tcv_dev, P->cpool, 1, &cmd);
    return 0;
}
#endif
