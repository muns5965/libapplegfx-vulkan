/* Direct cursor path test, without Vulkan loader or real GPU. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/vulkan/cursor.c"
void lagfx_log_impl(const char *fmt, ...) {(void)fmt;}
void lagfx_warn_impl(const char *fmt, ...) {(void)fmt;}
void lagfx_err_impl(const char *fmt, ...) {(void)fmt;}
void lagfx_trace_impl(const char *fmt, ...) {(void)fmt;}

#define H(T,n) ((T)(uintptr_t)(n))
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); errors++; } } while (0)
static int errors, maps, unmaps, flushes, submits, waits, idles, frees, destroys, draws, descriptors, barriers;
static bool pending;
static VkResult map_rc, flush_rc, wait_rc, idle_rc, submit_rc;
static VkImageLayout expected_old;
static unsigned char data[128];
static void reset(void) {
    maps=unmaps=flushes=submits=waits=idles=frees=destroys=draws=descriptors=barriers=0;
    pending=false; map_rc=flush_rc=wait_rc=idle_rc=submit_rc=VK_SUCCESS;
    expected_old=VK_IMAGE_LAYOUT_PREINITIALIZED; memset(data,0xa5,sizeof data);
}
static struct lagfx_vk_state state(bool coherent) {
    struct lagfx_vk_state v={0};
    v.initialized=true; v.device=H(VkDevice,1); v.graphics_queue=H(VkQueue,2);
    v.cursor_glyph_image=H(VkImage,3); v.cursor_glyph_mem=H(VkDeviceMemory,4);
    v.cursor_ubo_mem=H(VkDeviceMemory,5); v.cursor_glyph_view=H(VkImageView,6);
    v.cursor_pipeline=H(VkPipeline,7); v.cursor_sampler=H(VkSampler,8);
    v.cursor_desc_set=H(VkDescriptorSet,9); v.cursor_layout=H(VkPipelineLayout,10);
    v.cursor_glyph_coherent=coherent; v.cursor_glyph_w=2; v.cursor_glyph_h=2;
    v.cursor_glyph_layout=VK_IMAGE_LAYOUT_PREINITIALIZED;
    return v;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice d,VkImage i,const VkImageSubresource *s,VkSubresourceLayout *l) {
    CHECK(d==H(VkDevice,1)&&i==H(VkImage,3)&&s->aspectMask==VK_IMAGE_ASPECT_COLOR_BIT);
    *l=(VkSubresourceLayout){.offset=8,.size=32,.rowPitch=16};
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags f,void **p) {
    CHECK(d==H(VkDevice,1)&&o==0&&f==0&&!pending);
    CHECK((m==H(VkDeviceMemory,4)&&n==VK_WHOLE_SIZE)||(m==H(VkDeviceMemory,5)&&n==16));
    maps++; *p=map_rc==VK_SUCCESS?data:NULL; return map_rc;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d,VkDeviceMemory m) {
    CHECK(d==H(VkDevice,1)&&(m==H(VkDeviceMemory,4)||m==H(VkDeviceMemory,5))&&maps>unmaps);
    unmaps++;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice d,uint32_t n,const VkMappedMemoryRange *r) {
    CHECK(d==H(VkDevice,1)&&n==1&&r->memory==H(VkDeviceMemory,4)&&r->offset==0&&r->size==VK_WHOLE_SIZE&&maps>unmaps);
    flushes++; return flush_rc;
}
lagfx_status_t lagfx_vk_cmdbuf_alloc(struct lagfx_vk_state *v,VkCommandBuffer *o) {
    CHECK(v->device==H(VkDevice,1)&&!pending); *o=H(VkCommandBuffer,11); return LAGFX_OK;
}
void lagfx_vk_cmdbuf_free(struct lagfx_vk_state *v,VkCommandBuffer c) {
    CHECK(v->device==H(VkDevice,1)&&c==H(VkCommandBuffer,11)&&!pending); frees++;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *i) {
    CHECK(c==H(VkCommandBuffer,11)&&i->flags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c) {CHECK(c==H(VkCommandBuffer,11));return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,uint32_t n,const VkMemoryBarrier *m,uint32_t bn,const VkBufferMemoryBarrier *b,uint32_t in,const VkImageMemoryBarrier *i) {
    (void)f;(void)m;(void)b;
    CHECK(c==H(VkCommandBuffer,11)&&s==VK_PIPELINE_STAGE_HOST_BIT&&d==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT&&n==0&&bn==0&&in==1);
    CHECK(i->oldLayout==expected_old&&i->newLayout==VK_IMAGE_LAYOUT_GENERAL&&i->srcAccessMask==VK_ACCESS_HOST_WRITE_BIT);barriers++;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice d,const VkFenceCreateInfo *i,const VkAllocationCallbacks *a,VkFence *o) {
    CHECK(d==H(VkDevice,1)&&i&&!a);*o=H(VkFence,12);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f) {
    CHECK(q==H(VkQueue,2)&&n==1&&i->commandBufferCount==1&&f==H(VkFence,12)&&!pending);
    submits++;pending=submit_rc==VK_SUCCESS;return submit_rc;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t ns) {
    CHECK(d==H(VkDevice,1)&&n==1&&*f==H(VkFence,12)&&all&&ns>0&&pending);
    waits++;if(wait_rc==VK_SUCCESS)pending=false;return wait_rc;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice d) {
    CHECK(d==H(VkDevice,1)&&pending);idles++;if(idle_rc==VK_SUCCESS||idle_rc==VK_ERROR_DEVICE_LOST)pending=false;return idle_rc;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice d,VkFence f,const VkAllocationCallbacks *a) {
    CHECK(d==H(VkDevice,1)&&f==H(VkFence,12)&&!a&&!pending);destroys++;
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t c,const VkCopyDescriptorSet *s) {
    (void)s;CHECK(d==H(VkDevice,1)&&n==1&&c==0&&w->pImageInfo->imageLayout==VK_IMAGE_LAYOUT_GENERAL&&!pending);descriptors++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p){(void)c;(void)b;(void)p;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t f,uint32_t n,const VkDescriptorSet *s,uint32_t dn,const uint32_t *o){(void)c;(void)b;(void)l;(void)f;(void)n;(void)s;(void)dn;(void)o;}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c,uint32_t f,uint32_t n,const VkViewport *v){(void)c;(void)f;(void)n;(void)v;}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c,uint32_t f,uint32_t n,const VkRect2D *r){(void)c;(void)f;(void)n;(void)r;}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer c,uint32_t v,uint32_t i,uint32_t f,uint32_t b){(void)c;(void)f;(void)b;CHECK(v==4&&i==1);draws++;}
static lagfx_status_t upload(struct lagfx_vk_state *v) {
    const uint8_t px[]={1,2,3,4};return lagfx_vk_cursor_upload_glyph(v,px,1,1,4);
}
static lagfx_status_t draw(struct lagfx_vk_state *v) {
    return lagfx_vk_cursor_draw(v,H(VkCommandBuffer,11),64,64,16,16,0,0);
}
static void result(const char *name,int before) {printf("%s: %s (MOCK)\n",before==errors?"PASS":"FAIL",name);}
int main(void){
    struct lagfx_vk_state v;int before;
    reset();before=errors;v=state(true);v.cursor_glyph_valid=true;map_rc=VK_ERROR_MEMORY_MAP_FAILED;
    CHECK(draw(&v)!=LAGFX_OK&&maps==1&&!draws&&!unmaps&&!v.cursor_sync_failed);result("cursor UBO map failure no draw",before);
    reset();before=errors;v=state(true);v.cursor_glyph_valid=true;map_rc=VK_ERROR_DEVICE_LOST;
    CHECK(draw(&v)!=LAGFX_OK&&v.cursor_sync_failed&&!draws);result("cursor UBO device lost",before);
    reset();before=errors;v=state(false);flush_rc=VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(upload(&v)!=LAGFX_OK&&flushes==1&&unmaps==1&&!submits&&!v.cursor_glyph_valid&&!v.cursor_sync_failed);result("glyph flush failure unmap no submit",before);
    reset();before=errors;v=state(true);
    CHECK(upload(&v)==LAGFX_OK&&v.cursor_glyph_valid&&v.cursor_glyph_layout==VK_IMAGE_LAYOUT_GENERAL&&!flushes&&descriptors==1&&destroys==1&&frees==1);
    CHECK(data[8]==1&&data[11]==4&&data[12]==0&&data[24]==0&&data[16]==0xa5);result("coherent glyph row pitch and layout",before);
    reset();before=errors;v=state(false);
    CHECK(upload(&v)==LAGFX_OK&&flushes==1&&unmaps==1);
    expected_old=VK_IMAGE_LAYOUT_GENERAL;
    CHECK(upload(&v)==LAGFX_OK&&flushes==2&&barriers==2&&v.cursor_glyph_valid);result("noncoherent glyph repeated GENERAL upload",before);
    reset();before=errors;v=state(false);wait_rc=VK_TIMEOUT;
    CHECK(upload(&v)!=LAGFX_OK&&idles==1&&destroys==1&&frees==1&&!pending&&!v.cursor_sync_failed&&!v.cursor_glyph_valid&&v.cursor_glyph_layout==VK_IMAGE_LAYOUT_GENERAL);
    wait_rc=VK_SUCCESS;expected_old=VK_IMAGE_LAYOUT_GENERAL;
    CHECK(upload(&v)==LAGFX_OK&&v.cursor_glyph_valid);result("timeout idle success then retry",before);
    reset();before=errors;v=state(false);wait_rc=VK_TIMEOUT;idle_rc=VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(upload(&v)!=LAGFX_OK&&idles==1&&pending&&!destroys&&!frees&&v.cursor_sync_failed);
    CHECK(upload(&v)!=LAGFX_OK&&maps==1&&draw(&v)!=LAGFX_OK&&!draws);result("unconfirmed GPU completion blocks reuse",before);
    reset();before=errors;v=state(false);wait_rc=VK_TIMEOUT;idle_rc=VK_ERROR_DEVICE_LOST;
    CHECK(upload(&v)!=LAGFX_OK&&v.cursor_sync_failed&&!v.cursor_glyph_valid&&idles==1&&destroys==1&&frees==1);result("idle device lost stays latched",before);
    reset();before=errors;v=state(false);submit_rc=VK_ERROR_DEVICE_LOST;
    CHECK(upload(&v)!=LAGFX_OK&&v.cursor_sync_failed&&!waits&&!idles&&!v.cursor_glyph_valid);result("submit device lost stays latched",before);
    printf("CURSOR MOCK SUMMARY cases=9 errors=%d\n",errors);return errors?1:0;
}
