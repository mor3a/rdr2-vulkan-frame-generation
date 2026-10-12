#pragma once
#include <vulkan/vulkan.h>
#include <sl.h>
#include "resource_state.h"
namespace fgvk {
bool FsrLoad();
void FsrDisable();
bool FsrPlanDevice(VkInstance,VkPhysicalDevice,const VkDeviceCreateInfo*,VkDeviceCreateInfo&);
void FsrDeviceReady(VkDevice,PFN_vkGetDeviceProcAddr);
VkResult FsrCreateSwapchain(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR*);
PFN_vkVoidFunction FsrOverride(const char*);
bool FsrPrepare(VkCommandBuffer,VkImage,VkImage,uint32_t,uint32_t,uint32_t,uint32_t,float,float,float,float,const ImageState&,const ImageState&,const sl::Constants&,int);
VkResult FsrPresent(VkQueue,const VkPresentInfoKHR*,bool);
}
