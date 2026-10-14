#pragma once
#include <atomic>
namespace fgvk {
struct VulkanOwner {
  std::atomic<bool> instanceReserved{false},deviceReserved{false};
  bool ClaimInstance(){bool free=false;return instanceReserved.compare_exchange_strong(free,true);}
  bool ClaimDevice(){bool free=false;return deviceReserved.compare_exchange_strong(free,true);}
  void InstanceFailed(){instanceReserved.store(false);}
  void DeviceFailed(){deviceReserved.store(false);}
};
}
