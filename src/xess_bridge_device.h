#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

namespace fgvk::xess_bridge {
// Transport foundation only. No swapchain replacement or FG is activated here.
// The future provider must retire all GPU use before DestroyImage/Shutdown.
struct SharedImage { ID3D12Resource* dx{}; VkImage vk{}; VkDeviceMemory memory{}; };
struct SharedFence { ID3D12Fence* dx{}; VkSemaphore vk{}; };
class Device {
public:
  Device()=default;
  Device(const Device&)=delete;
  Device& operator=(const Device&)=delete;
  ~Device(){Shutdown();}
  bool Initialize(VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetInstanceProcAddr,
                  PFN_vkGetDeviceProcAddr,bool externalMemoryEnabled);
  bool CreateImage(uint32_t width,uint32_t height,DXGI_FORMAT,VkFormat,SharedImage&);
  void DestroyImage(SharedImage&); // caller must have completed BOTH APIs' GPU use
  bool CreateFence(SharedFence&); // external semaphore extension + timeline feature required
  void DestroyFence(SharedFence&); // caller must have completed BOTH APIs' GPU use
  void Shutdown(); // shared images must be destroyed first
  ID3D12Device* Native() const{return dx_;}
  const char* Error() const{return error_;}
  int64_t NativeResult() const{return nativeResult_;}
private:
  bool Fail(const char* message,int64_t native=0){error_=message;nativeResult_=native;return false;}
  ID3D12Device* dx_{};
  VkDevice vk_{}; VkPhysicalDevice physical_{};
  PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties_{};
  PFN_vkGetPhysicalDeviceImageFormatProperties2 imageProperties_{};
  PFN_vkCreateImage createImage_{};PFN_vkDestroyImage destroyImage_{};
  PFN_vkGetImageMemoryRequirements imageRequirements_{};
  PFN_vkGetMemoryWin32HandlePropertiesKHR handleProperties_{};
  PFN_vkAllocateMemory allocate_{};PFN_vkFreeMemory free_{};
  PFN_vkBindImageMemory bind_{};
  PFN_vkCreateSemaphore createSemaphore_{};PFN_vkDestroySemaphore destroySemaphore_{};
  PFN_vkImportSemaphoreWin32HandleKHR importSemaphore_{};
  const char* error_="Not initialized";
  int64_t nativeResult_{};
};
}
