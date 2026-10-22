#include "resource_state.h"
#include <cassert>
#include <cstdio>
namespace fgvk {void HudCaptureForgetCommand(VkCommandBuffer){}}
int main(){using namespace fgvk;
 auto cmd=(VkCommandBuffer)1;auto depth=(VkImage)2;auto color=(VkImage)3;auto view=(VkImageView)4;auto fb=(VkFramebuffer)5;auto pass=(VkRenderPass)6;
 VkImageSubresourceRange d{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1},c{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 resource_detail::images[depth]={VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_USAGE_SAMPLED_BIT,VK_FORMAT_D32_SFLOAT,1280,720};
 resource_detail::images[color]={VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_FORMAT_R8G8B8A8_UNORM,1920,1080};
 resource_detail::views[view]={color,c};resource_detail::framebuffers[fb]={view};resource_detail::passes[pass]={VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
 ObserveBarrier(cmd,depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,d);ObserveBarrier(cmd,color,VK_IMAGE_LAYOUT_GENERAL,c);
 VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};bi.framebuffer=fb;bi.renderPass=pass;
 ObservePassBegin(cmd,&bi);ImageState state{};assert(FindImageState(cmd,depth,state,VK_IMAGE_ASPECT_DEPTH_BIT));assert(!FindImageState(cmd,color,state));
 ObservePassEnd(cmd);assert(FindImageState(cmd,color,state));assert(state.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);assert(FindImageState(cmd,depth,state));
 CommitCommands(1,&cmd);assert(FindImageState((VkCommandBuffer)7,depth,state));
 bi.framebuffer=(VkFramebuffer)999;ObservePassBegin(cmd,&bi);assert(resource_detail::commands.find(cmd)==resource_detail::commands.end());
 ForgetCommand(cmd);assert(resource_detail::activePasses.find(cmd)==resource_detail::activePasses.end());
 auto child=(VkCommandBuffer)8;ObserveBarrier(child,depth,VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,d);ObserveExecuted(cmd,1,&child);assert(FindImageState(cmd,depth,state));assert(state.layout==VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
 VkImageView concrete=view;VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};attachments.attachmentCount=1;attachments.pAttachments=&concrete;bi.pNext=&attachments;bi.renderPass=pass;ObservePassBegin(cmd,&bi);ObservePassEnd(cmd);assert(FindImageState(cmd,color,state));
 assert(ReadableLayout(VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL));
 puts("Render-pass tracking: unrelated depth preserved, attachment invalidated then final-layout observed, submitted fallback and unknown-pass conservative path: PASS");
}
