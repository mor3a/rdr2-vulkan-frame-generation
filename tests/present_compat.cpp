#include "present_compat.h"
#include <cstdio>
int main(){using namespace fgvk;int failures=0;auto check=[&](bool ok){if(!ok)++failures;};
VkPresentInfoKHR source{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR},out{};
check(!NormalizeLocalPresent(nullptr,out));check(!NormalizeLocalPresent(&source,out));
uint32_t mask=1;VkDeviceGroupPresentInfoKHR group{VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR};group.swapchainCount=1;group.pDeviceMasks=&mask;group.mode=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
VkPresentIdKHR next{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};group.pNext=&next;source.pNext=&group;source.swapchainCount=1;
check(NormalizeLocalPresent(&source,out) && out.pNext==&next && source.pNext==&group);
mask=2;check(!NormalizeLocalPresent(&source,out));mask=1;
group.mode=VK_DEVICE_GROUP_PRESENT_MODE_REMOTE_BIT_KHR;check(!NormalizeLocalPresent(&source,out));group.mode=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
source.swapchainCount=2;check(!NormalizeLocalPresent(&source,out));source.swapchainCount=1;
group.pDeviceMasks=nullptr;check(!NormalizeLocalPresent(&source,out));
group.swapchainCount=0;check(NormalizeLocalPresent(&source,out));
std::printf("8 single-device presentation compatibility checks: %d failures\n",failures);return failures?1:0;}
