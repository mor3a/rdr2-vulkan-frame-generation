#pragma once
#include <vulkan/vulkan.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace fgvk::xess_bridge {
// Queue host access is externally synchronized. Keep independent queues
// independent: a present may wait for work still being submitted on compute.
inline std::mutex queueRegistryMutex;
inline std::map<VkQueue,std::shared_ptr<std::recursive_mutex>> queueLocks;
inline std::shared_ptr<std::recursive_mutex> QueueMutex(VkQueue queue){
  std::lock_guard<std::mutex> guard(queueRegistryMutex);
  auto& item=queueLocks[queue];if(!item)item=std::make_shared<std::recursive_mutex>();
  return item;
}
class QueueGuard {
  std::shared_ptr<std::recursive_mutex> mutex_;
public:
  explicit QueueGuard(VkQueue queue,bool enabled=true){if(enabled && queue){mutex_=QueueMutex(queue);mutex_->lock();}}
  void Unlock(){if(mutex_){mutex_->unlock();mutex_.reset();}}
  ~QueueGuard(){Unlock();}
  QueueGuard(const QueueGuard&)=delete;
};
class AllQueuesGuard {
  std::vector<std::shared_ptr<std::recursive_mutex>> mutexes_;
public:
  explicit AllQueuesGuard(bool enabled=true){
    if(!enabled)return;
    {std::lock_guard<std::mutex> guard(queueRegistryMutex);for(auto& item:queueLocks)mutexes_.push_back(item.second);}
    for(auto& mutex:mutexes_)mutex->lock();
  }
  ~AllQueuesGuard(){for(auto it=mutexes_.rbegin();it!=mutexes_.rend();++it)(*it)->unlock();}
  AllQueuesGuard(const AllQueuesGuard&)=delete;
};
}
