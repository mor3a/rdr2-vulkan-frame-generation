#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
namespace fgvk::xess_bridge {
// Prefer a reusable image whose copies have retired. If all are still pending,
// wait for the oldest completion, never overwrite an acquired/live image.
constexpr uint32_t SelectAcquireSlot(const uint64_t* retired,const bool* acquired,
    uint32_t count,uint32_t next,uint64_t completed){
  uint32_t pending=UINT32_MAX;
  for(uint32_t n=0;n<count;++n){uint32_t at=(next+n)%count;if(acquired[at])continue;
    if(retired[at]<=completed)return at;
    if(pending==UINT32_MAX || retired[at]<retired[pending])pending=at;
  }
  return pending;
}
constexpr uint32_t SelectGraphicsFamily(const VkDeviceQueueCreateInfo* requested,uint32_t count,
    const VkQueueFamilyProperties* available,uint32_t families){
  for(uint32_t i=0;i<count;++i){const auto& q=requested[i];
    if(q.queueCount && !(q.flags&VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) && q.queueFamilyIndex<families &&
       (available[q.queueFamilyIndex].queueFlags&VK_QUEUE_GRAPHICS_BIT))return q.queueFamilyIndex;
  }
  return UINT32_MAX;
}
}
