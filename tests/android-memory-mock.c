/*
 * Host-only regression: compile the actual pipeline translation unit into
 * this test, with section GC dropping unrelated pipeline creation code.
 * No Vulkan loader/device is needed. Every Vulkan function reached by the
 * tested paths is implemented here; an unexpected unresolved call fails link.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../src/vulkan/pipeline.c"

enum { COHERENT, NONCOHERENT, INVISIBLE };
enum { OK, MAP_FAIL, FLUSH_FAIL, ADDRESS_ZERO };
static int mode, fault, created, allocated, bound, mapped, flushed, unmapped, destroyed, freed, errors;
static unsigned char bytes[256];
static VkBuffer const fake_buffer=(VkBuffer)(uintptr_t)0x101;
static VkDeviceMemory const fake_memory=(VkDeviceMemory)(uintptr_t)0x102;
static VkDevice const fake_device=(VkDevice)(uintptr_t)0x103;
static VkPhysicalDevice const fake_phys=(VkPhysicalDevice)(uintptr_t)0x104;
#define EXPECT(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); errors++; } } while(0)
static void reset(int m,int f){
    mode=m; fault=f; created=allocated=bound=mapped=flushed=unmapped=destroyed=freed=0;
    memset(bytes,0xa5,sizeof bytes);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p,VkPhysicalDeviceMemoryProperties *o){
    EXPECT(p==fake_phys);memset(o,0,sizeof *o);o->memoryTypeCount=2;
    o->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    o->memoryTypes[1].propertyFlags=mode==INVISIBLE?VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|(mode==COHERENT?VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:0);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice d,const VkBufferCreateInfo *i,const VkAllocationCallbacks *a,VkBuffer *o){
    EXPECT(d==fake_device && i && i->size>0 && !a);created++;*o=fake_buffer;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice d,VkBuffer b,VkMemoryRequirements *o){
    EXPECT(d==fake_device && b==fake_buffer && created==1);
    *o=(VkMemoryRequirements){.size=sizeof bytes,.alignment=16,.memoryTypeBits=3};
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *i,const VkAllocationCallbacks *a,VkDeviceMemory *o){
    EXPECT(d==fake_device && i->memoryTypeIndex==1 && mode!=INVISIBLE && i->allocationSize==sizeof bytes && !a);
    allocated++;*o=fake_memory;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){
    EXPECT(d==fake_device && b==fake_buffer && m==fake_memory && o==0 && allocated==1);bound++;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice d,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize n,VkMemoryMapFlags f,void **p){
    EXPECT(d==fake_device && m==fake_memory && o==0 && (n==VK_WHOLE_SIZE||n==64) && f==0 && bound==1);
    mapped++;if(fault==MAP_FAIL){*p=NULL;return VK_ERROR_MEMORY_MAP_FAILED;}*p=bytes;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice d,uint32_t n,const VkMappedMemoryRange *r){
    EXPECT(d==fake_device && n==1 && r->memory==fake_memory && r->offset==0 && r->size==VK_WHOLE_SIZE && mapped==1 && !unmapped && mode==NONCOHERENT);
    flushed++;return fault==FLUSH_FAIL?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice d,VkDeviceMemory m){
    EXPECT(d==fake_device && m==fake_memory && mapped==1 && fault!=MAP_FAIL && !freed);unmapped++;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks *a){
    EXPECT(d==fake_device && b==fake_buffer && !a && !destroyed && !freed);destroyed++;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks *a){
    EXPECT(d==fake_device && m==fake_memory && !a && destroyed==1 && !freed);
    EXPECT(!mapped || fault==MAP_FAIL || unmapped==1);freed++;
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice d,const VkBufferDeviceAddressInfo *i){
    EXPECT(d==fake_device && i->buffer==fake_buffer && unmapped==1);
    return fault==ADDRESS_ZERO?0:0x123400;
}
/* Fallback UBO tests stop before descriptor creation. These stubs must
 * never execute; retaining them makes that expectation executable. */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d,const VkDescriptorPoolCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorPool *o){
    (void)d;(void)i;(void)a;(void)o;EXPECT(0);return VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *o){
    (void)d;(void)i;(void)o;EXPECT(0);return VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t c,const VkCopyDescriptorSet *s){
    (void)d;(void)n;(void)w;(void)c;(void)s;EXPECT(0);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice d,const VkSamplerCreateInfo *i,const VkAllocationCallbacks *a,VkSampler *o){
    (void)d;(void)i;(void)a;(void)o;EXPECT(0);return VK_ERROR_INITIALIZATION_FAILED;
}
static void run_upload(int kind,int m,int f){
    struct lagfx_vk_state v={.device=fake_device,.phys_device=fake_phys,.have_buffer_device_address=true};
    VkBuffer b=VK_NULL_HANDLE;VkDeviceMemory mem=VK_NULL_HANDLE;VkDeviceAddress addr=0;
    const unsigned char data[]={1,17,129,255};
    reset(m,f);int prior=errors;
    lagfx_status_t st=kind==0?lagfx_vk_make_host_storage_buffer(&v,data,sizeof data,&b,&mem):
        kind==1?lagfx_vk_make_host_storage_buffer_padded(&v,data,sizeof data,16,&b,&mem):
        lagfx_vk_make_device_address_buffer(&v,data,sizeof data,&b,&mem,&addr);
    int success=m!=INVISIBLE&&f==OK;
    EXPECT(st==(success?LAGFX_OK:LAGFX_ERR_BACKEND));
    EXPECT(created==1);
    EXPECT(allocated==(m!=INVISIBLE));
    EXPECT(mapped==(m!=INVISIBLE));
    EXPECT(flushed==(m==NONCOHERENT && f!=MAP_FAIL));
    EXPECT(unmapped==(m!=INVISIBLE&&f!=MAP_FAIL));
    if(success){
        EXPECT(b==fake_buffer&&mem==fake_memory);
        EXPECT(!memcmp(bytes,data,sizeof data));
        if(kind==1)for(unsigned i=sizeof data;i<16;i++)EXPECT(bytes[i]==0);
        if(kind==2)EXPECT(addr==0x123400);
        vkDestroyBuffer(v.device,b,NULL);vkFreeMemory(v.device,mem,NULL);
    }else{
        EXPECT(!b&&!mem&&!addr);
    }
    EXPECT(destroyed==1&&freed==(m!=INVISIBLE));
    printf("%s: helper=%d memory=%d fault=%d (MOCK)\n",errors==prior?"PASS":"FAIL",kind,m,f);
}
static void run_fallback(int m,int f){
    struct lagfx_vk_state v={.device=fake_device,.phys_device=fake_phys};
    reset(m,f);int prior=errors;
    EXPECT(create_fallback_ds(&v)==LAGFX_ERR_BACKEND);
    EXPECT(!v.fallback_ubo&&!v.fallback_ubo_mem);
    EXPECT(created==1&&destroyed==1);
    EXPECT(allocated==(m==COHERENT)&&freed==allocated);
    EXPECT(mapped==allocated&&!unmapped&&!flushed);
    printf("%s: fallback UBO memory=%d fault=%d (MOCK)\n",errors==prior?"PASS":"FAIL",m,f);
}
int main(void){
    for(int k=0;k<3;k++){
        run_upload(k,COHERENT,OK);
        run_upload(k,NONCOHERENT,OK);
        run_upload(k,INVISIBLE,OK);
        run_upload(k,COHERENT,MAP_FAIL);
        run_upload(k,NONCOHERENT,FLUSH_FAIL);
    }
    run_upload(2,COHERENT,ADDRESS_ZERO);
    run_fallback(COHERENT,MAP_FAIL);
    run_fallback(NONCOHERENT,OK);
    run_fallback(INVISIBLE,OK);
    printf("MOCK SUMMARY cases=19 errors=%d (no real Vulkan device)\n",errors);
    return errors?1:0;
}
