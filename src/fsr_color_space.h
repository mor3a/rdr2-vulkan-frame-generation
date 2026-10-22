#pragma once
#include <vulkan/vulkan.h>
#include "ffx_api/ffx_api_types.h"
namespace fgvk {
inline int FsrTransferFunction(VkColorSpaceKHR space){
  switch(space){
  case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:return FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  case VK_COLOR_SPACE_HDR10_ST2084_EXT:return FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
  case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:return FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
  default:return -1;
  }
}
}
