#pragma once
#include <vulkan/vulkan.h>
namespace fgvk {
// Vulkan defaults to LOCAL mode and mask 1 without device-group present info.
// Only remove an equivalent head node; leave all other extensions untouched.
inline bool NormalizeLocalPresent(const VkPresentInfoKHR* source,VkPresentInfoKHR& target){
  if(!source || source->swapchainCount!=1 || !source->pNext)return false;
  auto* head=(const VkBaseInStructure*)source->pNext;
  if(head->sType!=VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR)return false;
  auto* group=(const VkDeviceGroupPresentInfoKHR*)head;
  if(group->mode!=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR)return false;
  if(group->swapchainCount!=0 && (group->swapchainCount!=1 || !group->pDeviceMasks || group->pDeviceMasks[0]!=1))return false;
  target=*source;target.pNext=group->pNext;return true;
}
}
