#pragma once
#include "xess_bridge_device.h"

namespace fgvk::xess_bridge {
// One instance per reusable transport slot. All calls and Vulkan queue access
// must be externally serialized. No CPU wait, device idle or readback per frame.
class Handoff {
public:
  Handoff()=default;
  Handoff(const Handoff&)=delete;
  Handoff& operator=(const Handoff&)=delete;
  bool Initialize(Device&,VkDevice,PFN_vkGetDeviceProcAddr,ID3D12CommandQueue*);
  bool SubmitSnapshot(VkQueue,VkCommandBuffer,uint32_t binaryWaitCount=0,
      const VkSemaphore* binaryWaits=nullptr,const VkPipelineStageFlags* waitStages=nullptr);
  // List must contain transient-input copies recorded by XeFG ONLY_NOW tags
  // and any final-color copy into an SDK-owned backbuffer. External images must
  // end in COMMON. Never use this fence to retire resources still referenced
  // directly by XeFG's internal queues (UNTIL_NEXT_PRESENT lifetime).
  bool ConsumeCopies(ID3D12CommandList*);
  bool DestroyIfRetired();
  uint64_t Serial() const{return serial_;}
  const char* Error() const{return error_;}
  int64_t NativeResult() const{return native_;}
private:
  bool Fail(const char* error,int64_t native=0){error_=error;native_=native;return false;}
  Device* bridge_{};VkDevice device_{};PFN_vkQueueSubmit submit_{};
  ID3D12CommandQueue* queue_{};SharedFence produced_{},consumed_{};
  uint64_t serial_{};bool pending_{},failed_{};
  const char* error_="Not initialized";int64_t native_{};
};
}
