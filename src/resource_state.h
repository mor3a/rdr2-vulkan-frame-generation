#pragma once
#include <vulkan/vulkan.h>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include "config.h"
namespace fgvk {
void HudCaptureForgetCommand(VkCommandBuffer);
void ForgetFGCommand(VkCommandBuffer);
void ExecuteFGCommands(VkCommandBuffer,uint32_t,const VkCommandBuffer*);
struct ImageState {
  VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageUsageFlags usage=0;
  VkFormat format=VK_FORMAT_UNDEFINED;
  uint32_t width=0,height=0;
};
namespace resource_detail {
inline std::mutex mutex;
inline std::unordered_map<VkImage,ImageState> images;
struct ObservedLayout {VkImageLayout layout;VkImageAspectFlags aspects;};
inline std::unordered_map<VkCommandBuffer,std::unordered_map<VkImage,ObservedLayout>> commands;
inline std::unordered_map<VkImage,ObservedLayout> submitted;
inline PFN_vkCreateImage createImage=nullptr;
inline PFN_vkDestroyImage destroyImage=nullptr;
inline PFN_vkBeginCommandBuffer beginCommand=nullptr;
inline PFN_vkResetCommandBuffer resetCommand=nullptr;
inline PFN_vkFreeCommandBuffers freeCommands=nullptr;
inline PFN_vkCmdPipelineBarrier barrier=nullptr;
inline PFN_vkCmdPipelineBarrier2 barrier2=nullptr,barrier2KHR=nullptr;
inline PFN_vkCmdBeginRenderPass beginPass=nullptr;
inline PFN_vkCmdEndRenderPass endPass=nullptr;
inline PFN_vkCmdBeginRenderPass2 beginPass2=nullptr,beginPass2KHR=nullptr;
inline PFN_vkCmdEndRenderPass2 endPass2=nullptr,endPass2KHR=nullptr;
inline PFN_vkCmdExecuteCommands executeCommands=nullptr;
inline PFN_vkCmdBeginRendering beginRendering=nullptr,beginRenderingKHR=nullptr;
inline PFN_vkCmdEndRendering endRendering=nullptr,endRenderingKHR=nullptr;
}
inline bool ReadableLayout(VkImageLayout l){
  return l==VK_IMAGE_LAYOUT_GENERAL || l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
      l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || l==VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
      l==VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
}
inline void ForgetCommand(VkCommandBuffer c){
  std::lock_guard<std::mutex> lock(resource_detail::mutex);
  resource_detail::commands.erase(c);
}
inline void ObserveBarrier(VkCommandBuffer c,VkImage image,VkImageLayout layout,const VkImageSubresourceRange& range){
  std::lock_guard<std::mutex> lock(resource_detail::mutex);
  auto& states=resource_detail::commands[c];
  // NGX input views accepted by this port refer to mip 0 / layer 0 only.
  if(range.baseMipLevel!=0 || range.baseArrayLayer!=0 || !range.levelCount || !range.layerCount){ states.erase(image);return; }
  states[image]={layout,range.aspectMask};
}
inline bool FindImageState(VkCommandBuffer c,VkImage image,ImageState& result,VkImageAspectFlags expectedAspects=0){
  std::lock_guard<std::mutex> lock(resource_detail::mutex);
  auto i=resource_detail::images.find(image);
  auto cmd=resource_detail::commands.find(c);
  if(i==resource_detail::images.end())return false;
  const resource_detail::ObservedLayout* state=nullptr;
  if(cmd!=resource_detail::commands.end()){auto l=cmd->second.find(image);if(l!=cmd->second.end())state=&l->second;}
  if(!state){auto l=resource_detail::submitted.find(image);if(l!=resource_detail::submitted.end())state=&l->second;}
  if(!state || (expectedAspects && (state->aspects&expectedAspects)!=expectedAspects))return false;
  result=i->second;result.layout=state->layout;return true;
}
inline void CommitCommands(uint32_t n,const VkCommandBuffer* cs){
  std::lock_guard<std::mutex> lock(resource_detail::mutex);
  for(uint32_t i=0;i<n;++i){auto c=resource_detail::commands.find(cs[i]);if(c!=resource_detail::commands.end())for(const auto& p:c->second)resource_detail::submitted[p.first]=p.second;}
}
inline VKAPI_ATTR VkResult VKAPI_CALL TrackCreateImage(VkDevice d,const VkImageCreateInfo* ci,const VkAllocationCallbacks* a,VkImage* image){
  VkResult r=resource_detail::createImage(d,ci,a,image);
  if(r==VK_SUCCESS && image && ci){
    std::lock_guard<std::mutex> lock(resource_detail::mutex);
    if(!IsDX12Bridge(Cfg().provider) || !resource_detail::images.count(*image))
      resource_detail::images[*image]={ci->initialLayout,ci->usage,ci->format,ci->extent.width,ci->extent.height};
  }return r;
}
inline VKAPI_ATTR void VKAPI_CALL TrackDestroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks* a){
  {std::lock_guard<std::mutex> lock(resource_detail::mutex);resource_detail::images.erase(image);resource_detail::submitted.erase(image);
   for(auto& pair:resource_detail::commands)pair.second.erase(image);}
  resource_detail::destroyImage(d,image,a);
}
inline VKAPI_ATTR VkResult VKAPI_CALL TrackBegin(VkCommandBuffer c,const VkCommandBufferBeginInfo* bi){
  ForgetCommand(c);ForgetFGCommand(c);HudCaptureForgetCommand(c);return resource_detail::beginCommand(c,bi);
}
inline VKAPI_ATTR VkResult VKAPI_CALL TrackReset(VkCommandBuffer c,VkCommandBufferResetFlags flags){
  ForgetCommand(c);ForgetFGCommand(c);HudCaptureForgetCommand(c);return resource_detail::resetCommand(c,flags);
}
inline VKAPI_ATTR void VKAPI_CALL TrackFree(VkDevice d,VkCommandPool pool,uint32_t n,const VkCommandBuffer* cs){
  for(uint32_t i=0;i<n;i++){ForgetCommand(cs[i]);ForgetFGCommand(cs[i]);HudCaptureForgetCommand(cs[i]);}resource_detail::freeCommands(d,pool,n,cs);
}
inline VKAPI_ATTR void VKAPI_CALL TrackBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,
    VkDependencyFlags dep,uint32_t nm,const VkMemoryBarrier* m,uint32_t nb,const VkBufferMemoryBarrier* b,
    uint32_t ni,const VkImageMemoryBarrier* images){
  resource_detail::barrier(c,src,dst,dep,nm,m,nb,b,ni,images);
  for(uint32_t i=0;i<ni;i++)ObserveBarrier(c,images[i].image,images[i].newLayout,images[i].subresourceRange);
}
inline void ObserveBarrier2(VkCommandBuffer c,const VkDependencyInfo* di){
  if(!di)return;
  for(uint32_t i=0;i<di->imageMemoryBarrierCount;i++){
    const auto& b=di->pImageMemoryBarriers[i];ObserveBarrier(c,b.image,b.newLayout,b.subresourceRange);
  }
}
inline VKAPI_ATTR void VKAPI_CALL TrackBarrier2(VkCommandBuffer c,const VkDependencyInfo* di){resource_detail::barrier2(c,di);ObserveBarrier2(c,di);}
inline VKAPI_ATTR void VKAPI_CALL TrackBarrier2KHR(VkCommandBuffer c,const VkDependencyInfo* di){resource_detail::barrier2KHR(c,di);ObserveBarrier2(c,di);}
// Render passes and dynamic rendering can change layouts implicitly. Discard
// command-local observations rather than claim that an earlier layout still holds.
inline VKAPI_ATTR void VKAPI_CALL TrackBeginPass(VkCommandBuffer c,const VkRenderPassBeginInfo* bi,VkSubpassContents contents){
  ForgetCommand(c);resource_detail::beginPass(c,bi,contents);
}
inline VKAPI_ATTR void VKAPI_CALL TrackEndPass(VkCommandBuffer c){resource_detail::endPass(c);ForgetCommand(c);}
inline VKAPI_ATTR void VKAPI_CALL TrackBeginPass2(VkCommandBuffer c,const VkRenderPassBeginInfo* bi,const VkSubpassBeginInfo* si){ForgetCommand(c);resource_detail::beginPass2(c,bi,si);}
inline VKAPI_ATTR void VKAPI_CALL TrackBeginPass2KHR(VkCommandBuffer c,const VkRenderPassBeginInfo* bi,const VkSubpassBeginInfo* si){ForgetCommand(c);resource_detail::beginPass2KHR(c,bi,si);}
inline VKAPI_ATTR void VKAPI_CALL TrackEndPass2(VkCommandBuffer c,const VkSubpassEndInfo* si){resource_detail::endPass2(c,si);ForgetCommand(c);}
inline VKAPI_ATTR void VKAPI_CALL TrackEndPass2KHR(VkCommandBuffer c,const VkSubpassEndInfo* si){resource_detail::endPass2KHR(c,si);ForgetCommand(c);}
inline VKAPI_ATTR void VKAPI_CALL TrackExecute(VkCommandBuffer c,uint32_t n,const VkCommandBuffer* secondary){ForgetCommand(c);resource_detail::executeCommands(c,n,secondary);ExecuteFGCommands(c,n,secondary);ForgetCommand(c);}
inline VKAPI_ATTR void VKAPI_CALL TrackBeginRendering(VkCommandBuffer c,const VkRenderingInfo* ri){ForgetCommand(c);resource_detail::beginRendering(c,ri);}
inline VKAPI_ATTR void VKAPI_CALL TrackBeginRenderingKHR(VkCommandBuffer c,const VkRenderingInfo* ri){ForgetCommand(c);resource_detail::beginRenderingKHR(c,ri);}
inline VKAPI_ATTR void VKAPI_CALL TrackEndRendering(VkCommandBuffer c){resource_detail::endRendering(c);ForgetCommand(c);}
inline VKAPI_ATTR void VKAPI_CALL TrackEndRenderingKHR(VkCommandBuffer c){resource_detail::endRenderingKHR(c);ForgetCommand(c);}
inline PFN_vkVoidFunction WrapResourceFn(const char* name,PFN_vkVoidFunction original){
  if(!original)return nullptr;
#define RDR_RESOURCE_HOOK(vkname,member,wrapper) if(std::strcmp(name,vkname)==0){resource_detail::member=reinterpret_cast<decltype(resource_detail::member)>(original);return reinterpret_cast<PFN_vkVoidFunction>(wrapper);}
  RDR_RESOURCE_HOOK("vkCreateImage",createImage,TrackCreateImage)
  RDR_RESOURCE_HOOK("vkDestroyImage",destroyImage,TrackDestroyImage)
  RDR_RESOURCE_HOOK("vkBeginCommandBuffer",beginCommand,TrackBegin)
  RDR_RESOURCE_HOOK("vkResetCommandBuffer",resetCommand,TrackReset)
  RDR_RESOURCE_HOOK("vkFreeCommandBuffers",freeCommands,TrackFree)
  RDR_RESOURCE_HOOK("vkCmdPipelineBarrier",barrier,TrackBarrier)
  RDR_RESOURCE_HOOK("vkCmdPipelineBarrier2",barrier2,TrackBarrier2)
  RDR_RESOURCE_HOOK("vkCmdPipelineBarrier2KHR",barrier2KHR,TrackBarrier2KHR)
  RDR_RESOURCE_HOOK("vkCmdBeginRenderPass",beginPass,TrackBeginPass)
  RDR_RESOURCE_HOOK("vkCmdEndRenderPass",endPass,TrackEndPass)
  RDR_RESOURCE_HOOK("vkCmdBeginRenderPass2",beginPass2,TrackBeginPass2)
  RDR_RESOURCE_HOOK("vkCmdBeginRenderPass2KHR",beginPass2KHR,TrackBeginPass2KHR)
  RDR_RESOURCE_HOOK("vkCmdEndRenderPass2",endPass2,TrackEndPass2)
  RDR_RESOURCE_HOOK("vkCmdEndRenderPass2KHR",endPass2KHR,TrackEndPass2KHR)
  RDR_RESOURCE_HOOK("vkCmdExecuteCommands",executeCommands,TrackExecute)
  RDR_RESOURCE_HOOK("vkCmdBeginRendering",beginRendering,TrackBeginRendering)
  RDR_RESOURCE_HOOK("vkCmdBeginRenderingKHR",beginRenderingKHR,TrackBeginRenderingKHR)
  RDR_RESOURCE_HOOK("vkCmdEndRendering",endRendering,TrackEndRendering)
  RDR_RESOURCE_HOOK("vkCmdEndRenderingKHR",endRenderingKHR,TrackEndRenderingKHR)
#undef RDR_RESOURCE_HOOK
  return original;
}
}
