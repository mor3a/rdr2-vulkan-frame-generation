#pragma once
#include <vulkan/vulkan.h>
namespace fgvk {
// Name/family are detected from Vulkan when available, with a DXGI NVIDIA-adapter
// fallback before Vulkan initialization (needed by the Streamline startup path).
enum class GpuFamily { Unknown, RTX20, RTX30, RTX40, RTX50 };
enum class GpuFgSupport { Unknown, ExperimentalTuring, AmpereUnlock, Native };
void SetGpuInfo(const VkPhysicalDeviceProperties& properties);
const char* GPUName();
GpuFamily GPUFamily();
GpuFgSupport GPUFrameGenerationSupport();
bool ExperimentalTuringEnabled();
// Human-readable current state of the private RTX 30 unlock path.
const char* GPUUnlockStatus();
bool LoadAmpereUnlock();
}
