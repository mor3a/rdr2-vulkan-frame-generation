#pragma once
#include <dxgiformat.h>
#include <vulkan/vulkan.h>
namespace fgvk::xess_bridge {
constexpr DXGI_FORMAT TransportFormat(VkFormat format){
  switch(format){
  case VK_FORMAT_R8G8B8A8_UNORM:return DXGI_FORMAT_R8G8B8A8_UNORM;
  case VK_FORMAT_B8G8R8A8_UNORM:return DXGI_FORMAT_B8G8R8A8_UNORM;
  // Both layouts store R in bits 0..9, G 10..19, B 20..29, A 30..31.
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:return DXGI_FORMAT_R10G10B10A2_UNORM;
  case VK_FORMAT_R16G16B16A16_SFLOAT:return DXGI_FORMAT_R16G16B16A16_FLOAT;
  default:return DXGI_FORMAT_UNKNOWN;
  }
}
constexpr bool SupportedOutputColorSpace(VkFormat format,VkColorSpaceKHR space){
  return space==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
    (format==VK_FORMAT_A2B10G10R10_UNORM_PACK32 && space==VK_COLOR_SPACE_HDR10_ST2084_EXT);
}
constexpr bool CompatibleTransportFormats(DXGI_FORMAT dx,VkFormat vk){
  return (dx!=DXGI_FORMAT_UNKNOWN && TransportFormat(vk)==dx) ||
    (dx==DXGI_FORMAT_R16G16_FLOAT && vk==VK_FORMAT_R16G16_SFLOAT) ||
    (dx==DXGI_FORMAT_R32_FLOAT && vk==VK_FORMAT_R32_SFLOAT);
}
constexpr bool TransportNeedsStorage(DXGI_FORMAT dx){
  return dx!=DXGI_FORMAT_B8G8R8A8_UNORM && dx!=DXGI_FORMAT_R10G10B10A2_UNORM;
}
}
