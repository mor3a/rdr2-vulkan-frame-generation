#include "fsr_color_space.h"
#include <cstdio>
int main(){using namespace fgvk;
 // A 10-bit SDR swapchain remains sRGB; pixel format does not select PQ.
 if(FsrTransferFunction(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)!=FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB)return 1;
 if(FsrTransferFunction(VK_COLOR_SPACE_HDR10_ST2084_EXT)!=FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ)return 2;
 if(FsrTransferFunction(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)!=FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB)return 3;
 if(FsrTransferFunction(VK_COLOR_SPACE_HDR10_HLG_EXT)!=-1)return 4;
 puts("FSR SDR / PQ / scRGB / unsupported color-space checks: PASS");}
