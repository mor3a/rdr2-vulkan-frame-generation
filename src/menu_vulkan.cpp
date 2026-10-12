#include "menu.h"
#include "log.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <windows.h>
#include <vector>
namespace fgvk {
#define VK_FUNCS(X) \
 X(CreateRenderPass) X(DestroyRenderPass) X(CreateImageView) X(DestroyImageView) \
 X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CreateSemaphore) X(DestroySemaphore) \
 X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)
struct Functions {
#define FIELD(n) PFN_vk##n n{};
 VK_FUNCS(FIELD)
#undef FIELD
};
struct Frame {VkImageView view{};VkFramebuffer framebuffer{};VkCommandBuffer cmd{};VkFence fence{};VkSemaphore ready{};};
static Functions vk;static VkDevice device{};static VkPhysicalDevice physical{};static VkInstance instance{};
static VkSwapchainKHR chain{};static VkExtent2D extent{};static VkFormat format{};static uint32_t family{};
static VkRenderPass renderPass{};static VkCommandPool commandPool{};static std::vector<Frame> frames;
static PFN_vkGetDeviceProcAddr nativeGDPA{};static PFN_vkGetSwapchainImagesKHR gameImages{};
static bool rendererReady{},disabled{};
static PFN_vkVoidFunction LoadFunction(const char* name,void*){
  auto fn=nativeGDPA?nativeGDPA(device,name):nullptr;
  if(!fn)fn=(PFN_vkVoidFunction)GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),name);
  return fn;
}
static bool Check(VkResult r,const char* what){if(r==VK_SUCCESS)return true;Log("Vulkan menu: %s failed=%d; native game present retained",what,r);return false;}
static void BackendError(VkResult r){if(r!=VK_SUCCESS)Log("ImGui Vulkan result=%d",r);}
void MenuDestroySwapchain(VkDevice d,VkSwapchainKHR sc){
  if(d!=device || (sc && sc!=chain))return;
  MenuRendererAvailable(false);
  if(vk.DeviceWaitIdle)vk.DeviceWaitIdle(device);
  if(rendererReady){ImGui_ImplVulkan_Shutdown();rendererReady=false;}
  for(auto& f:frames){if(f.framebuffer)vk.DestroyFramebuffer(device,f.framebuffer,nullptr);if(f.view)vk.DestroyImageView(device,f.view,nullptr);if(f.ready)vk.DestroySemaphore(device,f.ready,nullptr);if(f.fence)vk.DestroyFence(device,f.fence,nullptr);}
  frames.clear();if(commandPool)vk.DestroyCommandPool(device,commandPool,nullptr);if(renderPass)vk.DestroyRenderPass(device,renderPass,nullptr);
  commandPool={};renderPass={};chain={};disabled=false;
}
void MenuRegisterSwapchain(VkDevice d,VkPhysicalDevice pd,VkInstance inst,uint32_t queueFamily,VkSwapchainKHR sc,const VkSwapchainCreateInfoKHR* ci,PFN_vkGetDeviceProcAddr gdpa,PFN_vkGetSwapchainImagesKHR images){
  if(chain)MenuDestroySwapchain(device,chain);
  device=d;physical=pd;instance=inst;family=queueFamily;chain=sc;extent=ci->imageExtent;format=ci->imageFormat;nativeGDPA=gdpa;gameImages=images;disabled=false;
  if(!(ci->imageUsage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || ci->imageArrayLayers!=1){disabled=true;Log("Vulkan menu: unsupported swapchain usage/layers; game unaffected");return;}
#define LOAD(n) vk.n=(PFN_vk##n)nativeGDPA(device,"vk" #n);if(!vk.n){disabled=true;Log("Vulkan menu: missing vk" #n);return;}
 VK_FUNCS(LOAD)
#undef LOAD
  Log("Vulkan menu: registered game swapchain %ux%u format=%d",extent.width,extent.height,format);
}
static bool Initialize(VkQueue queue){
  if(!MenuInitializeUI() || !gameImages)return false;
  uint32_t count{};if(!Check(gameImages(device,chain,&count,nullptr),"image query") || count<2 || count>8)return false;
  std::vector<VkImage> images(count);if(!Check(gameImages(device,chain,&count,images.data()),"images"))return false;
  VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
  VkSubpassDependency dependencies[2]{};dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependencies[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=dependencies;
  if(!Check(vk.CreateRenderPass(device,&rp,nullptr,&renderPass),"render pass"))return false;
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=family;
  if(!Check(vk.CreateCommandPool(device,&pool,nullptr,&commandPool),"command pool"))return false;
  frames.resize(count);
  for(uint32_t i=0;i<count;++i){auto& f=frames[i];
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=images[i];view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(!Check(vk.CreateImageView(device,&view,nullptr,&f.view),"image view"))return false;
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=renderPass;fb.attachmentCount=1;fb.pAttachments=&f.view;fb.width=extent.width;fb.height=extent.height;fb.layers=1;
    if(!Check(vk.CreateFramebuffer(device,&fb,nullptr,&f.framebuffer),"framebuffer"))return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};alloc.commandPool=commandPool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;
    if(!Check(vk.AllocateCommandBuffers(device,&alloc,&f.cmd),"command buffer"))return false;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    if(!Check(vk.CreateFence(device,&fence,nullptr,&f.fence),"fence"))return false;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};if(!Check(vk.CreateSemaphore(device,&sem,nullptr,&f.ready),"semaphore"))return false;
  }
  if(!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1,LoadFunction))return false;
  ImGui_ImplVulkan_InitInfo info{};info.ApiVersion=VK_API_VERSION_1_1;info.Instance=instance;info.PhysicalDevice=physical;info.Device=device;info.QueueFamily=family;info.Queue=queue;info.RenderPass=renderPass;info.MinImageCount=count;info.ImageCount=count;info.MSAASamples=VK_SAMPLE_COUNT_1_BIT;info.DescriptorPoolSize=32;info.CheckVkResultFn=BackendError;
  if(!ImGui_ImplVulkan_Init(&info))return false;
  rendererReady=true;MenuRendererAvailable(true);Log("Vulkan menu renderer initialized: %u game images",count);return true;
}
bool MenuRender(VkQueue queue,const VkPresentInfoKHR* original,VkPresentInfoKHR& modified){
  if(disabled || !chain || !original || original->swapchainCount!=1 || original->pSwapchains[0]!=chain)return false;
  if(!rendererReady && !Initialize(queue)){disabled=true;MenuRendererAvailable(false);Log("Vulkan menu initialization failed; rendering skipped");return false;}
  if(!MenuHasDrawing())return false;
  uint32_t index=original->pImageIndices[0];if(index>=frames.size())return false;auto& f=frames[index];
  // UI is optional: skip a busy image instead of stalling the game's present thread.
  if(vk.GetFenceStatus(device,f.fence)!=VK_SUCCESS)return false;
  ImGui_ImplVulkan_NewFrame();MenuBuildUI();auto* draw=ImGui::GetDrawData();if(!draw || !draw->TotalVtxCount)return false;
  if(!Check(vk.ResetCommandBuffer(f.cmd,0),"reset command"))return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if(!Check(vk.BeginCommandBuffer(f.cmd,&begin),"begin command"))return false;
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=renderPass;rp.framebuffer=f.framebuffer;rp.renderArea={{0,0},extent};vk.CmdBeginRenderPass(f.cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);ImGui_ImplVulkan_RenderDrawData(draw,f.cmd,VK_NULL_HANDLE,index);vk.CmdEndRenderPass(f.cmd);
  if(!Check(vk.EndCommandBuffer(f.cmd),"end command") || !Check(vk.ResetFences(device,1,&f.fence),"reset fence"))return false;
  std::vector<VkPipelineStageFlags> stages(original->waitSemaphoreCount,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=original->waitSemaphoreCount;submit.pWaitSemaphores=original->pWaitSemaphores;submit.pWaitDstStageMask=stages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&f.cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&f.ready;
  if(!Check(vk.QueueSubmit(queue,1,&submit,f.fence),"overlay submit")){disabled=true;MenuRendererAvailable(false);return false;}
  modified=*original;modified.waitSemaphoreCount=1;modified.pWaitSemaphores=&f.ready;return true;
}
}
