#include "menu.h"
#include "config.h"
#include "xess_queue_sync.h"
#include "swapchain_lock.h"
#include "menu_startup.h"
#include "log.h"
#include "runtime_errors.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <windows.h>
#include <vector>
#include <array>
#include <exception>
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
static unsigned initFailures{};static ULONGLONG retryAfter{};static VkResult backendResult=VK_SUCCESS;
static MenuStartup startup;
struct OverlayBackendFailure {VkResult result;};
static VkQueue rendererQueue{};
static VkFence retirementFence{};
static bool recoveryPending{},resetUIAfterRecovery{};
static std::vector<VkImage> capturedImages;
void MenuCaptureImages(VkDevice d,VkSwapchainKHR sc,uint32_t count,const VkImage* images){
  SwapchainScope lifecycle;
  if(d!=device || sc!=chain || rendererReady || !images || count<2 || count>8)return;
  capturedImages.assign(images,images+count);
  Log("Vulkan menu: captured %u images returned to the game",count);
}
void MenuPresentCompleted(bool evaluated,VkResult result){
  SwapchainScope lifecycle;
  startup.Presented(evaluated,result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR);
}
static PFN_vkVoidFunction LoadFunction(const char* name,void*){
  auto fn=nativeGDPA?nativeGDPA(device,name):nullptr;
  if(!fn)fn=(PFN_vkVoidFunction)GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),name);
  return fn;
}
static bool Check(VkResult r,const char* what){
  if(r==VK_SUCCESS)return true;
  backendResult=r;
  RuntimeErrors::ReportError(rendererReady?RuntimeErrors::Code::MenuRender:RuntimeErrors::Code::MenuInit,what,(int)r);
  if(r==VK_ERROR_DEVICE_LOST){disabled=true;MenuRendererAvailable(false);RuntimeErrors::ReportError(RuntimeErrors::Code::VulkanDevice,"Overlay operation reported device lost",(int)r);}
  Log("Vulkan menu: %s failed=%d; optional drawing skipped",what,r);return false;
}
// Stop at the first failed backend operation. Reporting alone lets upstream
// ImGui continue with invalid handles or a failed mapped-memory pointer.
// This is a local C++ unwind, never an attempt to resume an access violation.
static void BackendError(VkResult r){if(r<0){backendResult=r;Check(r,"ImGui Vulkan backend");throw OverlayBackendFailure{r};}}
static void ReleaseResources(){
  MenuRendererAvailable(false);
  if(ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)ImGui_ImplVulkan_Shutdown();
  rendererReady=false;
  for(auto& f:frames){if(f.framebuffer)vk.DestroyFramebuffer(device,f.framebuffer,nullptr);if(f.view)vk.DestroyImageView(device,f.view,nullptr);if(f.ready)vk.DestroySemaphore(device,f.ready,nullptr);if(f.fence)vk.DestroyFence(device,f.fence,nullptr);}
  frames.clear();if(commandPool)vk.DestroyCommandPool(device,commandPool,nullptr);if(renderPass)vk.DestroyRenderPass(device,renderPass,nullptr);
  commandPool={};renderPass={};
}
void MenuDestroySwapchain(VkDevice d,VkSwapchainKHR sc){
  LogDebug("overlay teardown requested: device=%p swapchain=%p",(void*)d,(void*)sc);
  SwapchainScope lifecycle;
  if(d!=device || (sc && sc!=chain))return;
  startup.Reset();
  MenuRendererAvailable(false);
  // Registration alone owns no GPU work. Waiting for the whole device here
  // can stall startup even though there is no overlay resource to retire.
  if((rendererReady || commandPool || renderPass || !frames.empty()) && vk.DeviceWaitIdle){LogDebug("overlay GPU idle wait begin: frames=%u",(unsigned)frames.size());auto result=vk.DeviceWaitIdle(device);LogDebug("overlay GPU idle wait end: result=%d",(int)result);}
  ReleaseResources();
  if(retirementFence)vk.DestroyFence(device,retirementFence,nullptr);
  retirementFence={};recoveryPending=resetUIAfterRecovery=false;rendererQueue={};
  capturedImages.clear();
  chain={};disabled=false;initFailures=0;retryAfter=0;
  LogDebug("overlay teardown completed");
}
void MenuRegisterSwapchain(VkDevice d,VkPhysicalDevice pd,VkInstance inst,uint32_t queueFamily,VkSwapchainKHR sc,const VkSwapchainCreateInfoKHR* ci,PFN_vkGetDeviceProcAddr gdpa,PFN_vkGetSwapchainImagesKHR images){
  SwapchainScope lifecycle;
  if(!d || !pd || !inst || !sc || !ci || !ci->imageExtent.width || !ci->imageExtent.height){RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInit,"Invalid swapchain registration arguments; overlay skipped");return;}
  if(chain)MenuDestroySwapchain(device,chain);
  startup.Reset();
  device=d;physical=pd;instance=inst;family=queueFamily;chain=sc;extent=ci->imageExtent;format=ci->imageFormat;nativeGDPA=gdpa;gameImages=images;disabled=false;
  if(!(ci->imageUsage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || ci->imageArrayLayers!=1){disabled=true;RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInit,"Swapchain lacks color attachment usage or has multiple layers");Log("Vulkan menu: unsupported swapchain usage/layers; game unaffected");return;}
  if(!nativeGDPA){disabled=true;RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInit,"Native Vulkan device procedure resolver unavailable");return;}
#define LOAD(n) vk.n=(PFN_vk##n)nativeGDPA(device,"vk" #n);if(!vk.n){disabled=true;RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInit,"Missing Vulkan function: vk" #n);Log("Vulkan menu: missing vk" #n);return;}
 VK_FUNCS(LOAD)
#undef LOAD
  Log("Vulkan menu: registered game swapchain %ux%u format=%d",extent.width,extent.height,format);
}
static bool Initialize(VkQueue queue){
  backendResult=VK_SUCCESS;
  rendererQueue=queue;
  if(!MenuInitializeUI() || !gameImages)return false;
  // Use the images already returned to the host. A later proxy query can expose
  // a different set after FG options change, which the host does not render into.
  if(capturedImages.empty())return false;
  std::vector<VkImage> images=capturedImages;uint32_t count=(uint32_t)images.size();
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
  if(!ImGui_ImplVulkan_Init(&info) || backendResult!=VK_SUCCESS)return false;
  rendererReady=true;initFailures=0;RuntimeErrors::ClearError(RuntimeErrors::Code::MenuInit);MenuRendererAvailable(true);Log("Vulkan menu renderer initialized: %u game images",count);return true;
}
static bool RenderOverlay(VkQueue queue,const VkPresentInfoKHR* original,VkPresentInfoKHR& modified,bool& submitted){
  if(disabled || !queue || !chain || !original || original->swapchainCount!=1 || !original->pSwapchains || !original->pImageIndices || original->pSwapchains[0]!=chain)return false;
  if(original->waitSemaphoreCount>64 || (original->waitSemaphoreCount && !original->pWaitSemaphores)){RuntimeErrors::ReportError(RuntimeErrors::Code::MenuRender,"Unsupported presentation wait list; original presentation preserved");return false;}
  // Wait for a successful host presentation, never for valid FG inputs.
  if(!startup.ready)return false;
  if(capturedImages.empty())return false;
  if(!rendererReady){
    if(GetTickCount64()<retryAfter)return false;
    if(!Initialize(queue)){
      // Initialization has not submitted overlay work; partial objects can be
      // reclaimed without consuming the game's presentation semaphores.
      ReleaseResources();++initFailures;
      retryAfter=GetTickCount64()+(initFailures<5?1000ull:10000ull);
      RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInit,"Overlay initialization failed; retry scheduled",(int)backendResult);
      Log("Vulkan menu initialization failed; retry=%u delayed; rendering skipped",initFailures);return false;
    }
  }
  if(queue!=rendererQueue){
    RuntimeErrors::ReportError(RuntimeErrors::Code::MenuRender,"Presentation queue changed; retiring the old overlay before rebuilding on the current queue");
    MenuRendererAvailable(false);disabled=true;recoveryPending=true;retryAfter=GetTickCount64();return false;
  }
  if(!MenuHasDrawing())return false;
  uint32_t index=original->pImageIndices[0];if(index>=frames.size()){RuntimeErrors::ReportError(RuntimeErrors::Code::MenuRender,"Presented image index is outside captured swapchain images",(int)index);return false;}auto& f=frames[index];
  // UI is optional: skip a busy image instead of stalling the game's present thread.
  auto fenceStatus=vk.GetFenceStatus(device,f.fence);if(fenceStatus==VK_NOT_READY)return false;if(!Check(fenceStatus,"frame fence status"))return false;
  backendResult=VK_SUCCESS;ImGui_ImplVulkan_NewFrame();if(backendResult!=VK_SUCCESS)return false;MenuBuildUI();auto* draw=ImGui::GetDrawData();if(!draw || !draw->TotalVtxCount)return false;
  if(!Check(vk.ResetCommandBuffer(f.cmd,0),"reset command"))return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if(!Check(vk.BeginCommandBuffer(f.cmd,&begin),"begin command"))return false;
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=renderPass;rp.framebuffer=f.framebuffer;rp.renderArea={{0,0},extent};vk.CmdBeginRenderPass(f.cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);ImGui_ImplVulkan_RenderDrawData(draw,f.cmd,VK_NULL_HANDLE,index);vk.CmdEndRenderPass(f.cmd);
  if(!Check(vk.EndCommandBuffer(f.cmd),"end command") || backendResult!=VK_SUCCESS || !Check(vk.ResetFences(device,1,&f.fence),"reset fence"))return false;
  std::array<VkPipelineStageFlags,64> stages;std::fill_n(stages.begin(),original->waitSemaphoreCount,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=original->waitSemaphoreCount;submit.pWaitSemaphores=original->pWaitSemaphores;submit.pWaitDstStageMask=stages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&f.cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&f.ready;
  modified=*original;modified.waitSemaphoreCount=1;modified.pWaitSemaphores=&f.ready;
  if(!Check(vk.QueueSubmit(queue,1,&submit,f.fence),"overlay submit")){disabled=true;MenuRendererAvailable(false);return false;}
  submitted=true;
  // QueueSubmit consumed the host wait semaphores. The caller MUST present
  // with this replacement wait even if any later optional UI work fails.
  RuntimeErrors::ClearError(RuntimeErrors::Code::MenuRender);return true;
}
static void OverlayFailed(const char* detail,int native,bool resetUI){
  const bool initializing=!rendererReady;
  RuntimeErrors::ReportError(initializing?RuntimeErrors::Code::MenuInit:RuntimeErrors::Code::MenuRender,detail,native);
  Log("%s: %s; native=%d; host presentation preserved",initializing?"UI-101":"UI-102",detail,native);
  MenuRendererAvailable(false);
  if(initializing && backendResult!=VK_ERROR_DEVICE_LOST){
    // Initialization creates objects but never submits GPU work. Retire these
    // partial resources and retry later without touching host semaphores.
    ReleaseResources();if(resetUI)MenuResetUI();++initFailures;retryAfter=GetTickCount64()+10000;
  }else{
    // A backend failure during upload/draw may leave work pending. Do not
    // destroy/reuse those objects; swapchain teardown performs the GPU wait.
    disabled=true;recoveryPending=backendResult!=VK_ERROR_DEVICE_LOST;
    resetUIAfterRecovery=resetUI;retryAfter=GetTickCount64()+5000;
  }
}
static void RecoverOverlay(){
  if(!recoveryPending || GetTickCount64()<retryAfter || !rendererQueue)return;
  // Present already owns the current queue. Never block acquiring an older
  // queue in the opposite order during a queue transition.
  std::shared_ptr<std::recursive_mutex> oldQueue;
  std::unique_lock<std::recursive_mutex> queueGuard;
  if(IsDX12Bridge(Cfg().provider)){
    oldQueue=xess_bridge::QueueMutex(rendererQueue);
    queueGuard=std::unique_lock<std::recursive_mutex>(*oldQueue,std::try_to_lock);
    if(!queueGuard.owns_lock())return;
  }
  if(!retirementFence){
    VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    auto result=vk.CreateFence(device,&create,nullptr,&retirementFence);
    if(result==VK_SUCCESS)result=vk.QueueSubmit(rendererQueue,0,nullptr,retirementFence);
    if(result!=VK_SUCCESS){
      if(retirementFence)vk.DestroyFence(device,retirementFence,nullptr);retirementFence={};
      if(result==VK_ERROR_DEVICE_LOST)recoveryPending=false;
      retryAfter=GetTickCount64()+10000;return;
    }
    return;
  }
  const auto result=vk.GetFenceStatus(device,retirementFence);
  if(result==VK_NOT_READY)return;
  if(result!=VK_SUCCESS){recoveryPending=false;return;}
  // The empty submission consumes no host semaphores. Its fence proves that
  // previous font/upload/draw work on the overlay queue has retired.
  vk.DestroyFence(device,retirementFence,nullptr);retirementFence={};
  ReleaseResources();if(resetUIAfterRecovery)MenuResetUI();
  resetUIAfterRecovery=recoveryPending=disabled=false;retryAfter=GetTickCount64()+1000;
  Log("UI-102: overlay GPU work retired; renderer rebuild scheduled without a device-wide idle wait");
}
bool MenuRender(VkQueue queue,const VkPresentInfoKHR* original,VkPresentInfoKHR& modified){
  SwapchainScope lifecycle;bool submitted=false;
  try{RecoverOverlay();return RenderOverlay(queue,original,modified,submitted);}
  catch(const OverlayBackendFailure& failure){OverlayFailed("ImGui Vulkan operation aborted after a failed native result",(int)failure.result,false);}
  catch(const std::exception& failure){OverlayFailed(failure.what(),0,true);}
  // Once the host's waits were consumed, even failure handling must retain
  // the overlay's replacement semaphore instead of waiting a second time.
  return submitted;
}
}
