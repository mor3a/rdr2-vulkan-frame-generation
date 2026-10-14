#include "vulkan_owner.h"
#include <thread>
#include <cstdio>
int main(){
  fgvk::VulkanOwner owner;int failed=0;auto check=[&](bool value){if(!value)++failed;};
  check(owner.ClaimInstance());bool auxiliaryWon=true;
  std::thread auxiliary([&]{auxiliaryWon=owner.ClaimInstance();});auxiliary.join();check(!auxiliaryWon);
  check(owner.ClaimDevice());bool secondWon=true;
  std::thread helper([&]{secondWon=owner.ClaimDevice();});helper.join();check(!secondWon);
  owner.DeviceFailed();check(owner.ClaimDevice());check(!owner.ClaimDevice());
  owner.InstanceFailed();check(owner.ClaimInstance());
  printf("7 Vulkan owner concurrency/retry checks: %d failures\n",failed);return failed?1:0;
}
