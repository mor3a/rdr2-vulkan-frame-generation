#pragma once
#include <vulkan/vulkan.h>
#include <sl.h>
namespace fgvk {
bool NativeHudEnabled();
void NativeHudRegister(VkDevice,VkPhysicalDevice,VkSwapchainKHR,const VkSwapchainCreateInfoKHR*,PFN_vkGetDeviceProcAddr);
void NativeHudImages(VkDevice,VkSwapchainKHR,uint32_t,const VkImage*);
void NativeHudDestroy(VkDevice,VkSwapchainKHR);
PFN_vkVoidFunction NativeHudWrap(const char*,PFN_vkVoidFunction);
void NativeHudAcquire(VkSwapchainKHR,uint32_t);
void NativeHudForget(VkCommandBuffer);
void NativeHudExecute(VkCommandBuffer,uint32_t,const VkCommandBuffer*);
void NativeHudSubmit(uint32_t,const VkCommandBuffer*,uint32_t token);
void NativeHudTag(const VkPresentInfoKHR*,sl::FrameToken&,uint32_t token);
}
