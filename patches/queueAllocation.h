// RDR2VulkanFG: capacity-aware Streamline queue allocation.
// Original application queue indices, flags, pNext chains and priorities stay intact.
#pragma once
#include <vector>
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace sl::interposer {
struct QueueAllocation {
    uint32_t family = UINT32_MAX;
    uint32_t index = 0;
    VkDeviceQueueCreateFlags flags = 0;
};
struct QueueAllocationPlan {
    std::vector<VkDeviceQueueCreateInfo> infos;
    std::vector<std::vector<float>> priorities;
    QueueAllocation graphics, compute, optical;

    bool Build(const VkDeviceCreateInfo& host,
               const std::vector<VkQueueFamilyProperties>& families,
               uint32_t preferredGraphics, uint32_t preferredCompute,
               uint32_t opticalFamily, uint32_t graphicsCount,
               uint32_t computeCount, uint32_t opticalCount) {
        infos.clear(); priorities.clear(); graphics={}; compute={}; optical={};
        // Reserve before storing data pointers; no inner-vector is subsequently
        // resized except the entry receiving this allocation.
        infos.reserve(host.queueCreateInfoCount+3);
        priorities.reserve(host.queueCreateInfoCount+3);
        std::vector<uint32_t> used(families.size(),0);
        for(uint32_t i=0;i<host.queueCreateInfoCount;++i) {
            const auto& q=host.pQueueCreateInfos[i];
            if(q.queueFamilyIndex>=families.size() || !q.queueCount || !q.pQueuePriorities ||
               q.queueCount>families[q.queueFamilyIndex].queueCount-used[q.queueFamilyIndex])return false;
            for(const auto& prior:infos)
                if(prior.queueFamilyIndex==q.queueFamilyIndex && prior.flags==q.flags)return false;
            used[q.queueFamilyIndex]+=q.queueCount;
            infos.push_back(q);
            priorities.emplace_back(q.pQueuePriorities,q.pQueuePriorities+q.queueCount);
            infos.back().pQueuePriorities=priorities.back().data();
        }
        auto fits=[&](uint32_t family,uint32_t count,VkQueueFlags capability) {
            return family<families.size() &&
                (families[family].queueFlags&capability)==capability &&
                count<=families[family].queueCount-used[family];
        };
        auto append=[&](QueueAllocation& out,uint32_t family,uint32_t count) {
            out.family=family; out.flags=0;
            for(size_t i=0;i<infos.size();++i) {
                auto& q=infos[i];
                if(q.queueFamilyIndex!=family || q.flags!=0)continue;
                out.index=q.queueCount;
                priorities[i].resize(q.queueCount+count,0.0f);
                q.queueCount+=count; q.pQueuePriorities=priorities[i].data();
                used[family]+=count; return;
            }
            VkDeviceQueueCreateInfo q{};
            q.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            q.queueFamilyIndex=family; q.queueCount=count;
            priorities.emplace_back(count,0.0f);
            q.pQueuePriorities=priorities.back().data();
            infos.push_back(q); used[family]+=count;
        };
        auto allocate=[&](QueueAllocation& out,uint32_t preferred,uint32_t count,VkQueueFlags capability) {
            if(!count){out.family=preferred;return true;}
            if(fits(preferred,count,capability)){append(out,preferred,count);return true;}
            // Prefer an otherwise unused compute-only family before a graphics
            // family. A graphics queue is compute-capable when advertised so.
            if(capability==VK_QUEUE_COMPUTE_BIT)
                for(uint32_t i=0;i<families.size();++i)
                    if(i!=opticalFamily && !(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) && fits(i,count,capability)) {
                        append(out,i,count);return true;
                    }
            for(uint32_t i=0;i<families.size();++i)
                if(i!=opticalFamily && fits(i,count,capability)){append(out,i,count);return true;}
            return false;
        };
        if(!allocate(graphics,preferredGraphics,graphicsCount,VK_QUEUE_GRAPHICS_BIT) ||
           !allocate(compute,preferredCompute,computeCount,VK_QUEUE_COMPUTE_BIT))return false;
        if(opticalCount) {
            if(!fits(opticalFamily,opticalCount,VK_QUEUE_OPTICAL_FLOW_BIT_NV))return false;
            append(optical,opticalFamily,opticalCount);
        } else optical.family=opticalFamily;
        return true;
    }
};
}
