#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

namespace fgvk::xess_bridge {
// Owns the added extension list and feature structure until vkCreateDevice.
// Never changes the original application's structures or an existing provider.
class DevicePlan {
public:
  DevicePlan()=default;
  DevicePlan(const DevicePlan&)=delete;
  DevicePlan& operator=(const DevicePlan&)=delete;
  bool Prepare(VkInstance,VkPhysicalDevice,const VkDeviceCreateInfo*,PFN_vkGetInstanceProcAddr);
  const VkDeviceCreateInfo* Info() const{return ready_?&info_:nullptr;}
  const char* Error() const{return error_;}
private:
  bool Fail(const char* reason){ready_=false;error_=reason;return false;}
  VkDeviceCreateInfo info_{};
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  std::vector<const char*> extensions_;
  std::vector<std::unique_ptr<unsigned char[]>> featureCopies_;
  bool ready_{};
  const char* error_="Not prepared";
};
}
