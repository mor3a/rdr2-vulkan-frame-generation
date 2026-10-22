#pragma once
#include "xess_bridge_device.h"

namespace fgvk::xess_bridge {
struct SnapshotInput {
  VkImage image{};
  VkFormat format{};
  VkImageLayout layout{};
  VkImageUsageFlags usage{};
  VkExtent2D allocationExtent{};
  VkOffset2D origin{};
};
class Snapshot {
public:
  Snapshot()=default;
  Snapshot(const Snapshot&)=delete;
  Snapshot& operator=(const Snapshot&)=delete;
  // Explicit Destroy after BOTH APIs retire. Never implicitly free live GPU data.
  bool Create(Device&,VkDevice,VkPhysicalDevice,PFN_vkGetDeviceProcAddr,
      PFN_vkGetPhysicalDeviceMemoryProperties,VkExtent2D output,VkExtent2D render,
      DXGI_FORMAT colorDX,VkFormat colorVK);
  bool Record(VkCommandBuffer,const SnapshotInput& hudless,const SnapshotInput& depth,
      const SnapshotInput& motion,uint32_t graphicsFamily,bool firstUse);
  bool RecordColor(VkCommandBuffer,const SnapshotInput&,uint32_t graphicsFamily,bool firstUse);
  void Destroy();
  const char* Error() const{return error_;}
  SharedImage hudless{},depth{},motion{};
private:
  bool Fail(const char* reason){error_=reason;return false;}
  Device* bridge_{};
  VkDevice device_{};
  VkBuffer depthScratch_{};
  VkDeviceMemory scratchMemory_{};
  VkExtent2D output_{},render_{};
  VkFormat colorFormat_{};
  const char* error_="Not created";
#define FIELD(name) PFN_vk##name p_##name{}
  FIELD(CreateBuffer);FIELD(DestroyBuffer);FIELD(GetBufferMemoryRequirements);
  FIELD(AllocateMemory);FIELD(FreeMemory);FIELD(BindBufferMemory);
  FIELD(CmdPipelineBarrier);FIELD(CmdCopyImage);FIELD(CmdCopyImageToBuffer);FIELD(CmdCopyBufferToImage);
#undef FIELD
};
}
