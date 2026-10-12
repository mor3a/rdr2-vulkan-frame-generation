#include "fsr_native.h"
#include "log.h"
#include "config.h"
#include "menu.h"
#include "startup_guard.h"
#include "hud_capture.h"
#include "hud_mask.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/vk/ffx_api_vk.h"
#include <windows.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>
namespace fgvk {
static HMODULE library{};static PfnFfxCreateContext create{};static PfnFfxDestroyContext destroy{};
static PfnFfxConfigure configure{};static PfnFfxDispatch dispatch{};static PfnFfxQuery query{};
static ffxContext scContext{},fgContext{};
static ffxCreateBackendVKDesc backend{};
static ffxCreateContextDescFrameGenerationSwapChainVK scDesc{};
static ffxCreateContextDescFrameGeneration fgDesc{};
static ffxQueryDescSwapchainReplacementFunctionsVK replacements{};
static PFN_vkGetDeviceProcAddr nativeGDPA{};static VkDevice device{};static VkPhysicalDevice physical{};
static VkSwapchainKHR chain{};static uint32_t family{},extraBase{};
struct RetiredChain { VkSwapchainKHR chain;ffxContext swapchain,interpolation; };
static std::vector<RetiredChain> retiredChains;
static std::vector<VkDeviceQueueCreateInfo> queueDescriptions;static std::vector<float> priorities;
static VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
static VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
static std::vector<const char*> extensions;
static std::recursive_mutex mutex;static uint64_t frameId{};static bool enabled{},failed{};
static bool offConfigured{};static uint64_t resetFrameId{1};
static ULONGLONG previousFrame{};
struct HudRect { unsigned x,y,w,h; };
static std::vector<HudRect> hudRects;
static bool strictHud=true,autoHudCapture=true;
struct HudFrame { uint64_t id{}; FfxApiResource color{}; };
static HudFrame hudFrames[4]{};
static void LoadHudRegions(const wchar_t* modulePath){
  wchar_t ini[MAX_PATH]{};wcscpy_s(ini,modulePath);auto slash=wcsrchr(ini,L'\\');if(!slash)return;*(slash+1)=0;wcscat_s(ini,L"RDR2VulkanFG.ini");
  strictHud=GetPrivateProfileIntW(L"fgvk",L"FSRHUDStrict",1,ini)!=0;
  autoHudCapture=GetPrivateProfileIntW(L"fgvk",L"FSRHUDCapture",1,ini)!=0;
  if(!GetPrivateProfileIntW(L"fgvk",L"FSRHUDProtection",1,ini))return;
  for(int i=1;i<=8;++i){wchar_t key[32]{},value[128]{};swprintf_s(key,L"FSRHUDRect%d",i);
    const wchar_t* fallback=i==1?L"2,70,18,28":i==2?L"78,2,20,15":L"";
    GetPrivateProfileStringW(L"fgvk",key,fallback,value,128,ini);
    unsigned x,y,w,h;if(swscanf_s(value,L"%u,%u,%u,%u",&x,&y,&w,&h)==4 && x<100 && y<100 && w && h && w<=100-x && h<=100-y)hudRects.push_back({x,y,w,h});
  }
  Log("FSR3: HUD region protection enabled for %u regions (current-frame copies, not UI extraction)",(unsigned)hudRects.size());
}
// AMD's interpolation callback supplies the real final color and its generated output.
// Restore selected HUD regions after interpolation, keeping both image layouts unchanged.
// Regions include their background; actual engine UI separation is not available here.
static void ProtectHud(ffxDispatchDescFrameGeneration* desc){
  static bool reported=false;if(!reported){reported=true;Log("FSR HUD callback: frames=%u source=%p output=%p extent=%ux%u regions=%u",desc->numGeneratedFrames,desc->presentColor.resource,desc->outputs[0].resource,fgDesc.displaySize.width,fgDesc.displaySize.height,(unsigned)hudRects.size());}
  if(hudRects.empty() || !desc->presentColor.resource)return;
  auto barrier=(PFN_vkCmdPipelineBarrier)nativeGDPA(device,"vkCmdPipelineBarrier");
  auto copy=(PFN_vkCmdCopyImage)nativeGDPA(device,"vkCmdCopyImage");
  if(!barrier||!copy)return;
  auto cmd=(VkCommandBuffer)desc->commandList;auto source=(VkImage)desc->presentColor.resource;
  for(unsigned i=0;i<desc->numGeneratedFrames && i<4;++i){auto target=(VkImage)desc->outputs[i].resource;if(!target || target==source)continue;
    VkImageMemoryBarrier bs[2]{};
    for(auto& b:bs){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    bs[0].image=source;bs[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT;bs[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;bs[0].oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;bs[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[1].image=target;bs[1].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;bs[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bs[1].oldLayout=VK_IMAGE_LAYOUT_GENERAL;bs[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,bs);
    VkImageCopy regions[8]{};unsigned count=0;auto width=fgDesc.displaySize.width,height=fgDesc.displaySize.height;
    for(auto r:hudRects){auto& c=regions[count];c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.srcOffset=c.dstOffset={(int)(width*r.x/100),(int)(height*r.y/100),0};c.extent={width*r.w/100,height*r.h/100,1};if(c.extent.width && c.extent.height)++count;}
    if(count)copy(cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,count,regions);
    for(auto& b:bs){auto layout=b.oldLayout;b.oldLayout=b.newLayout;b.newLayout=layout;auto access=b.srcAccessMask;b.srcAccessMask=b.dstAccessMask;b.dstAccessMask=access;}
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,bs);
  }
}
void FsrDisable(){failed=true;}
bool FsrLoad(){
  if(library)return true;
  HMODULE self{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&FsrLoad,&self);
  wchar_t path[MAX_PATH]{};GetModuleFileNameW(self,path,MAX_PATH);auto slash=wcsrchr(path,L'\\');if(!slash)return false;*(slash+1)=0;
  LoadHudRegions(path);wcscat_s(path,L"RDR2VulkanFG\\FSR3\\amd_fidelityfx_vk.dll");
  library=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if(!library){Log("FSR3: native runtime missing, error=%lu; native rendering retained",GetLastError());return false;}
  create=(PfnFfxCreateContext)GetProcAddress(library,"ffxCreateContext");destroy=(PfnFfxDestroyContext)GetProcAddress(library,"ffxDestroyContext");
  configure=(PfnFfxConfigure)GetProcAddress(library,"ffxConfigure");dispatch=(PfnFfxDispatch)GetProcAddress(library,"ffxDispatch");query=(PfnFfxQuery)GetProcAddress(library,"ffxQuery");
  if(!create||!destroy||!configure||!dispatch||!query){Log("FSR3: incompatible Vulkan runtime exports; FG disabled");failed=true;return false;}
  Log("FSR3: native AMD Vulkan runtime loaded");return true;
}
bool FsrPlanDevice(VkInstance instance,VkPhysicalDevice pd,const VkDeviceCreateInfo* ci,VkDeviceCreateInfo& planned){
  if(!FsrLoad()||failed)return false;
  auto vk=(PFN_vkGetInstanceProcAddr)GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),"vkGetInstanceProcAddr");
  auto props=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)vk(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
  auto features=(PFN_vkGetPhysicalDeviceFeatures2)vk(instance,"vkGetPhysicalDeviceFeatures2");
  uint32_t count=0;props(pd,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);props(pd,&count,families.data());
  queueDescriptions.assign(ci->pQueueCreateInfos,ci->pQueueCreateInfos+ci->queueCreateInfoCount);
  int selected=-1;for(size_t i=0;i<queueDescriptions.size();++i){auto q=queueDescriptions[i];if(q.queueFamilyIndex<count && (families[q.queueFamilyIndex].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT) && families[q.queueFamilyIndex].queueCount>=q.queueCount+3){selected=int(i);break;}}
  if(selected<0){Log("FSR3: three independent pacing queues unavailable; FG disabled");failed=true;return false;}
  auto& q=queueDescriptions[selected];family=q.queueFamilyIndex;extraBase=q.queueCount;priorities.assign(q.pQueuePriorities,q.pQueuePriorities+q.queueCount);priorities.resize(q.queueCount+3,1.f);q.queueCount+=3;q.pQueuePriorities=priorities.data();
  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};timeline.pNext=&sync2;supported.pNext=&timeline;features(pd,&supported);
  if(!timeline.timelineSemaphore || !sync2.synchronization2){Log("FSR3: required timeline/synchronization2 features unavailable");failed=true;return false;}
  planned=*ci;planned.pQueueCreateInfos=queueDescriptions.data();
  bool hasTimeline=false,hasSync=false;
  for(auto next=(const VkBaseInStructure*)ci->pNext;next;next=next->pNext){if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES||next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)hasTimeline=true;if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES||next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)hasSync=true;}
  for(auto next=(const VkBaseInStructure*)ci->pNext;next;next=next->pNext){if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)Log("FSR3 existing Vk12 timeline=%u",((const VkPhysicalDeviceVulkan12Features*)next)->timelineSemaphore);if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)Log("FSR3 existing timeline=%u",((const VkPhysicalDeviceTimelineSemaphoreFeatures*)next)->timelineSemaphore);}
  timeline.pNext=(void*)ci->pNext;sync2.pNext=(void*)ci->pNext;
  if(!hasTimeline){timeline.pNext=(void*)planned.pNext;planned.pNext=&timeline;}
  if(!hasSync){sync2.pNext=(void*)planned.pNext;planned.pNext=&sync2;}
  extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
  for(const char* name:{"VK_KHR_timeline_semaphore","VK_KHR_synchronization2"}){bool found=false;for(auto ext:extensions)if(strcmp(ext,name)==0)found=true;if(!found)extensions.push_back(name);}
  planned.enabledExtensionCount=(uint32_t)extensions.size();planned.ppEnabledExtensionNames=extensions.data();physical=pd;
  Log("FSR3: reserved queue family=%u game count=%u plus three FG queues",family,extraBase);return true;
}
void FsrDeviceReady(VkDevice d,PFN_vkGetDeviceProcAddr gdpa){device=d;nativeGDPA=gdpa;backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;backend.vkDevice=d;backend.vkPhysicalDevice=physical;backend.vkDeviceProcAddr=gdpa;}
static ffxReturnCode_t Interpolate(ffxDispatchDescFrameGeneration* desc,void*){
  std::lock_guard<std::recursive_mutex> lock(mutex);if(!fgContext||failed)return FFX_API_RETURN_ERROR;
  if(desc->frameID==resetFrameId)desc->reset=true;
  desc->backbufferTransferFunction=FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;desc->minMaxLuminance[0]=0;desc->minMaxLuminance[1]=1000;
  auto r=dispatch(&fgContext,&desc->header);if(r){Log("FSR3: interpolate failed=%u",r);failed=true;}
  else{const auto& hud=hudFrames[desc->frameID%4];if(hud.id==desc->frameID && hud.color.resource){if(strictHud)HudMaskApply((VkCommandBuffer)desc->commandList,desc->presentColor,hud.color,desc->outputs[0]);}else ProtectHud(desc);}return r;
}
static VkResult VKAPI_CALL Acquire2(VkDevice d,const VkAcquireNextImageInfoKHR* info,uint32_t* index){
  if(info->deviceMask==1 && info->swapchain==chain)return replacements.pOutAcquireNextImageKHR(d,chain,info->timeout,info->semaphore,info->fence,index);
  return ((PFN_vkAcquireNextImage2KHR)nativeGDPA(d,"vkAcquireNextImage2KHR"))(d,info,index);
}
static void VKAPI_CALL DestroyChain(VkDevice d,VkSwapchainKHR sc,const VkAllocationCallbacks* allocator){
  for(size_t i=0;i<retiredChains.size();++i)if(retiredChains[i].chain==sc){
    ((PFN_vkDeviceWaitIdle)nativeGDPA(d,"vkDeviceWaitIdle"))(d);std::lock_guard<std::recursive_mutex> lock(mutex);
    auto& old=retiredChains[i];if(old.interpolation)destroy(&old.interpolation,nullptr);if(old.swapchain)destroy(&old.swapchain,nullptr);retiredChains.erase(retiredChains.begin()+i);return;
  }
  if(sc!=chain){((PFN_vkDestroySwapchainKHR)nativeGDPA(d,"vkDestroySwapchainKHR"))(d,sc,allocator);return;}
  ((PFN_vkDeviceWaitIdle)nativeGDPA(d,"vkDeviceWaitIdle"))(d);std::lock_guard<std::recursive_mutex> lock(mutex);
  HudMaskDestroy();if(fgContext)destroy(&fgContext,nullptr);if(scContext)destroy(&scContext,nullptr);HudCaptureDestroy();chain={};enabled=false;
}
VkResult FsrCreateSwapchain(VkDevice d,const VkSwapchainCreateInfoKHR* ci,const VkAllocationCallbacks* allocator,VkSwapchainKHR* out){
  auto native=(PFN_vkCreateSwapchainKHR)nativeGDPA(d,"vkCreateSwapchainKHR");
  VkSwapchainCreateInfoKHR normalized=*ci;
  if(scContext){
    // A replacement handle is not a driver swapchain. Retain the old context until
    // the game's matching destroy, and create a fresh context for the new extent.
    ((PFN_vkDeviceWaitIdle)nativeGDPA(d,"vkDeviceWaitIdle"))(d);
    std::lock_guard<std::recursive_mutex> lock(mutex);
    retiredChains.push_back({chain,scContext,fgContext});scContext={};fgContext={};chain={};enabled=false;previousFrame=0;normalized.oldSwapchain=VK_NULL_HANDLE;
    Log("FSR3: retiring interpolation context for swapchain recreation");
  }
  ci=&normalized;if(failed||!physical)return native(d,ci,allocator,out);
  std::lock_guard<std::recursive_mutex> lock(mutex);
  scDesc={};scDesc.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;scDesc.device=d;scDesc.physicalDevice=physical;scDesc.swapchain=&chain;scDesc.createInfo=*ci;scDesc.allocator=(VkAllocationCallbacks*)allocator;
  // RDR2 supplies a device-group extension even for local single-GPU presentation.
  // AMD's swapchain rejects this extension; its LOCAL mode needs no group override.
  auto extension=(const VkBaseInStructure*)ci->pNext;
  if(extension && extension->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR){
    auto group=(const VkDeviceGroupSwapchainCreateInfoKHR*)extension;
    if(group->modes==VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR){scDesc.createInfo.pNext=group->pNext;Log("FSR3: single-device LOCAL swapchain extension normalized");}
    else{Log("FSR3: non-local device-group presentation unsupported; FG disabled");failed=true;return native(d,ci,allocator,out);}
  }
  auto getQueue=(PFN_vkGetDeviceQueue)nativeGDPA(d,"vkGetDeviceQueue");
  scDesc.gameQueue.familyIndex=family;getQueue(d,family,0,&scDesc.gameQueue.queue);
  VkQueueInfoFFXAPI* qs[]={&scDesc.asyncComputeQueue,&scDesc.presentQueue,&scDesc.imageAcquireQueue};for(int i=0;i<3;++i){qs[i]->familyIndex=family;getQueue(d,family,extraBase+i,&qs[i]->queue);}
  Log("FSR3 queues: %p %p %p %p",(void*)scDesc.gameQueue.queue,(void*)scDesc.asyncComputeQueue.queue,(void*)scDesc.presentQueue.queue,(void*)scDesc.imageAcquireQueue.queue);
  for(auto next=(const VkBaseInStructure*)ci->pNext;next;next=next->pNext)Log("FSR3 swapchain extension sType=%u",(uint32_t)next->sType);
  auto r=create(&scContext,&scDesc.header,nullptr);Log("FSR3: native swapchain context result=%u",r);
  if(r){failed=true;UpdateMenuStats(1,r,1.f);return native(d,ci,allocator,out);}
  replacements={};replacements.header.type=FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;r=query(&scContext,&replacements.header);
  if(r||!replacements.pOutQueuePresentKHR||!replacements.pOutAcquireNextImageKHR||!replacements.pOutGetSwapchainImagesKHR){Log("FSR3: replacement function query failed=%u; native rendering retained",r);destroy(&scContext,nullptr);scContext={};chain={};failed=true;return native(d,ci,allocator,out);}
  *out=chain;frameId=0;resetFrameId=1;offConfigured=false;fgDesc.displaySize={ci->imageExtent.width,ci->imageExtent.height};fgDesc.maxRenderSize=fgDesc.displaySize;fgDesc.backBufferFormat=ffxApiGetSurfaceFormatVK(ci->imageFormat);
  if(autoHudCapture){uint32_t imageCount=0;if(replacements.pOutGetSwapchainImagesKHR(d,chain,&imageCount,nullptr)==VK_SUCCESS && imageCount){std::vector<VkImage> images(imageCount);if(replacements.pOutGetSwapchainImagesKHR(d,chain,&imageCount,images.data())==VK_SUCCESS){images.resize(imageCount);HudCaptureInit(d,physical,nativeGDPA,ci->imageFormat,ci->imageExtent,images);if(strictHud)HudMaskInit(d,physical,nativeGDPA,ci->imageFormat,ci->imageExtent);}}}
  return VK_SUCCESS;
}
PFN_vkVoidFunction FsrOverride(const char* name){
  if(strcmp(name,"vkGetSwapchainImagesKHR")==0)return (PFN_vkVoidFunction)+[](VkDevice d,VkSwapchainKHR sc,uint32_t* n,VkImage* images)->VkResult {if(scContext && sc==chain)return replacements.pOutGetSwapchainImagesKHR(d,sc,n,images);return ((PFN_vkGetSwapchainImagesKHR)nativeGDPA(d,"vkGetSwapchainImagesKHR"))(d,sc,n,images);};
  if(strcmp(name,"vkAcquireNextImageKHR")==0)return (PFN_vkVoidFunction)+[](VkDevice d,VkSwapchainKHR sc,uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t* index)->VkResult {if(scContext && sc==chain)return replacements.pOutAcquireNextImageKHR(d,sc,timeout,sem,fence,index);return ((PFN_vkAcquireNextImageKHR)nativeGDPA(d,"vkAcquireNextImageKHR"))(d,sc,timeout,sem,fence,index);};
  if(strcmp(name,"vkAcquireNextImage2KHR")==0)return (PFN_vkVoidFunction)Acquire2;
  if(strcmp(name,"vkDestroySwapchainKHR")==0)return (PFN_vkVoidFunction)DestroyChain;
  if(strcmp(name,"vkSetHdrMetadataEXT")==0)return (PFN_vkVoidFunction)+[](VkDevice d,uint32_t n,const VkSwapchainKHR* sc,const VkHdrMetadataEXT* md){if(scContext)replacements.pOutSetHdrMetadataEXT(d,n,sc,md);else ((PFN_vkSetHdrMetadataEXT)nativeGDPA(d,"vkSetHdrMetadataEXT"))(d,n,sc,md);};
  return nullptr;
}
static FfxApiResource Resource(VkImage image,const ImageState& state){
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=state.format;ci.extent={state.width,state.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.usage=state.usage;
  FfxApiResource r{};r.resource=(void*)image;r.description=ffxApiGetImageResourceDescriptionVK(image,ci,0);r.state=FFX_API_RESOURCE_STATE_COMPUTE_READ;return r;
}
bool FsrPrepare(VkCommandBuffer cmd,VkImage depth,VkImage mv,uint32_t dw,uint32_t dh,uint32_t mw,uint32_t mh,float jx,float jy,float sx,float sy,const ImageState& ds,const ImageState& ms,const sl::Constants& camera,int flags){
  std::lock_guard<std::recursive_mutex> lock(mutex);if(!scContext||failed)return false;
  if(Rt().fgUserOff || MenuIsOpen()){HudCaptureBeginFrame(0);previousFrame=0;resetFrameId=frameId+1;return false;}
  if(!fgContext){fgDesc.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&backend.header};fgDesc.flags=(flags&8?FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED:0)|(flags&4?FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION:0)|(flags&2?0:FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS);auto r=create(&fgContext,&fgDesc.header,nullptr);Log("FSR3: native interpolation context result=%u",r);if(r){failed=true;return false;}}
  ++frameId;
  HudCaptureBeginFrame(frameId);
  ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.swapChain=(void*)chain;cfg.frameGenerationEnabled=!Rt().fgUserOff;cfg.allowAsyncWorkloads=false;cfg.frameGenerationCallback=Interpolate;cfg.generationRect={0,0,(int)fgDesc.displaySize.width,(int)fgDesc.displaySize.height};cfg.frameID=frameId;
  auto result=configure(&fgContext,&cfg.header);if(result){Log("FSR3: configure failed=%u",result);failed=true;return false;}enabled=cfg.frameGenerationEnabled;offConfigured=false;SetFPSGenerationActive(enabled);
  ffxDispatchDescFrameGenerationPrepareCameraInfo info{};info.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
  memcpy(info.cameraPosition,&camera.cameraPos,sizeof(info.cameraPosition));memcpy(info.cameraUp,&camera.cameraUp,sizeof(info.cameraUp));memcpy(info.cameraRight,&camera.cameraRight,sizeof(info.cameraRight));memcpy(info.cameraForward,&camera.cameraFwd,sizeof(info.cameraForward));
  ffxDispatchDescFrameGenerationPrepare prep{};prep.header={FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE,&info.header};prep.commandList=(void*)cmd;prep.frameID=frameId;prep.renderSize={dw,dh};prep.jitterOffset={jx,jy};prep.motionVectorScale={sx,sy};auto now=GetTickCount64();prep.frameTimeDelta=previousFrame?float(now-previousFrame):16.667f;previousFrame=now;prep.cameraNear=camera.cameraNear;prep.cameraFar=camera.cameraFar;prep.cameraFovAngleVertical=camera.cameraFOV;prep.viewSpaceToMetersFactor=1.f;prep.depth=Resource(depth,ds);prep.motionVectors=Resource(mv,ms);
  result=dispatch(&fgContext,&prep.header);static bool first=false;if(!first||result){first=true;Log("FSR3: prepare DLSS depth/MV %ux%u result=%u",dw,dh,result);}if(result){failed=true;return false;}return true;
}
VkResult FsrPresent(VkQueue queue,const VkPresentInfoKHR* pi,bool seen){
  if(seen && enabled && fgContext && !failed && !Rt().fgUserOff){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.swapChain=(void*)chain;cfg.frameGenerationEnabled=true;cfg.allowAsyncWorkloads=false;cfg.frameGenerationCallback=Interpolate;cfg.generationRect={0,0,(int)fgDesc.displaySize.width,(int)fgDesc.displaySize.height};cfg.frameID=frameId;cfg.HUDLessColor=HudCaptureResource();
    auto r=configure(&fgContext,&cfg.header);if(r){Log("FSR3: HUD composition configuration failed=%u",r);failed=true;}
    hudFrames[frameId%4]={frameId,cfg.HUDLessColor};
  }
  {std::lock_guard<std::recursive_mutex> lock(mutex);if(fgContext && !offConfigured && (!seen||Rt().fgUserOff||failed)){
    // The backend requires the replacement swapchain even when disabling FG.
    // Always notify it: Prepare may already have changed the local enabled flag.
    ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain=(void*)chain;cfg.frameGenerationEnabled=false;cfg.frameID=frameId;
    cfg.generationRect={0,0,(int)fgDesc.displaySize.width,(int)fgDesc.displaySize.height};
    auto r=configure(&fgContext,&cfg.header);
    static bool reportedOff=false;
    if(r){Log("FSR3: disable configuration failed=%u",r);failed=true;UpdateMenuStats(1,r,1.f);}
    else{if(!reportedOff)Log("FSR3: swapchain interpolation disabled (userOff=%d, DLSS=%d)",(int)Rt().fgUserOff.load(),(int)seen);enabled=false;offConfigured=true;previousFrame=0;resetFrameId=frameId+1;SetFPSGenerationActive(false);reportedOff=true;}
    if(!Rt().fgUserOff)reportedOff=false;
  }}
  bool ours=scContext && pi->swapchainCount==1 && pi->pSwapchains[0]==chain;
  VkResult result=ours?replacements.pOutQueuePresentKHR(queue,pi):((PFN_vkQueuePresentKHR)nativeGDPA(device,"vkQueuePresentKHR"))(queue,pi);
  if(ours && (result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR) && replacements.pOutGetLastPresentCountFFXAPI){static uint64_t base=0,lastCount=0;static unsigned presents=0;auto count=replacements.pOutGetLastPresentCountFFXAPI(chain);RecordDisplayedFrames(lastCount && count>=lastCount?(uint32_t)(count-lastCount):1);lastCount=count;if(!presents)base=count;if(++presents==300){auto ratio=float(count-base)/299.f;Log("FSR3 stats: 300 presents displayed delta=%llu ratio=%.2fx",(unsigned long long)(count-base),ratio);UpdateMenuStats(1,failed?1:0,ratio);if(!failed && ratio>1.5f)CompleteStartupGuard();presents=0;}}
  return result;
}
}
