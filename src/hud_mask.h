#pragma once

#include <vulkan/vulkan.h>
#include <ffx_api/ffx_api_types.h>

namespace fgvk {
void HudMaskInit(VkDevice, VkPhysicalDevice, PFN_vkGetDeviceProcAddr, VkFormat, VkExtent2D);
void HudMaskDestroy();
bool HudMaskApply(VkCommandBuffer, FfxApiResource real, FfxApiResource hudless, FfxApiResource generated);
}
