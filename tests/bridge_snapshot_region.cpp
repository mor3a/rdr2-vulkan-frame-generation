#include "xess_bridge_device.h"
#define private public
#include "xess_bridge_snapshot.h"
#undef private
#include <cassert>
#include <cstdio>
using namespace fgvk::xess_bridge;
static VkImageCopy motionCopy{};static VkBufferImageCopy depthCopy{},depthTarget{};
static unsigned calls{};
static void VKAPI_CALL Barriers(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*){}
static void VKAPI_CALL Copy(VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,uint32_t,const VkImageCopy* r){motionCopy=*r;++calls;}
static void VKAPI_CALL ToBuffer(VkCommandBuffer,VkImage,VkImageLayout,VkBuffer,uint32_t,const VkBufferImageCopy* r){depthCopy=*r;++calls;}
static void VKAPI_CALL FromBuffer(VkCommandBuffer,VkBuffer,VkImage,VkImageLayout,uint32_t,const VkBufferImageCopy* r){depthTarget=*r;++calls;}
int main(){
 Device bridge;Snapshot snapshot;
 snapshot.bridge_=&bridge;snapshot.render_={1280,720};snapshot.output_={2560,1440};
 snapshot.depth.vk=(VkImage)uintptr_t(1);snapshot.motion.vk=(VkImage)uintptr_t(2);
 snapshot.depthScratch_=(VkBuffer)uintptr_t(3);
 snapshot.p_CmdPipelineBarrier=Barriers;snapshot.p_CmdCopyImage=Copy;
 snapshot.p_CmdCopyImageToBuffer=ToBuffer;snapshot.p_CmdCopyBufferToImage=FromBuffer;
 SnapshotInput depth{(VkImage)uintptr_t(4),VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_USAGE_TRANSFER_SRC_BIT,{2560,1440},{32,16}};
 SnapshotInput motion{(VkImage)uintptr_t(5),VK_FORMAT_R16G16_SFLOAT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_USAGE_TRANSFER_SRC_BIT,{2560,1440},{64,8}};
 assert(snapshot.Record((VkCommandBuffer)uintptr_t(6),{},depth,motion,0,true));
 assert(calls==3 && depthCopy.imageOffset.x==32 && depthCopy.imageOffset.y==16);
 assert(depthCopy.imageExtent.width==1280 && depthCopy.imageExtent.height==720);
 assert(depthTarget.imageOffset.x==0 && depthTarget.imageOffset.y==0);
 assert(motionCopy.srcOffset.x==64 && motionCopy.srcOffset.y==8);
 assert(motionCopy.extent.width==1280 && motionCopy.extent.height==720);
 motion.origin.x=2000;assert(!snapshot.Record((VkCommandBuffer)uintptr_t(6),{},depth,motion,0,false));assert(calls==3);
 puts("Production snapshot recording: active extents and separate depth/MV origins, pooled texture bounds and invalid-input rejection: PASS");
}
