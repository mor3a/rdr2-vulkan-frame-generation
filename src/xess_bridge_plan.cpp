#include "xess_bridge_plan.h"
#include <cstring>

namespace fgvk::xess_bridge {
bool DevicePlan::Prepare(VkInstance instance,VkPhysicalDevice physical,
    const VkDeviceCreateInfo* input,PFN_vkGetInstanceProcAddr gi){
  ready_=false;extensions_.clear();featureCopies_.clear();
  if(!instance || !physical || !input || !gi ||
     (input->enabledExtensionCount && !input->ppEnabledExtensionNames))return Fail("Invalid Vulkan device plan");
  auto enumerate=(PFN_vkEnumerateDeviceExtensionProperties)gi(instance,"vkEnumerateDeviceExtensionProperties");
  auto features=(PFN_vkGetPhysicalDeviceFeatures2)gi(instance,"vkGetPhysicalDeviceFeatures2");
  auto semaphore=(PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gi(instance,"vkGetPhysicalDeviceExternalSemaphoreProperties");
  if(!enumerate || !features || !semaphore)return Fail("External semaphore capability queries unavailable");
  uint32_t count=0;
  if(enumerate(physical,nullptr,&count,nullptr)!=VK_SUCCESS)return Fail("Device extension count query failed");
  std::vector<VkExtensionProperties> available(count);
  if(enumerate(physical,nullptr,&count,available.data())!=VK_SUCCESS)return Fail("Device extension query failed");
  const char* required[]={"VK_KHR_external_memory_win32","VK_KHR_external_semaphore_win32","VK_KHR_timeline_semaphore"};
  for(auto name:required){
    bool found=false;for(uint32_t i=0;i<count;++i)if(!std::strcmp(available[i].extensionName,name)){found=true;break;}
    if(!found)return Fail(name);
  }
  VkPhysicalDeviceTimelineSemaphoreFeatures supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  VkPhysicalDeviceFeatures2 supported2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};supported2.pNext=&supported;features(physical,&supported2);
  if(!supported.timelineSemaphore)return Fail("Timeline semaphores are not supported by this Vulkan device");
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
  VkPhysicalDeviceExternalSemaphoreInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
  query.pNext=&type;query.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
  VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};semaphore(physical,&query,&properties);
  if(!(properties.externalSemaphoreFeatures&VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
    return Fail("D3D12 shared fences cannot be imported as Vulkan timeline semaphores");
  bool alreadyRequested=false;
  void* clonedHead=nullptr;VkBaseOutStructure* clonedTail=nullptr;
  for(auto next=(const VkBaseInStructure*)input->pNext;next;next=next->pNext){
    auto size=[](VkStructureType type)->size_t{switch(type){
#include "xess_feature_sizes.inc"
        default:return 0;}};size_t bytes=size(next->sType);
    if(!bytes)return Fail("Unknown Vulkan device pNext structure; original chain preserved");
    auto storage=std::make_unique<unsigned char[]>(bytes);std::memcpy(storage.get(),next,bytes);
    auto copy=(VkBaseOutStructure*)storage.get();copy->pNext=nullptr;
    if(clonedTail)clonedTail->pNext=copy;else clonedHead=copy;clonedTail=copy;
    if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES){
      ((VkPhysicalDeviceTimelineSemaphoreFeatures*)copy)->timelineSemaphore=VK_TRUE;
      alreadyRequested=true;
    }else if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES){
      ((VkPhysicalDeviceVulkan12Features*)copy)->timelineSemaphore=VK_TRUE;
      alreadyRequested=true;
    }
    featureCopies_.push_back(std::move(storage));
  }
  if(input->enabledExtensionCount)
    extensions_.assign(input->ppEnabledExtensionNames,input->ppEnabledExtensionNames+input->enabledExtensionCount);
  for(auto name:required){
    bool found=false;for(auto requested:extensions_)if(!std::strcmp(name,requested)){found=true;break;}
    if(!found)extensions_.push_back(name);
  }
  info_=*input;info_.enabledExtensionCount=(uint32_t)extensions_.size();info_.ppEnabledExtensionNames=extensions_.data();
  info_.pNext=clonedHead;
  if(!alreadyRequested){timeline_.timelineSemaphore=VK_TRUE;timeline_.pNext=clonedHead;info_.pNext=&timeline_;}
  ready_=true;error_="";return true;
}
}
