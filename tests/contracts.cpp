#include "input_contract.h"
#include "resource_state.h"
#include "mvecscale.h"
#include <cstdio>
#include <limits>
int main(){
  int failures=0; auto check=[&](bool ok){if(!ok)failures++;};
  check(fgvk::ValidExtent(1920,1080));
  check(!fgvk::ValidExtent(0,1080) && !fgvk::ValidExtent(~0u,1080));
  check(fgvk::ValidScalars(0.5f,-0.5f,-1.f,-1.f));
  check(!fgvk::ValidScalars(0,0,0,-1.f) && !fgvk::ValidScalars(std::numeric_limits<float>::quiet_NaN(),0,-1,-1));
  auto scale=fgvk::MvecScaleForStreamline(-1,-1,960,540,true);
  check(std::abs(scale.x*960+1.f)<1e-6f && std::abs(scale.y*540+1.f)<1e-6f);
  auto c=reinterpret_cast<VkCommandBuffer>(1);auto image=reinterpret_cast<VkImage>(2);
  fgvk::resource_detail::images[image]={VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_USAGE_SAMPLED_BIT,VK_FORMAT_R32_SFLOAT,1920,1080};
  fgvk::ImageState state{}; check(!fgvk::FindImageState(c,image,state));
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  fgvk::ObserveBarrier(c,image,VK_IMAGE_LAYOUT_GENERAL,range);
  check(fgvk::FindImageState(c,image,state) && state.layout==VK_IMAGE_LAYOUT_GENERAL);
  fgvk::ForgetCommand(c);check(!fgvk::FindImageState(c,image,state));
  std::printf("8 focused contract checks: %d failures\n",failures);
  return failures?1:0;
}
