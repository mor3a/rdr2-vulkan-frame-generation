#pragma once

#include <windows.h>
#include <vulkan/vulkan.h>
#include <ffx_api/ffx_api_types.h>
#include <cstdint>
#include <vector>

namespace fgvk {
void HudCaptureInit(VkDevice, VkPhysicalDevice, PFN_vkGetDeviceProcAddr, VkFormat, VkExtent2D, const std::vector<VkImage>&);
void HudCaptureDestroy();
PFN_vkVoidFunction HudCaptureWrap(const char*, PFN_vkVoidFunction);
FfxApiResource HudCaptureResource();
void HudCaptureBeforePass(VkCommandBuffer, const VkRenderPassBeginInfo*);
void HudCaptureBeginFrame(uint64_t);
void HudCaptureInstallExportHooks(HMODULE);
void HudCaptureForgetCommand(VkCommandBuffer);
void HudCaptureOnSubmit(uint32_t,const VkCommandBuffer*);
}
