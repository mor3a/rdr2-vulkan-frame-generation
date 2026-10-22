#pragma once
#include <vulkan/vulkan.h>
#include <sl_consts.h>
namespace fgvk {
bool RecordRenderReprojection(VkCommandBuffer,uint32_t,VkImage,VkFormat,VkImage,VkFormat,uint32_t,uint32_t,VkImageLayout,VkImageLayout,const sl::Constants&,uint32_t depthX=0,uint32_t depthY=0,uint32_t motionX=0,uint32_t motionY=0);
bool ResolveRenderReprojection(uint32_t,sl::Constants&,bool auditOnly=false);
struct ReprojectionMetrics {uint32_t token{};double sampledError{},reconstructedError{},gpuWaitMs{},cpuFitMs{};bool valid{};};
ReprojectionMetrics ReadReprojectionMetrics();
void CommitRenderReprojection(uint32_t,bool);
void CancelRenderReprojection(uint32_t);
void RenderReprojectionExecuted(VkCommandBuffer,uint32_t,const VkCommandBuffer*);
void RenderReprojectionForget(VkCommandBuffer);
void RenderReprojectionSubmitted(VkQueue,uint32_t,const VkCommandBuffer*);
}
