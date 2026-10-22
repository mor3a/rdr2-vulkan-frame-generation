#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>
#include <sl.h>
#include "resource_state.h"
#include "config.h"
namespace fgvk {
bool XessPlanDevice(VkInstance,VkPhysicalDevice,const VkDeviceCreateInfo*,VkDeviceCreateInfo&,PFN_vkGetInstanceProcAddr);
void XessDeviceReady(VkInstance,VkPhysicalDevice,VkDevice,uint32_t,PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr);
uint32_t XessGraphicsFamily();
bool XessValidatePresentQueue(VkQueue,const VkPresentInfoKHR*);
PFN_vkVoidFunction XessInstanceFn(const char*,PFN_vkVoidFunction);
PFN_vkVoidFunction XessOverride(const char*,PFN_vkVoidFunction);
PFN_vkVoidFunction VKAPI_CALL XessMenuGDPA(VkDevice,const char*);
VkResult XessCreateSwapchain(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR*);
VkResult XessPresent(VkQueue,const VkPresentInfoKHR*);
bool XessPrepare(VkCommandBuffer,VkImage,VkImage,uint32_t,uint32_t,uint32_t,uint32_t,float,float,float,float,const ImageState&,const ImageState&,const sl::Constants&,int,VkOffset2D depthOrigin={0,0},VkOffset2D motionOrigin={0,0});
void RequestBridgeRestart(FGProvider,int);
bool BridgeRestartPending();
void XessBeforeSubmit(VkQueue,uint32_t,const VkCommandBuffer*);
void XessSubmitted(VkQueue,uint32_t,const VkCommandBuffer*);
void XessForget(VkCommandBuffer);
void XessExecute(VkCommandBuffer,uint32_t,const VkCommandBuffer*);
void XessEvalResult(VkCommandBuffer,bool);
bool XessHiddenExtension(const char*);
void XessCaptureLock();
void XessCaptureUnlock();
}
