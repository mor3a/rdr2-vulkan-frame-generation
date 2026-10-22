#pragma once
#include <vulkan/vulkan.h>

namespace fgvk {
extern VkInstance gInstance;
extern VkPhysicalDevice gPhysicalDevice;
extern VkDevice gDevice;
extern VkQueue gGraphicsQueue;
extern uint32_t gGraphicsFamily;

void* DeviceFn(const char* name);
void* LoaderFn(const char* name);
VkResult SubmitInternalReadbackFence(VkQueue,VkFence);

bool InstallVkHooks(bool frameGenerationAvailable=true);
void RemoveVkHooks();
}
