#include "present_route.h"
#include <cstdio>
int main(){using namespace fgvk;int failures=0;auto check=[&](bool ok){if(!ok)++failures;};
// No swapchain tracked yet: never divert, creation may predate the wrapper.
check(RoutePresent(true,false,1)==PresentRoute::FrameGeneration);
check(RoutePresent(true,true,1)==PresentRoute::FrameGeneration);
// Owned swapchain known: frame generation path.
check(RoutePresent(false,true,1)==PresentRoute::FrameGeneration);
check(RoutePresent(false,true,2)==PresentRoute::FrameGeneration);
// Only foreign swapchains once the owned set is populated: native path.
check(RoutePresent(false,false,1)==PresentRoute::Native);
check(RoutePresent(false,false,3)==PresentRoute::Native);
// A present without swapchains is not diverted.
check(RoutePresent(false,false,0)==PresentRoute::FrameGeneration);
check(RoutePresent(true,false,0)==PresentRoute::FrameGeneration);
std::printf("8 present routing checks: %d failures\n",failures);return failures?1:0;}
