#include "xess_bridge_handoff.h"
#include <vector>

namespace fgvk::xess_bridge {
bool Handoff::Initialize(Device& bridge,VkDevice device,PFN_vkGetDeviceProcAddr gd,ID3D12CommandQueue* queue){
  if(bridge_ || !device || !gd || !queue)return Fail("Invalid handoff initialization");
  if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)
    return Fail("XeSS presentation requires the app direct command queue");
  auto submit=(PFN_vkQueueSubmit)gd(device,"vkQueueSubmit");
  if(!submit)return Fail("Native vkQueueSubmit unavailable");
  if(!bridge.CreateFence(produced_))return Fail(bridge.Error(),bridge.NativeResult());
  if(!bridge.CreateFence(consumed_)){bridge.DestroyFence(produced_);return Fail(bridge.Error(),bridge.NativeResult());}
  bridge_=&bridge;device_=device;submit_=submit;queue_=queue;queue_->AddRef();
  serial_=0;pending_=failed_=false;error_="";native_=0;return true;
}
bool Handoff::SubmitSnapshot(VkQueue queue,VkCommandBuffer command,uint32_t count,
    const VkSemaphore* binaryWaits,const VkPipelineStageFlags* stages){
  if(!bridge_ || !queue || !command || pending_ || failed_ || serial_==UINT64_MAX ||
     (count && (!binaryWaits || !stages)) || count>256)return Fail("Invalid or unfinished handoff submission");
  std::vector<VkSemaphore> waits;std::vector<VkPipelineStageFlags> waitStages;
  std::vector<uint64_t> waitValues;
  waits.push_back(consumed_.vk);waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);waitValues.push_back(serial_);
  for(uint32_t i=0;i<count;++i){
    if(!binaryWaits[i] || !stages[i])return Fail("Invalid binary semaphore wait");
    waits.push_back(binaryWaits[i]);waitStages.push_back(stages[i]);waitValues.push_back(0);
  }
  const uint64_t next=serial_+1;
  VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline.waitSemaphoreValueCount=(uint32_t)waits.size();timeline.pWaitSemaphoreValues=waitValues.data();
  timeline.signalSemaphoreValueCount=1;timeline.pSignalSemaphoreValues=&next;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;
  submit.waitSemaphoreCount=(uint32_t)waits.size();submit.pWaitSemaphores=waits.data();submit.pWaitDstStageMask=waitStages.data();
  submit.commandBufferCount=1;submit.pCommandBuffers=&command;
  submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&produced_.vk;
  VkResult result=submit_(queue,1,&submit,VK_NULL_HANDLE);
  if(result!=VK_SUCCESS){failed_=true;return Fail("Vulkan snapshot submission failed",result);}
  serial_=next;pending_=true;error_="";native_=0;return true;
}
bool Handoff::ConsumeCopies(ID3D12CommandList* list){
  if(!bridge_ || !list || !pending_ || failed_)return Fail("DX12 copies require a submitted Vulkan snapshot");
  HRESULT result=queue_->Wait(produced_.dx,serial_);
  if(FAILED(result)){failed_=true;return Fail("DX12 wait on Vulkan snapshot failed",result);}
  // The caller records SDK ONLY_NOW copies before closing this command list.
  // ExecuteCommandLists preserves app-queue ordering with the consumer signal.
  queue_->ExecuteCommandLists(1,&list);
  result=queue_->Signal(consumed_.dx,serial_);
  if(FAILED(result)){failed_=true;return Fail("DX12 consumer completion signal failed",result);}
  pending_=false;error_="";native_=0;return true;
}
bool Handoff::DestroyIfRetired(){
  if(!bridge_)return true;
  const uint64_t producer=produced_.dx->GetCompletedValue();
  const uint64_t consumer=consumed_.dx->GetCompletedValue();
  if(pending_ || producer==UINT64_MAX || consumer==UINT64_MAX || producer<serial_ || consumer<serial_)
    return Fail("Handoff GPU work is not retired; resources retained");
  bridge_->DestroyFence(produced_);bridge_->DestroyFence(consumed_);
  queue_->Release();queue_=nullptr;bridge_=nullptr;device_=VK_NULL_HANDLE;submit_=nullptr;
  serial_=0;pending_=failed_=false;return true;
}
}
