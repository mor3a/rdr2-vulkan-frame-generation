#include "../src/xess_queue_contract.h"
using fgvk::xess_bridge::SelectGraphicsFamily;
constexpr bool QueueRegression(){
  VkQueueFamilyProperties capabilities[3]{};
  capabilities[0].queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT|VK_QUEUE_TRANSFER_BIT;
  capabilities[1].queueFlags=VK_QUEUE_TRANSFER_BIT;
  capabilities[2].queueFlags=VK_QUEUE_COMPUTE_BIT|VK_QUEUE_TRANSFER_BIT;
  VkDeviceQueueCreateInfo requested[3]{};
  requested[0].queueFamilyIndex=0;requested[0].queueCount=1;
  requested[1].queueFamilyIndex=2;requested[1].queueCount=8;
  requested[2].queueFamilyIndex=1;requested[2].queueCount=2;
  if(SelectGraphicsFamily(requested,3,capabilities,3)!=0)return false;
  // Graphics need not be device queue description zero.
  auto tmp=requested[0];requested[0]=requested[1];requested[1]=tmp;
  if(SelectGraphicsFamily(requested,3,capabilities,3)!=0)return false;
  requested[1].flags=VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
  if(SelectGraphicsFamily(requested,3,capabilities,3)!=UINT32_MAX)return false;
  requested[1].flags=0;requested[1].queueCount=0;
  if(SelectGraphicsFamily(requested,3,capabilities,3)!=UINT32_MAX)return false;
  requested[1].queueCount=1;requested[1].queueFamilyIndex=99;
  if(SelectGraphicsFamily(requested,3,capabilities,3)!=UINT32_MAX)return false;
  // Selection leaves every game's queue request unchanged.
  return requested[0].queueFamilyIndex==2 && requested[0].queueCount==8 &&
    requested[2].queueFamilyIndex==1 && requested[2].queueCount==2;
}
static_assert(QueueRegression(),"XeSS must preserve multi-family requests and choose a real unprotected graphics queue");
constexpr bool AcquireRegression(){
  using fgvk::xess_bridge::SelectAcquireSlot;
  uint64_t retired[]={7,0,3};bool acquired[]={false,false,false};
  if(SelectAcquireSlot(retired,acquired,3,0,3)!=1)return false;
  acquired[1]=true;if(SelectAcquireSlot(retired,acquired,3,0,3)!=2)return false;
  retired[0]=9;retired[2]=6;if(SelectAcquireSlot(retired,acquired,3,0,3)!=2)return false;
  acquired[0]=acquired[2]=true;
  if(SelectAcquireSlot(retired,acquired,3,0,100)!=UINT32_MAX)return false;
  return SelectAcquireSlot(retired,acquired,0,0,100)==UINT32_MAX;
}
static_assert(AcquireRegression(),"XeSS acquire must prefer retired slots and preserve pending/acquired images");
