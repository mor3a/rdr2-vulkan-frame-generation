#include "native_hud.h"
#include "config.h"
#include "slboot.h"
#include <cassert>
#include <cstring>
#include <cstdio>
namespace {
template<class H>H Handle(uintptr_t n){return (H)n;}
fgvk::Config config;fgvk::Runtime runtime;fgvk::SlFns slFns;
uintptr_t serial=100;unsigned imageCopies=0,barriers=0,liveImages=0,liveMemory=0,liveViews=0,tagCalls=0;
bool recentSR=true;
bool failAllocation=false,lastTagNull=false,failTag=false;unsigned failAfter=0,allocationAttempts=0;sl::Resource lastResource;uint32_t taggedToken=0;
VkImage copiedSource{},copiedDestination{};
VKAPI_ATTR VkResult VKAPI_CALL MockImage(VkDevice,const VkImageCreateInfo* c,const VkAllocationCallbacks*,VkImage* o){assert(c->usage==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));if(failAllocation && allocationAttempts++>=failAfter)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*o=Handle<VkImage>(++serial);++liveImages;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL MockDestroyImage(VkDevice,VkImage,const VkAllocationCallbacks*){--liveImages;}
VKAPI_ATTR void VKAPI_CALL MockRequirements(VkDevice,VkImage,VkMemoryRequirements* r){r->size=1920*1080*4;r->memoryTypeBits=1;}
VKAPI_ATTR VkResult VKAPI_CALL MockMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* o){*o=Handle<VkDeviceMemory>(++serial);++liveMemory;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL MockFree(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*){--liveMemory;}
VKAPI_ATTR VkResult VKAPI_CALL MockBind(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL MockView(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* o){*o=Handle<VkImageView>(++serial);++liveViews;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL MockDestroyView(VkDevice,VkImageView,const VkAllocationCallbacks*){--liveViews;}
VKAPI_ATTR void VKAPI_CALL MockCopy(VkCommandBuffer,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageCopy* r){assert(sl==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && dl==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && n==1);assert(r->extent.width==1920 && r->extent.height==1080);copiedSource=src;copiedDestination=dst;++imageCopies;}
VKAPI_ATTR void VKAPI_CALL MockBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t n,const VkImageMemoryBarrier* b){assert(n==2);assert(b[0].srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);if(barriers%2==0){assert(b[0].oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && b[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);assert(b[1].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && b[1].newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);}else{assert(b[0].oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && b[0].newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);assert(b[1].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}++barriers;}
VKAPI_ATTR void VKAPI_CALL MockMemProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties* p){p->memoryTypeCount=1;p->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;}
VKAPI_ATTR void VKAPI_CALL MockFormat(VkPhysicalDevice,VkFormat,VkFormatProperties* p){p->optimalTilingFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;}
VKAPI_ATTR void VKAPI_CALL MockQueues(VkPhysicalDevice,uint32_t* n,VkQueueFamilyProperties* q){if(!q){*n=2;return;}q[0].queueCount=q[1].queueCount=1;}
VKAPI_ATTR VkResult VKAPI_CALL MockPass(VkDevice,const VkRenderPassCreateInfo*,const VkAllocationCallbacks*,VkRenderPass* o){*o=Handle<VkRenderPass>(++serial);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL MockFramebuffer(VkDevice,const VkFramebufferCreateInfo*,const VkAllocationCallbacks*,VkFramebuffer* o){*o=Handle<VkFramebuffer>(++serial);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL MockBegin(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents){}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Driver(VkDevice,const char* n){
#define RESOLVE(name,fn) if(!strcmp(n,name))return (PFN_vkVoidFunction)fn;
 RESOLVE("vkCreateImage",MockImage) RESOLVE("vkDestroyImage",MockDestroyImage) RESOLVE("vkGetImageMemoryRequirements",MockRequirements)
 RESOLVE("vkAllocateMemory",MockMemory) RESOLVE("vkFreeMemory",MockFree) RESOLVE("vkBindImageMemory",MockBind)
 RESOLVE("vkCreateImageView",MockView) RESOLVE("vkDestroyImageView",MockDestroyView) RESOLVE("vkCmdCopyImage",MockCopy) RESOLVE("vkCmdPipelineBarrier",MockBarrier)
 return nullptr;
#undef RESOLVE
}
sl::Result Tag(const sl::FrameToken& frame,const sl::ViewportHandle&,const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* cmd){assert(n==1 && !cmd && t->type==sl::kBufferTypeHUDLessColor && t->lifecycle==sl::ResourceLifecycle::eValidUntilPresent);lastTagNull=!t->resource;if(!lastTagNull){lastResource=*t->resource;assert(t->extent.width==1920 && t->extent.height==1080);}taggedToken=(uint32_t)frame;++tagCalls;return failTag && !lastTagNull?sl::Result::eErrorInvalidParameter:sl::Result::eOk;}
struct Token:sl::FrameToken {operator uint32_t()const override{return 42;}} token;
}
namespace fgvk {
bool HasRecentDLSSInput(){return recentSR;}
const Config& Cfg(){return config;}Runtime& Rt(){return runtime;}SlFns& GetSlFns(){return slFns;}
void Log(const char*,...){}bool DebugLoggingEnabled(){return false;}
void* LoaderFn(const char* n){if(!strcmp(n,"vkGetPhysicalDeviceMemoryProperties"))return (void*)MockMemProperties;if(!strcmp(n,"vkGetPhysicalDeviceFormatProperties"))return (void*)MockFormat;if(!strcmp(n,"vkGetPhysicalDeviceQueueFamilyProperties"))return (void*)MockQueues;return nullptr;}
}
int main(){
 using namespace fgvk;slFns.setTagForFrame=Tag;
 auto d=Handle<VkDevice>(1);auto pd=Handle<VkPhysicalDevice>(2);auto sc=Handle<VkSwapchainKHR>(3);auto cmd=Handle<VkCommandBuffer>(4);
 VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.imageFormat=VK_FORMAT_B8G8R8A8_UNORM;ci.imageExtent={1920,1080};ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.imageArrayLayers=1;
 NativeHudRegister(d,pd,sc,&ci,Driver);VkImage images[2]{Handle<VkImage>(11),Handle<VkImage>(12)};NativeHudImages(d,sc,2,images);NativeHudImages(d,sc,2,images);assert(liveImages==2 && liveMemory==2 && liveViews==2);
 auto createView=(PFN_vkCreateImageView)NativeHudWrap("vkCreateImageView",(PFN_vkVoidFunction)MockView);
 VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=images[0];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ci.imageFormat;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view{};assert(createView(d,&vi,nullptr,&view)==VK_SUCCESS);
 auto createPass=(PFN_vkCreateRenderPass)NativeHudWrap("vkCreateRenderPass",(PFN_vkVoidFunction)MockPass);
 VkAttachmentDescription a{};a.format=ci.imageFormat;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
 VkAttachmentReference ar{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.colorAttachmentCount=1;sub.pColorAttachments=&ar;
 VkRenderPassCreateInfo pci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pci.attachmentCount=1;pci.pAttachments=&a;pci.subpassCount=1;pci.pSubpasses=&sub;VkRenderPass pass{};assert(createPass(d,&pci,nullptr,&pass)==VK_SUCCESS);
 auto createFB=(PFN_vkCreateFramebuffer)NativeHudWrap("vkCreateFramebuffer",(PFN_vkVoidFunction)MockFramebuffer);
 VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbci.width=1920;fbci.height=1080;fbci.layers=1;fbci.attachmentCount=1;fbci.pAttachments=&view;fbci.renderPass=pass;VkFramebuffer fb{};assert(createFB(d,&fbci,nullptr,&fb)==VK_SUCCESS);
 auto begin=(PFN_vkCmdBeginRenderPass)NativeHudWrap("vkCmdBeginRenderPass",(PFN_vkVoidFunction)MockBegin);
 VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};bi.renderPass=pass;bi.framebuffer=fb;bi.renderArea.extent=ci.imageExtent;
 uint32_t index=0;VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.swapchainCount=1;pi.pSwapchains=&sc;pi.pImageIndices=&index;
 NativeHudAcquire(sc,0);begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);assert(imageCopies==1 && barriers==2 && copiedSource==images[0]);
 NativeHudTag(&pi,token,42);assert(lastTagNull); // copy recorded but not submitted
 NativeHudSubmit(1,&cmd,42);NativeHudTag(&pi,token,42);assert(!lastTagNull && taggedToken==42 && lastResource.native==(void*)copiedDestination && lastResource.view && lastResource.memory && lastResource.nativeFormat==ci.imageFormat && lastResource.state==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 NativeHudTag(&pi,token,43);assert(lastTagNull);index=1;NativeHudTag(&pi,token,42);assert(lastTagNull);index=0;
 NativeHudAcquire(sc,0);NativeHudTag(&pi,token,42);assert(lastTagNull);NativeHudForget(cmd);
 runtime.fgUserOff=true;begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);assert(imageCopies==1);runtime.fgUserOff=false;
 recentSR=false;begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);assert(imageCopies==1);recentSR=true;
 bi.renderArea.extent.width=960;begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);assert(imageCopies==1);bi.renderArea.extent.width=1920;
 begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);assert(imageCopies==2);NativeHudForget(cmd);NativeHudSubmit(1,&cmd,42);NativeHudTag(&pi,token,42);assert(lastTagNull);
 begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);NativeHudSubmit(1,&cmd,42);NativeHudTag(&pi,token,42);assert(imageCopies==3 && !lastTagNull);
 failTag=true;NativeHudTag(&pi,token,42);assert(lastTagNull);failTag=false;
 NativeHudDestroy(d,sc);assert(liveImages==0 && liveMemory==0 && liveViews==1);NativeHudTag(&pi,token,42);assert(lastTagNull);
 failAllocation=true;NativeHudRegister(d,pd,sc,&ci,Driver);NativeHudImages(d,sc,2,images);NativeHudAcquire(sc,0);begin(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);assert(imageCopies==3 && liveImages==0 && liveMemory==0);NativeHudTag(&pi,token,42);assert(lastTagNull);NativeHudDestroy(d,sc);
 failAfter=1;allocationAttempts=0;NativeHudRegister(d,pd,sc,&ci,Driver);NativeHudImages(d,sc,2,images);assert(liveImages==0 && liveMemory==0 && liveViews==1);NativeHudDestroy(d,sc);
 config.provider=FGProvider::XeSS;assert(NativeHudWrap("vkCreateImageView",(PFN_vkVoidFunction)MockView)==(PFN_vkVoidFunction)MockView);auto oldTags=tagCalls;NativeHudTag(&pi,token,42);assert(tagCalls==oldTags);
 puts("Native HUD production path with mocked Vulkan/SDK: correct format/layout/copy, one copy per image epoch, exact submitted-frame tags, null on mismatch/reset/reuse/teardown, FG-off no copy, allocation failure cleanup, XeSS untouched: PASS");
}
