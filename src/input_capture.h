#pragma once
#include <vulkan/vulkan.h>
#include <sl_consts.h>
namespace fgvk {
// Local diagnostic capture, armed by a one-shot marker file. No shipping INI option.
void InputCaptureRecord(VkCommandBuffer,uint32_t,VkImage,VkImage,uint32_t,uint32_t,uint32_t,uint32_t,VkImageLayout,VkImageLayout,const sl::Constants&);
void InputCaptureExecuted(VkCommandBuffer,uint32_t,const VkCommandBuffer*);
void InputCaptureForget(VkCommandBuffer);
void InputCaptureSubmitted(VkQueue,uint32_t,const VkCommandBuffer*);
void InputCapturePoll();
}
