#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>
namespace fgvk {
void StartMenu();
bool ConsumeMenuRequest(bool& off,uint32_t& frames);
void UpdateMenuRuntime(bool off,uint32_t frames);
void UpdateMenuStats(uint32_t maximum,uint32_t status,float ratio);
void RecordGamePresent();
void RecordDisplayedFrames(uint32_t displayedFrames);
void SetFPSGenerationActive(bool active);
bool MenuIsOpen();
void MenuRendererAvailable(bool available);
bool MenuInitializeUI();
bool MenuHasDrawing();
void MenuBuildUI();
void MenuRegisterSwapchain(VkDevice,VkPhysicalDevice,VkInstance,uint32_t,VkSwapchainKHR,const VkSwapchainCreateInfoKHR*,PFN_vkGetDeviceProcAddr,PFN_vkGetSwapchainImagesKHR);
void MenuDestroySwapchain(VkDevice,VkSwapchainKHR);
bool MenuRender(VkQueue,const VkPresentInfoKHR*,VkPresentInfoKHR&);
}
