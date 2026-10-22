#include "hud_capture.h"
#include "log.h"
#include "config.h"
#include "ffx_api/vk/ffx_api_vk.h"
#include <detours.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fgvk {
void XessCaptureLock();void XessCaptureUnlock();
namespace {
struct XessCaptureScope{bool on=IsDX12Bridge(Cfg().provider);XessCaptureScope(){if(on)XessCaptureLock();}~XessCaptureScope(){if(on)XessCaptureUnlock();}};
struct ViewInfo { VkImage image{}; VkFormat format{}; VkImageSubresourceRange range{}; };
struct FramebufferInfo { VkRenderPass pass{}; uint32_t width{}, height{}, layers{}; std::vector<VkImageView> views; };
struct PassAttachment { VkFormat format{}; VkSampleCountFlagBits samples{}; VkAttachmentLoadOp load{}; VkImageLayout initial{}; bool color{}; };
struct PassInfo { std::vector<PassAttachment> attachments; };

std::mutex metadataMutex;
std::mutex captureMutex;
std::unordered_map<VkImageView, ViewInfo> views;
std::unordered_map<VkFramebuffer, FramebufferInfo> framebuffers;
std::unordered_map<VkRenderPass, PassInfo> renderPasses;
std::unordered_set<VkImage> swapchainImages;
struct RetiredCapture {VkDevice device{};PFN_vkGetDeviceProcAddr get{};VkImage image{};VkDeviceMemory memory{};std::unordered_set<VkImage> sources;};
std::unordered_map<uint64_t,RetiredCapture> retiredCaptures;
std::unordered_set<VkCommandBuffer> captureCommands;

VkDevice device{};
VkPhysicalDevice physical{};
PFN_vkGetDeviceProcAddr gdpa{};
VkFormat captureFormat{};
VkExtent2D captureExtent{};
VkImage captureImage{};
VkDeviceMemory captureMemory{};
FfxApiResourceDescription captureDescription{};
bool initializedLayout{};
bool capturedThisFrame{};
bool capturedAvailable{};
bool loggedCapture{};
bool loggedFailure{};
uint64_t activeFrame{};
VkRenderPass capturedPass{};
VkFramebuffer capturedFramebuffer{};

PFN_vkCreateImageView realCreateImageView{};
PFN_vkDestroyImageView realDestroyImageView{};
PFN_vkCreateFramebuffer realCreateFramebuffer{};
PFN_vkDestroyFramebuffer realDestroyFramebuffer{};
PFN_vkCreateRenderPass realCreateRenderPass{};
PFN_vkDestroyRenderPass realDestroyRenderPass{};
PFN_vkCmdBeginRenderPass realBeginRenderPass{};
PFN_vkCreateRenderPass2 realCreateRenderPass2{},realCreateRenderPass2KHR{};
PFN_vkCmdBeginRenderPass2 realBeginRenderPass2{},realBeginRenderPass2KHR{};
PFN_vkCmdBeginRendering realBeginRendering{},realBeginRenderingKHR{};

template<class T> T DeviceFn(const char* name) { return gdpa && device ? reinterpret_cast<T>(gdpa(device, name)) : nullptr; }

void FailOnce(const char* reason) {
  if (!loggedFailure) { loggedFailure = true; Log("HUD capture unavailable: %s", reason); }
}

void DestroyCaptureImage() {
  if (!device || !gdpa) { captureImage = VK_NULL_HANDLE; captureMemory = VK_NULL_HANDLE; return; }
  auto destroyImage = DeviceFn<PFN_vkDestroyImage>("vkDestroyImage");
  auto freeMemory = DeviceFn<PFN_vkFreeMemory>("vkFreeMemory");
  if (captureImage && destroyImage) destroyImage(device, captureImage, nullptr);
  if (captureMemory && freeMemory) freeMemory(device, captureMemory, nullptr);
  captureImage = VK_NULL_HANDLE; captureMemory = VK_NULL_HANDLE;
  initializedLayout = false; capturedAvailable = false; capturedThisFrame = false;
}

bool CreateCaptureImage() {
  if (!device || !physical || !gdpa || !captureFormat || !captureExtent.width || !captureExtent.height) return false;
  HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
  auto getFormat = vk ? reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(GetProcAddress(vk,"vkGetPhysicalDeviceFormatProperties")) : nullptr;
  if (!getFormat) return false;
  VkFormatProperties formatProps{}; getFormat(physical,captureFormat,&formatProps);
  if ((formatProps.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)==0 ||
      (formatProps.optimalTilingFeatures&VK_FORMAT_FEATURE_TRANSFER_DST_BIT)==0) return false;
  auto createImage = DeviceFn<PFN_vkCreateImage>("vkCreateImage");
  auto getRequirements = DeviceFn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
  auto allocate = DeviceFn<PFN_vkAllocateMemory>("vkAllocateMemory");
  auto bind = DeviceFn<PFN_vkBindImageMemory>("vkBindImageMemory");
  if (!createImage || !getRequirements || !allocate || !bind) return false;

  std::vector<uint32_t> families;
  auto getQueues = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)nullptr;
  // Concurrent sharing keeps the capture image usable from the game's and FSR's queue families.
  // The physical-device queue query is resolved from the Vulkan loader export.
  if (vk) getQueues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(GetProcAddress(vk, "vkGetPhysicalDeviceQueueFamilyProperties"));
  uint32_t familyCount = 0;
  if (getQueues) {
    getQueues(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> props(familyCount);
    getQueues(physical, &familyCount, props.data());
    for (uint32_t i=0;i<familyCount;++i) if (props[i].queueCount) families.push_back(i);
  }

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType=VK_IMAGE_TYPE_2D; ci.format=captureFormat; ci.extent={captureExtent.width,captureExtent.height,1};
  ci.mipLevels=1; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
  ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
  if(IsDX12Bridge(Cfg().provider))ci.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (families.size()>1) { ci.sharingMode=VK_SHARING_MODE_CONCURRENT; ci.queueFamilyIndexCount=(uint32_t)families.size(); ci.pQueueFamilyIndices=families.data(); }
  else ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
  if (createImage(device,&ci,nullptr,&captureImage)!=VK_SUCCESS) return false;
  VkMemoryRequirements req{}; getRequirements(device,captureImage,&req);
  auto allocInfo=VkMemoryAllocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocInfo.allocationSize=req.size;
  // Obtain memory properties through the loader. It is safe to select the first compatible type;
  // this image is short lived and only used as an FSR sampled input.
  auto getPhysicalMemory = vk ? reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(GetProcAddress(vk,"vkGetPhysicalDeviceMemoryProperties")) : nullptr;
  VkPhysicalDeviceMemoryProperties memory{};
  if (!getPhysicalMemory) { DestroyCaptureImage(); return false; }
  getPhysicalMemory(physical,&memory);
  uint32_t type=UINT32_MAX;
  for (uint32_t i=0;i<memory.memoryTypeCount;++i) if ((req.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {type=i;break;}
  if (type==UINT32_MAX) for (uint32_t i=0;i<memory.memoryTypeCount;++i) if (req.memoryTypeBits&(1u<<i)) {type=i;break;}
  if (type==UINT32_MAX) { DestroyCaptureImage(); return false; }
  allocInfo.memoryTypeIndex=type;
  if (allocate(device,&allocInfo,nullptr,&captureMemory)!=VK_SUCCESS || bind(device,captureImage,captureMemory,0)!=VK_SUCCESS) { DestroyCaptureImage(); return false; }
  captureDescription=ffxApiGetImageResourceDescriptionVK(captureImage,ci,0);
  return true;
}

bool CaptureCandidate(const VkRenderPassBeginInfo* begin, VkImage& source, VkImageLayout& oldLayout, VkRenderPass& passOut, VkFramebuffer& framebufferOut) {
  if (!begin || !begin->renderPass || !begin->framebuffer || begin->renderArea.offset.x || begin->renderArea.offset.y ||
      begin->renderArea.extent.width!=captureExtent.width || begin->renderArea.extent.height!=captureExtent.height) return false;
  std::lock_guard<std::mutex> lock(metadataMutex);
  auto fb=framebuffers.find(begin->framebuffer); auto rp=renderPasses.find(begin->renderPass);
  if (fb==framebuffers.end() || rp==renderPasses.end() ||
      fb->second.width!=captureExtent.width || fb->second.height!=captureExtent.height || fb->second.layers!=1 ||
      fb->second.views.size()!=rp->second.attachments.size()) return false;
  for (size_t i=0;i<rp->second.attachments.size();++i) {
    const auto& a=rp->second.attachments[i];
    if (!a.color || a.format!=captureFormat || a.load!=VK_ATTACHMENT_LOAD_OP_LOAD || a.initial==VK_IMAGE_LAYOUT_UNDEFINED || a.samples!=VK_SAMPLE_COUNT_1_BIT) continue;
    auto vi=views.find(fb->second.views[i]); if (vi==views.end() || vi->second.format!=captureFormat ||
        !(vi->second.range.aspectMask&VK_IMAGE_ASPECT_COLOR_BIT) || vi->second.range.baseMipLevel!=0 || vi->second.range.levelCount!=1 ||
        vi->second.range.baseArrayLayer!=0 || vi->second.range.layerCount!=1) continue;
    if (swapchainImages.find(vi->second.image)==swapchainImages.end()) continue;
    source=vi->second.image; oldLayout=a.initial; passOut=begin->renderPass; framebufferOut=begin->framebuffer; return true;
  }
  return false;
}

void RecordCapture(VkCommandBuffer cmd, VkImage source, VkImageLayout sourceLayout) {
  auto barrier=DeviceFn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  auto copy=DeviceFn<PFN_vkCmdCopyImage>("vkCmdCopyImage");
  if (!barrier || !copy || !captureImage) { FailOnce("required Vulkan commands are unavailable"); return; }
  VkImageMemoryBarrier b[2]{};
  for (auto& x:b) { x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; x.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; }
  b[0].image=source; b[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; b[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  b[0].oldLayout=sourceLayout; b[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[1].image=captureImage; b[1].srcAccessMask=initializedLayout?VK_ACCESS_SHADER_READ_BIT:0; b[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].oldLayout=initializedLayout?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b);
  VkImageCopy region{}; region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.dstSubresource=region.srcSubresource; region.extent={captureExtent.width,captureExtent.height,1};
  copy(cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,captureImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
  b[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; b[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  b[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].newLayout=sourceLayout;
  b[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  b[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,b);
  initializedLayout=true; capturedAvailable=!IsDX12Bridge(Cfg().provider); capturedThisFrame=true;
  captureCommands.insert(cmd);
  if (!loggedCapture) { loggedCapture=true; Log("HUD capture first success: frame=%llu renderPass=%p framebuffer=%p",(unsigned long long)activeFrame,(void*)capturedPass,(void*)capturedFramebuffer); }
}

VKAPI_ATTR VkResult VKAPI_CALL WrapCreateImageView(VkDevice d,const VkImageViewCreateInfo* ci,const VkAllocationCallbacks* a,VkImageView* out) {
  auto fn=realCreateImageView; if (!fn) return VK_ERROR_INITIALIZATION_FAILED; auto r=fn(d,ci,a,out);
  if (r==VK_SUCCESS && ci && out) { std::lock_guard<std::mutex> lock(metadataMutex); views[*out]={ci->image,ci->format,ci->subresourceRange}; }
  return r;
}
VKAPI_ATTR void VKAPI_CALL WrapDestroyImageView(VkDevice d,VkImageView v,const VkAllocationCallbacks* a) {
  auto fn=realDestroyImageView; if (fn) fn(d,v,a); std::lock_guard<std::mutex> lock(metadataMutex); views.erase(v);
}
VKAPI_ATTR VkResult VKAPI_CALL WrapCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo* ci,const VkAllocationCallbacks* a,VkFramebuffer* out) {
  auto fn=realCreateFramebuffer; if (!fn) return VK_ERROR_INITIALIZATION_FAILED; auto r=fn(d,ci,a,out);
  if (r==VK_SUCCESS && ci && out) { FramebufferInfo f{}; f.pass=ci->renderPass;f.width=ci->width;f.height=ci->height;f.layers=ci->layers;if(ci->pAttachments && !(ci->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))f.views.assign(ci->pAttachments,ci->pAttachments+ci->attachmentCount); std::lock_guard<std::mutex> lock(metadataMutex);framebuffers[*out]=std::move(f); }
  return r;
}
VKAPI_ATTR void VKAPI_CALL WrapDestroyFramebuffer(VkDevice d,VkFramebuffer f,const VkAllocationCallbacks* a) {
  auto fn=realDestroyFramebuffer; if (fn) fn(d,f,a); std::lock_guard<std::mutex> lock(metadataMutex);framebuffers.erase(f);
}
VKAPI_ATTR VkResult VKAPI_CALL WrapCreateRenderPass(VkDevice d,const VkRenderPassCreateInfo* ci,const VkAllocationCallbacks* a,VkRenderPass* out) {
  auto fn=realCreateRenderPass; if (!fn) return VK_ERROR_INITIALIZATION_FAILED; auto r=fn(d,ci,a,out);
  if (r==VK_SUCCESS && ci && out) {
    PassInfo p{};p.attachments.reserve(ci->attachmentCount);
    for (uint32_t i=0;i<ci->attachmentCount;++i) {const auto& x=ci->pAttachments[i];p.attachments.push_back({x.format,x.samples,x.loadOp,x.initialLayout,false});}
    for (uint32_t s=0;s<ci->subpassCount;++s) {const auto& sub=ci->pSubpasses[s];if(s!=0)continue;for(uint32_t i=0;i<sub.colorAttachmentCount;++i)if(sub.pColorAttachments[i].attachment<p.attachments.size())p.attachments[sub.pColorAttachments[i].attachment].color=true;}
    std::lock_guard<std::mutex> lock(metadataMutex);renderPasses[*out]=std::move(p);
  }
  return r;
}
VKAPI_ATTR void VKAPI_CALL WrapDestroyRenderPass(VkDevice d,VkRenderPass p,const VkAllocationCallbacks* a) {
  auto fn=realDestroyRenderPass; if (fn) fn(d,p,a); std::lock_guard<std::mutex> lock(metadataMutex);renderPasses.erase(p);
}
VKAPI_ATTR void VKAPI_CALL WrapBeginRenderPass(VkCommandBuffer cmd,const VkRenderPassBeginInfo* begin,VkSubpassContents contents) {
  HudCaptureBeforePass(cmd,begin); auto fn=realBeginRenderPass; if(fn)fn(cmd,begin,contents);
}
VkResult CreatePass2(VkDevice d,const VkRenderPassCreateInfo2* ci,const VkAllocationCallbacks* a,VkRenderPass* out,PFN_vkCreateRenderPass2 fn){
 auto result=fn(d,ci,a,out);if(result!=VK_SUCCESS || !ci || !out)return result;
 PassInfo p;p.attachments.resize(ci->attachmentCount);
 for(uint32_t i=0;i<ci->attachmentCount;++i){const auto& source=ci->pAttachments[i];auto& target=p.attachments[i];target.format=source.format;target.samples=source.samples;target.load=source.loadOp;target.initial=source.initialLayout;}
 for(uint32_t i=0;i<ci->subpassCount;++i)for(uint32_t j=0;j<ci->pSubpasses[i].colorAttachmentCount;++j){auto at=ci->pSubpasses[i].pColorAttachments[j].attachment;if(at<p.attachments.size())p.attachments[at].color=true;}
 std::lock_guard<std::mutex> guard(metadataMutex);renderPasses[*out]=std::move(p);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL WrapCreatePass2(VkDevice d,const VkRenderPassCreateInfo2* c,const VkAllocationCallbacks* a,VkRenderPass* o){return CreatePass2(d,c,a,o,realCreateRenderPass2);}
VKAPI_ATTR VkResult VKAPI_CALL WrapCreatePass2KHR(VkDevice d,const VkRenderPassCreateInfo2* c,const VkAllocationCallbacks* a,VkRenderPass* o){return CreatePass2(d,c,a,o,realCreateRenderPass2KHR);}
VKAPI_ATTR void VKAPI_CALL WrapBeginPass2(VkCommandBuffer c,const VkRenderPassBeginInfo* i,const VkSubpassBeginInfo* s){HudCaptureBeforePass(c,i);realBeginRenderPass2(c,i,s);}
VKAPI_ATTR void VKAPI_CALL WrapBeginPass2KHR(VkCommandBuffer c,const VkRenderPassBeginInfo* i,const VkSubpassBeginInfo* s){HudCaptureBeforePass(c,i);realBeginRenderPass2KHR(c,i,s);}
void CaptureRendering(VkCommandBuffer c,const VkRenderingInfo* info){
 XessCaptureScope ownership;std::lock_guard<std::mutex> guard(captureMutex);
 if(!info || !captureImage || !activeFrame || capturedThisFrame || info->renderArea.offset.x || info->renderArea.offset.y ||
    info->renderArea.extent.width!=captureExtent.width || info->renderArea.extent.height!=captureExtent.height || info->layerCount!=1 || info->viewMask)return;
 VkImage image{};VkImageLayout layout{};
 {std::lock_guard<std::mutex> metadata(metadataMutex);for(uint32_t i=0;i<info->colorAttachmentCount;++i){const auto& a=info->pColorAttachments[i];auto vi=views.find(a.imageView);
   if(a.loadOp==VK_ATTACHMENT_LOAD_OP_LOAD && vi!=views.end() && vi->second.format==captureFormat && swapchainImages.count(vi->second.image)){image=vi->second.image;layout=a.imageLayout;break;}}}
 if(image)RecordCapture(c,image,layout);
}
VKAPI_ATTR void VKAPI_CALL WrapRendering(VkCommandBuffer c,const VkRenderingInfo* i){CaptureRendering(c,i);realBeginRendering(c,i);}
VKAPI_ATTR void VKAPI_CALL WrapRenderingKHR(VkCommandBuffer c,const VkRenderingInfo* i){CaptureRendering(c,i);realBeginRenderingKHR(c,i);}

template<class T> void AttachExport(HMODULE vk,const char* name,T& target,void* detour) {
  target=reinterpret_cast<T>(GetProcAddress(vk,name)); if(target) DetourAttach(reinterpret_cast<PVOID*>(&target),detour);
}
}

void HudCaptureInstallExportHooks(HMODULE vk) {
  if (!vk) return;
  AttachExport(vk,"vkCreateImageView",realCreateImageView,(void*)WrapCreateImageView);
  AttachExport(vk,"vkDestroyImageView",realDestroyImageView,(void*)WrapDestroyImageView);
  AttachExport(vk,"vkCreateFramebuffer",realCreateFramebuffer,(void*)WrapCreateFramebuffer);
  AttachExport(vk,"vkDestroyFramebuffer",realDestroyFramebuffer,(void*)WrapDestroyFramebuffer);
  AttachExport(vk,"vkCreateRenderPass",realCreateRenderPass,(void*)WrapCreateRenderPass);
  AttachExport(vk,"vkDestroyRenderPass",realDestroyRenderPass,(void*)WrapDestroyRenderPass);
  AttachExport(vk,"vkCmdBeginRenderPass",realBeginRenderPass,(void*)WrapBeginRenderPass);
}

void HudCaptureInit(VkDevice d,VkPhysicalDevice pd,PFN_vkGetDeviceProcAddr getProc,VkFormat format,VkExtent2D extent,const std::vector<VkImage>& images) {
  std::lock_guard<std::mutex> lock(captureMutex);
  DestroyCaptureImage();captureCommands.clear();device=d;physical=pd;gdpa=getProc;captureFormat=format;captureExtent=extent;
  { std::lock_guard<std::mutex> metadataLock(metadataMutex);swapchainImages.clear();swapchainImages.insert(images.begin(),images.end()); }
  if (!CreateCaptureImage()) FailOnce("could not allocate matching sampled image");
}

void HudCaptureDestroy() {
  std::lock_guard<std::mutex> lock(captureMutex);DestroyCaptureImage();
  captureCommands.clear();
  { std::lock_guard<std::mutex> metadataLock(metadataMutex);swapchainImages.clear(); }
  device={};physical={};gdpa=nullptr;captureFormat=VK_FORMAT_UNDEFINED;captureExtent={};
}

void HudCaptureRetire(uint64_t owner){
  std::lock_guard<std::mutex> guard(captureMutex);
  std::lock_guard<std::mutex> metadata(metadataMutex);
  auto& retired=retiredCaptures[owner];retired.device=device;retired.get=gdpa;
  retired.image=captureImage;retired.memory=captureMemory;retired.sources=std::move(swapchainImages);
  captureImage={};captureMemory={};swapchainImages.clear();captureCommands.clear();
  initializedLayout=capturedThisFrame=capturedAvailable=false;activeFrame=0;
}
void HudCaptureReleaseRetired(uint64_t owner){
  std::lock_guard<std::mutex> guard(captureMutex);
  std::lock_guard<std::mutex> metadata(metadataMutex);
  auto found=retiredCaptures.find(owner);if(found==retiredCaptures.end())return;
  const auto& r=found->second;
  auto destroy=(PFN_vkDestroyImage)r.get(r.device,"vkDestroyImage");
  auto free=(PFN_vkFreeMemory)r.get(r.device,"vkFreeMemory");
  if(r.image)destroy(r.device,r.image,nullptr);if(r.memory)free(r.device,r.memory,nullptr);
  retiredCaptures.erase(found);
}

PFN_vkVoidFunction HudCaptureWrap(const char* name,PFN_vkVoidFunction fn) {
  if (!name || !fn) return nullptr;
  if (!strcmp(name,"vkCreateImageView")) {realCreateImageView=(PFN_vkCreateImageView)fn;return (PFN_vkVoidFunction)WrapCreateImageView;}
  if (!strcmp(name,"vkDestroyImageView")) {realDestroyImageView=(PFN_vkDestroyImageView)fn;return (PFN_vkVoidFunction)WrapDestroyImageView;}
  if (!strcmp(name,"vkCreateFramebuffer")) {realCreateFramebuffer=(PFN_vkCreateFramebuffer)fn;return (PFN_vkVoidFunction)WrapCreateFramebuffer;}
  if (!strcmp(name,"vkDestroyFramebuffer")) {realDestroyFramebuffer=(PFN_vkDestroyFramebuffer)fn;return (PFN_vkVoidFunction)WrapDestroyFramebuffer;}
  if (!strcmp(name,"vkCreateRenderPass")) {realCreateRenderPass=(PFN_vkCreateRenderPass)fn;return (PFN_vkVoidFunction)WrapCreateRenderPass;}
  if (!strcmp(name,"vkDestroyRenderPass")) {realDestroyRenderPass=(PFN_vkDestroyRenderPass)fn;return (PFN_vkVoidFunction)WrapDestroyRenderPass;}
  if (!strcmp(name,"vkCmdBeginRenderPass")) {realBeginRenderPass=(PFN_vkCmdBeginRenderPass)fn;return (PFN_vkVoidFunction)WrapBeginRenderPass;}
  if(IsDX12Bridge(Cfg().provider)){
   if(!strcmp(name,"vkCreateRenderPass2")){realCreateRenderPass2=(PFN_vkCreateRenderPass2)fn;return (PFN_vkVoidFunction)WrapCreatePass2;}
   if(!strcmp(name,"vkCreateRenderPass2KHR")){realCreateRenderPass2KHR=(PFN_vkCreateRenderPass2)fn;return (PFN_vkVoidFunction)WrapCreatePass2KHR;}
   if(!strcmp(name,"vkCmdBeginRenderPass2")){realBeginRenderPass2=(PFN_vkCmdBeginRenderPass2)fn;return (PFN_vkVoidFunction)WrapBeginPass2;}
   if(!strcmp(name,"vkCmdBeginRenderPass2KHR")){realBeginRenderPass2KHR=(PFN_vkCmdBeginRenderPass2)fn;return (PFN_vkVoidFunction)WrapBeginPass2KHR;}
   if(!strcmp(name,"vkCmdBeginRendering")){realBeginRendering=(PFN_vkCmdBeginRendering)fn;return (PFN_vkVoidFunction)WrapRendering;}
   if(!strcmp(name,"vkCmdBeginRenderingKHR")){realBeginRenderingKHR=(PFN_vkCmdBeginRendering)fn;return (PFN_vkVoidFunction)WrapRenderingKHR;}
  }
  return fn;
}

void HudCaptureBeginFrame(uint64_t frameId) {
  std::lock_guard<std::mutex> lock(captureMutex);activeFrame=frameId;capturedThisFrame=false;capturedAvailable=false;
}
void HudCaptureForgetCommand(VkCommandBuffer cmd){std::lock_guard<std::mutex> lock(captureMutex);captureCommands.erase(cmd);}
void HudCaptureOnSubmit(uint32_t count,const VkCommandBuffer* commands){
  std::lock_guard<std::mutex> lock(captureMutex);if(!captureImage || !activeFrame)return;
  for(uint32_t i=0;i<count;++i)if(captureCommands.count(commands[i])){capturedAvailable=true;capturedThisFrame=true;break;}
}

void HudCaptureBeforePass(VkCommandBuffer cmd,const VkRenderPassBeginInfo* begin) {
  XessCaptureScope ownership;
  std::lock_guard<std::mutex> lock(captureMutex);
  if (!captureImage || !activeFrame || capturedThisFrame) return;
  VkImage source{};VkImageLayout layout{};VkRenderPass pass{};VkFramebuffer fb{};
  if (!CaptureCandidate(begin,source,layout,pass,fb)) return;
  capturedPass=pass;capturedFramebuffer=fb;RecordCapture(cmd,source,layout);
}

FfxApiResource HudCaptureResource() {
  std::lock_guard<std::mutex> lock(captureMutex);FfxApiResource r{};
  if (!captureImage || !capturedAvailable) return r;
  r.resource=(void*)captureImage;r.description=captureDescription;r.state=FFX_API_RESOURCE_STATE_COMPUTE_READ;return r;
}
uint64_t HudCaptureFrameIndex(){std::lock_guard<std::mutex> guard(captureMutex);return capturedAvailable?activeFrame:0;}
bool HudCaptureFramebufferIsSwapchain(VkFramebuffer fb){
 std::lock_guard<std::mutex> guard(metadataMutex);auto found=framebuffers.find(fb);if(found==framebuffers.end())return false;
 for(auto v:found->second.views){auto vi=views.find(v);if(vi!=views.end()){if(swapchainImages.count(vi->second.image))return true;for(const auto& old:retiredCaptures)if(old.second.sources.count(vi->second.image))return true;}}return false;
}
void HudCaptureExecuted(VkCommandBuffer primary,uint32_t count,const VkCommandBuffer* children){std::lock_guard<std::mutex> guard(captureMutex);for(uint32_t i=0;i<count;++i)if(captureCommands.count(children[i])){captureCommands.insert(primary);break;}}
}
