#include <vulkan/vulkan_core.h>
#include "source/core/sl.interposer/vulkan/queueAllocation.h"
#include <cassert>
#include <cstdio>
using namespace sl::interposer;
int main(){
 std::vector<VkQueueFamilyProperties> families(6);
 families[0].queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;families[0].queueCount=16;
 families[1].queueFlags=VK_QUEUE_TRANSFER_BIT;families[1].queueCount=2;
 families[2].queueFlags=VK_QUEUE_COMPUTE_BIT|VK_QUEUE_TRANSFER_BIT;families[2].queueCount=8;
 families[5].queueFlags=VK_QUEUE_OPTICAL_FLOW_BIT_NV;families[5].queueCount=1;
 float gamePriorities[8]={1.f,.75f,.5f,.25f,0,0,0,0};
 VkDeviceQueueCreateInfo requests[3]{};
 for(auto& q:requests){q.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;q.pQueuePriorities=gamePriorities;}
 requests[0].queueFamilyIndex=0;requests[0].queueCount=1;
 requests[1].queueFamilyIndex=2;requests[1].queueCount=8;
 requests[2].queueFamilyIndex=1;requests[2].queueCount=2;
 VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};ci.queueCreateInfoCount=3;ci.pQueueCreateInfos=requests;
 QueueAllocationPlan plan;
 assert(plan.Build(ci,families,0,2,5,1,2,1));
 assert(plan.graphics.family==0 && plan.graphics.index==1);
 assert(plan.compute.family==0 && plan.compute.index==2);
 assert(plan.optical.family==5 && plan.optical.index==0);
 assert(plan.infos.size()==4 && plan.infos[0].queueCount==4 && plan.infos[1].queueCount==8);
 assert(plan.infos[1].pQueuePriorities[1]==.75f && requests[1].queueCount==8);
 for(size_t i=0;i<plan.infos.size();++i)for(size_t j=i+1;j<plan.infos.size();++j)
   assert(plan.infos[i].queueFamilyIndex!=plan.infos[j].queueFamilyIndex || plan.infos[i].flags!=plan.infos[j].flags);
 // With space in the dedicated family, retain its selection and host indices.
 requests[1].queueCount=4;
 assert(plan.Build(ci,families,0,2,5,1,2,1));
 assert(plan.compute.family==2 && plan.compute.index==4 && plan.infos[1].queueCount==6);
 // Preserve the host's pNext chain and priority values.
 VkDeviceQueueGlobalPriorityCreateInfo priority{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO};
 requests[0].pNext=&priority;
 assert(plan.Build(ci,families,0,2,5,1,2,1));assert(plan.infos[0].pNext==&priority);
 // No capacity must fail instead of manufacturing duplicate or excess queues.
 requests[1].queueCount=8;families[0].queueCount=2;
 assert(!plan.Build(ci,families,0,2,5,1,2,1));
 // Protected host queues remain separate from unprotected SDK queues.
 families[0].queueCount=16;requests[1].flags=VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;requests[1].queueCount=4;
 assert(plan.Build(ci,families,0,2,5,1,2,1));
 assert(plan.compute.family==2 && plan.compute.index==0 && plan.compute.flags==0);
 assert(plan.infos[1].flags==VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT && plan.infos[1].queueCount==4);
 // Duplicate host requests themselves are rejected.
 requests[2]=requests[1];assert(!plan.Build(ci,families,0,2,5,1,2,1));
 puts("Native queue allocation regressions passed.");
}
