#include "bridge_format.h"
#include "bridge_lifecycle.h"
#include <cassert>
#include <cstdio>
using namespace fgvk::xess_bridge;
static_assert(int(VK_FORMAT_A2B10G10R10_UNORM_PACK32)==64);
static_assert(CompatibleTransportFormats(DXGI_FORMAT_R10G10B10A2_UNORM,VK_FORMAT_A2B10G10R10_UNORM_PACK32));
static_assert(!CompatibleTransportFormats(DXGI_FORMAT_R10G10B10A2_UNORM,VK_FORMAT_A2R10G10B10_UNORM_PACK32));
static_assert(!TransportNeedsStorage(DXGI_FORMAT_R10G10B10A2_UNORM));
static_assert(!TransportNeedsStorage(DXGI_FORMAT_B8G8R8A8_UNORM));
static_assert(SupportedOutputColorSpace(VK_FORMAT_A2B10G10R10_UNORM_PACK32,VK_COLOR_SPACE_HDR10_ST2084_EXT));
static_assert(!SupportedOutputColorSpace(VK_FORMAT_R8G8B8A8_UNORM,VK_COLOR_SPACE_HDR10_ST2084_EXT));
static_assert(!SupportedOutputColorSpace(VK_FORMAT_R16G16B16A16_SFLOAT,VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT));
static_assert(TransportNeedsStorage(DXGI_FORMAT_R32_FLOAT));
static_assert(ReplacePresentation(1,1,true,false)); // no oldSwapchain, same surface
static_assert(ReplacePresentation(1,1,false,true));
static_assert(!ReplacePresentation(1,2,true,false)); // second window untouched
static_assert(!ReplacePresentation(1,1,false,false)); // unrelated old chain
static_assert(InputRegionFits(2560,1440,0,0,1280,720)); // pooled texture after quality change
static_assert(InputRegionFits(2560,1440,16,8,1920,1080));
static_assert(!InputRegionFits(2560,1440,-1,0,1280,720));
static_assert(!InputRegionFits(2560,1440,1000,0,1920,1080));
static_assert(!InputRegionFits(2560,1440,0,0,0,720));
int main(){
 InputDimensions dimensions;unsigned resets=0;
 const unsigned sizes[][2]={{1280,720},{1920,1080},{2560,1440},{1280,720}};
 for(auto& size:sizes){
   assert(dimensions.Changed(size[0],size[1]));
   // Rejected input cannot commit dimensions or consume the required reset.
   assert(dimensions.Changed(size[0],size[1]));
   resets+=dimensions.Accept(size[0],size[1]);
   for(int i=0;i<1000;++i)assert(!dimensions.Accept(size[0],size[1]));
 }
 assert(resets==4);dimensions={};assert(dimensions.Changed(1280,720));
 puts("Bridge recreation: packed RGB10 layout, secondary-window isolation, pooled active rectangles and 4000 stable/rejected quality transitions: PASS");
}
