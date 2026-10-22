#include "xess_bridge_snapshot.h"

namespace fgvk::xess_bridge {
bool Snapshot::Create(Device& bridge,VkDevice device,VkPhysicalDevice physical,
    PFN_vkGetDeviceProcAddr gd,PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties,
    VkExtent2D output,VkExtent2D render,DXGI_FORMAT colorDX,VkFormat colorVK){
  if(bridge_ || !device || !physical || !gd || !memoryProperties ||
     !output.width || !output.height || !render.width || !render.height)
    return Fail("Invalid snapshot initialization");
#define LOAD(name) p_##name=(PFN_vk##name)gd(device,"vk" #name);if(!p_##name)return Fail("Missing vk" #name)
  LOAD(CreateBuffer);LOAD(DestroyBuffer);LOAD(GetBufferMemoryRequirements);
  LOAD(AllocateMemory);LOAD(FreeMemory);LOAD(BindBufferMemory);
  LOAD(CmdPipelineBarrier);LOAD(CmdCopyImage);LOAD(CmdCopyImageToBuffer);LOAD(CmdCopyBufferToImage);
#undef LOAD
  bridge_=&bridge;device_=device;output_=output;render_=render;colorFormat_=colorVK;
  auto unwind=[&](const char* reason){Destroy();return Fail(reason);};
  if(!bridge.CreateImage(output.width,output.height,colorDX,colorVK,hudless) ||
     !bridge.CreateImage(render.width,render.height,DXGI_FORMAT_R32_FLOAT,VK_FORMAT_R32_SFLOAT,depth) ||
     !bridge.CreateImage(render.width,render.height,DXGI_FORMAT_R16G16_FLOAT,VK_FORMAT_R16G16_SFLOAT,motion))
    return unwind(bridge.Error());
  // D32 depth aspect bytes are IEEE float32. The image -> buffer -> image
  // route preserves them without an illegal depth/color image-format copy.
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size=VkDeviceSize(render.width)*render.height*4;
  ci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(p_CreateBuffer(device_,&ci,nullptr,&depthScratch_)!=VK_SUCCESS)return unwind("Depth transport buffer creation failed");
  VkMemoryRequirements requirements{};p_GetBufferMemoryRequirements(device_,depthScratch_,&requirements);
  VkPhysicalDeviceMemoryProperties memory{};memoryProperties(physical,&memory);uint32_t type=UINT32_MAX;
  for(uint32_t i=0;i<memory.memoryTypeCount;++i)
    if((requirements.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
  if(type==UINT32_MAX)return unwind("Depth transport requires device-local memory");
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.memoryTypeIndex=type;ai.allocationSize=requirements.size;
  if(p_AllocateMemory(device_,&ai,nullptr,&scratchMemory_)!=VK_SUCCESS ||
     p_BindBufferMemory(device_,depthScratch_,scratchMemory_,0)!=VK_SUCCESS)return unwind("Depth transport memory allocation/binding failed");
  error_="";return true;
}
bool Snapshot::Record(VkCommandBuffer cmd,const SnapshotInput& color,const SnapshotInput& z,
    const SnapshotInput& mv,uint32_t family,bool firstUse){
  if(!bridge_ || !cmd || family>=VK_QUEUE_FAMILY_FOREIGN_EXT)return Fail("Invalid snapshot command context");
  const SnapshotInput* source[]={&color,&z,&mv};
  const VkExtent2D sizes[]={output_,render_,render_};
  const bool includeColor=color.image!=VK_NULL_HANDLE;
  if((includeColor && color.format!=colorFormat_) || (z.format!=VK_FORMAT_D32_SFLOAT && z.format!=VK_FORMAT_D32_SFLOAT_S8_UINT) ||
     mv.format!=VK_FORMAT_R16G16_SFLOAT)return Fail("Unsupported snapshot formats; no format reinterpretation is permitted");
  for(uint32_t i=0;i<3;++i){
    if(i==0 && !includeColor)continue;
    const auto& in=*source[i];const auto size=sizes[i];
    if(!in.image || !(in.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
       in.layout==VK_IMAGE_LAYOUT_UNDEFINED || in.layout==VK_IMAGE_LAYOUT_PREINITIALIZED ||
       in.origin.x<0 || in.origin.y<0 || uint64_t(in.origin.x)+size.width>in.allocationExtent.width ||
       uint64_t(in.origin.y)+size.height>in.allocationExtent.height)
      return Fail("Snapshot source lacks a valid layout, transfer usage or extent");
  }
  // Wait for the previous DX12 consumer on its shared fence before submitting
  // this command. firstUse becomes false only after successful Vulkan submission.
  SharedImage* target[]={&hudless,&depth,&motion};
  VkImageMemoryBarrier acquire[6]{};
  for(uint32_t i=0;i<3;++i){
    if(i==0 && !includeColor)continue;
    auto& s=acquire[i];s.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    s.image=source[i]->image;s.oldLayout=source[i]->layout;s.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    s.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;s.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    s.srcQueueFamilyIndex=s.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    if(i==1)aspect=z.format==VK_FORMAT_D32_SFLOAT_S8_UINT?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT;
    s.subresourceRange={aspect,0,1,0,1};
    auto& d=acquire[i+3];d.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    d.image=target[i]->vk;d.oldLayout=firstUse?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL;
    d.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;d.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    d.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;d.dstQueueFamilyIndex=family;
    d.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  }
  VkImageMemoryBarrier compact[6]{};uint32_t barrierCount=0;
  for(uint32_t i=0;i<6;++i)if(acquire[i].image)compact[barrierCount++]=acquire[i];
  p_CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
      0,0,nullptr,0,nullptr,barrierCount,compact);
  for(uint32_t i=0;i<3;i+=2){
    if(i==0 && !includeColor)continue;
    VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.srcOffset={source[i]->origin.x,source[i]->origin.y,0};region.extent={sizes[i].width,sizes[i].height,1};
    p_CmdCopyImage(cmd,source[i]->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target[i]->vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
  }
  VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
  region.imageOffset={z.origin.x,z.origin.y,0};region.imageExtent={render_.width,render_.height,1};
  p_CmdCopyImageToBuffer(cmd,z.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depthScratch_,1,&region);
  VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  buffer.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;buffer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  buffer.srcQueueFamilyIndex=buffer.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  buffer.buffer=depthScratch_;buffer.size=VK_WHOLE_SIZE;
  p_CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&buffer,0,nullptr);
  region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;region.imageOffset={0,0,0};
  p_CmdCopyBufferToImage(cmd,depthScratch_,depth.vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
  VkImageMemoryBarrier finish[6]{};
  for(uint32_t i=0;i<3;++i){
    if(i==0 && !includeColor)continue;
    finish[i]=acquire[i];finish[i].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    finish[i].newLayout=source[i]->layout;finish[i].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    finish[i].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    finish[i+3]=acquire[i+3];finish[i+3].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    finish[i+3].newLayout=VK_IMAGE_LAYOUT_GENERAL;finish[i+3].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    finish[i+3].dstAccessMask=0;finish[i+3].srcQueueFamilyIndex=family;finish[i+3].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
  }
  barrierCount=0;for(uint32_t i=0;i<6;++i)if(finish[i].image)compact[barrierCount++]=finish[i];
  p_CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,barrierCount,compact);
  // Signal the Vulkan producer fence after this command; DX12 must GPU-Wait
  // before reading, then return resources to COMMON and signal its consumer fence.
  error_="";return true;
}
bool Snapshot::RecordColor(VkCommandBuffer cmd,const SnapshotInput& in,uint32_t family,bool firstUse){
  if(!bridge_ || !cmd || !in.image || in.format!=colorFormat_ ||
     !(in.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || in.origin.x<0 || in.origin.y<0 ||
     uint64_t(in.origin.x)+output_.width>in.allocationExtent.width ||
     uint64_t(in.origin.y)+output_.height>in.allocationExtent.height)return Fail("Invalid HUD-less color snapshot");
  VkImageMemoryBarrier barriers[2]{};
  for(auto& b:barriers){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
  auto& s=barriers[0];s.image=in.image;s.oldLayout=in.layout;s.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  s.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;s.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  s.srcQueueFamilyIndex=s.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  auto& d=barriers[1];d.image=hudless.vk;d.oldLayout=firstUse?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL;
  d.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;d.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  d.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;d.dstQueueFamilyIndex=family;
  p_CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers);
  VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
  copy.srcOffset={in.origin.x,in.origin.y,0};copy.extent={output_.width,output_.height,1};
  p_CmdCopyImage(cmd,in.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hudless.vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  s.oldLayout=s.newLayout;s.newLayout=in.layout;s.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;s.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  d.oldLayout=d.newLayout;d.newLayout=VK_IMAGE_LAYOUT_GENERAL;d.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;d.dstAccessMask=0;
  d.srcQueueFamilyIndex=family;d.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
  p_CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,barriers);
  return true;
}
void Snapshot::Destroy(){
  if(depthScratch_ && p_DestroyBuffer)p_DestroyBuffer(device_,depthScratch_,nullptr);
  if(scratchMemory_ && p_FreeMemory)p_FreeMemory(device_,scratchMemory_,nullptr);
  depthScratch_=VK_NULL_HANDLE;scratchMemory_=VK_NULL_HANDLE;
  if(bridge_){bridge_->DestroyImage(hudless);bridge_->DestroyImage(depth);bridge_->DestroyImage(motion);}
  bridge_=nullptr;device_=VK_NULL_HANDLE;
}
}
