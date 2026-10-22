#include "xess_provider.h"
#include "inputs.h"
#include "turing_unlock.h"
#include "bridge_format.h"
#include "bridge_restart.h"
#include "bridge_resume_history.h"
#include "bridge_sdk_retirement.h"
#include "swapchain_lock.h"
#include "slboot.h"
#include "camera_frame.h"
#include "render_reprojection.h"
#include "rdr_camera.h"
#include <cmath>
#include <dxgi1_5.h>
#include "bridge_lifecycle.h"
#include "xess_bridge_plan.h"
#include "xess_bridge_device.h"
#include "xess_bridge_runtime.h"
#include "shared_bridge_runtime.h"
#include "display_target.h"
#include "mvecscale.h"
#include "xess_bridge_snapshot.h"
#include "xess_queue_sync.h"
#include "xess_queue_contract.h"
#include "config.h"
#include "menu.h"
#include "log.h"
#include "runtime_errors.h"
#include "hud_capture.h"
#include <vulkan/vulkan_win32.h>
#include <dxgi1_5.h>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <memory>
#include <cstring>
#include <array>

namespace fgvk { namespace {
using namespace xess_bridge;
std::recursive_mutex lock;
DevicePlan plan;
// Retained until process exit: never unload GPU SDKs from CRT/DllMain teardown
// under the Windows loader lock. Swapchain recreation retires them explicitly.
Device& bridge=*new Device;
SharedBridgeRuntime& sdk=*new SharedBridgeRuntime;
VkDevice device{};VkPhysicalDevice physical{};VkInstance instance{};uint32_t family{};
VkQueue graphicsQueue{};
struct QueueRequest {uint32_t family,count;VkDeviceQueueCreateFlags flags;};
struct QueueState {uint32_t family;VkDeviceQueueCreateFlags flags;};
struct PoolState {uint32_t family;VkCommandPoolCreateFlags flags;};
std::vector<QueueRequest> queueRequests;
std::vector<VkQueueFamilyProperties> queueProperties;
std::unordered_map<VkQueue,QueueState> queues;
std::unordered_map<VkCommandPool,PoolState> pools;
std::unordered_map<VkCommandBuffer,VkCommandPool> commandPools;
std::unordered_set<uint32_t> loggedProducerFamilies;
PFN_vkGetInstanceProcAddr gi{};PFN_vkGetDeviceProcAddr gd{};
bool planned{},ready{},attached{},sdkAvailable{},sdkFailed{};
bool slDevicePrepared{};
BridgeRestart restartRequest;
BridgeResumeHistory resumeHistory;
ID3D12CommandQueue* dxQueue{};ID3D12CommandQueue* nativeDxQueue{};ID3D12Fence* dxDone{};HANDLE dxEvent{};uint64_t dxSerial{},acquireSerial{},inputSerial{};
std::unordered_map<VkSurfaceKHR,HWND> windows;
PFN_vkCreateWin32SurfaceKHR createSurface{};
struct Frame {
 VkImage image{};VkDeviceMemory memory{};SharedImage transport{};
 VkCommandBuffer command{};VkFence fence{};SharedFence uploaded{};
 ID3D12CommandAllocator* allocator{};ID3D12GraphicsCommandList* list{};
 uint64_t retired{},epoch{};bool acquired{},transportUsed{},submitMarked{};
};
struct Chain {
 VkSwapchainKHR handle{};VkSurfaceKHR surface{};HWND window{};VkExtent2D extent{};VkFormat format{};DXGI_FORMAT dxFormat{};
 IDXGISwapChain3* dx{};VkCommandPool pool{};std::vector<Frame> frames;uint32_t next{};bool allowTearing{};
};
std::unique_ptr<Chain> chain;
std::unordered_set<VkSwapchainKHR> retiredChains;
uint64_t nextChainId=0xf176000000000000ull;
std::unordered_map<VkRenderPass,VkRenderPass> passVariants;
struct Input {
 std::unique_ptr<Snapshot> snapshot=std::make_unique<Snapshot>();VkSemaphore available{};VkFence submittedFence{};
 std::unordered_set<VkCommandBuffer> commands;bool recorded{},submitted{},valid{},used{},colorUsed{};
 uint64_t order{},epoch{},retired{};uint32_t cameraToken{};uint32_t dw{},dh{},mw{},mh{},producerFamily=UINT32_MAX;int flags{};
 xefg_swapchain_frame_constant_data_t constants{};
 sl::Constants slConstants{};
};
Input inputs[4];
struct RetiredResources {std::unique_ptr<Chain> chain;std::vector<Input> inputs;};
std::unordered_map<VkSwapchainKHR,RetiredResources> retiredResources;
xess_bridge::InputDimensions inputDimensions;
xess_bridge::InputDimensions presentedDimensions;
double ClockMs(){static const double scale=[](){LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);return 1000./double(frequency.QuadPart);}();LARGE_INTEGER now{};QueryPerformanceCounter(&now);return now.QuadPart*scale;}
struct Samples {double sum{},maximum{};uint32_t count{};void Add(double ms){sum+=ms;maximum=(std::max)(maximum,ms);++count;}double Mean()const{return count?sum/count:0.;}};
struct BridgeTimings {Samples acquireRetire,sleep,uploadRetire,tagCPU,presentCPU;};
BridgeTimings timings;
uint32_t timingReportsLeft=3;
template<class T>T Fn(const char* name){return gd?(T)gd(device,name):nullptr;}
VKAPI_ATTR VkResult VKAPI_CALL CreatePool(VkDevice d,const VkCommandPoolCreateInfo* ci,const VkAllocationCallbacks* a,VkCommandPool* out){
 std::lock_guard<std::recursive_mutex> guard(lock);auto result=Fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(d,ci,a,out);
 if(result==VK_SUCCESS && d==device && ci && out)pools[*out]={ci->queueFamilyIndex,ci->flags};return result;
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommands(VkDevice d,const VkCommandBufferAllocateInfo* ci,VkCommandBuffer* out){
 std::lock_guard<std::recursive_mutex> guard(lock);auto result=Fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(d,ci,out);
 if(result==VK_SUCCESS && d==device && ci && out && pools.count(ci->commandPool))for(uint32_t n=0;n<ci->commandBufferCount;++n)commandPools[out[n]]=ci->commandPool;
 return result;
}
void ForgetPoolCommands(VkCommandPool pool,bool erase){
 for(auto it=commandPools.begin();it!=commandPools.end();)if(it->second==pool){auto cmd=it->first;XessForget(cmd);HudCaptureForgetCommand(cmd);ForgetCommand(cmd);if(erase)it=commandPools.erase(it);else ++it;}else ++it;
}
VKAPI_ATTR void VKAPI_CALL FreeCommands(VkDevice d,VkCommandPool pool,uint32_t count,const VkCommandBuffer* cmds){
 std::lock_guard<std::recursive_mutex> guard(lock);
 if(d==device && cmds)for(uint32_t n=0;n<count;++n){XessForget(cmds[n]);commandPools.erase(cmds[n]);}
 Fn<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(d,pool,count,cmds);
}
VKAPI_ATTR void VKAPI_CALL DestroyPool(VkDevice d,VkCommandPool pool,const VkAllocationCallbacks* a){
 std::lock_guard<std::recursive_mutex> guard(lock);if(d==device){ForgetPoolCommands(pool,true);pools.erase(pool);}
 Fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(d,pool,a);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetPool(VkDevice d,VkCommandPool pool,VkCommandPoolResetFlags flags){
 std::lock_guard<std::recursive_mutex> guard(lock);auto result=Fn<PFN_vkResetCommandPool>("vkResetCommandPool")(d,pool,flags);
 if(result==VK_SUCCESS && d==device)ForgetPoolCommands(pool,false);return result;
}
void Error(RuntimeErrors::Code code,const char* detail,int64_t native=0){
 RuntimeErrors::ReportError(code,detail,(int)native);
 static std::array<ULONGLONG,RuntimeErrors::Capacity> last{};auto now=GetTickCount64();auto at=(size_t)code;
 if(at>=last.size() || !last[at] || now-last[at]>=1000){if(at<last.size())last[at]=now;Log("%s bridge: %s; native=%lld",sdk.IsDLSS()?"DLSS-G":"XeSS",detail,(long long)native);}
}
bool SdkError(bool fatal=true){
 const auto s=sdk.Status();
 const bool hags=fatal && sdk.IsDLSS() && s.hardwareSchedulingRequired;
 if(hags)RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderInit);
 else if(fatal)RuntimeErrors::ClearError(RuntimeErrors::Code::HardwareScheduling);
 Error(hags?RuntimeErrors::Code::HardwareScheduling:fatal?RuntimeErrors::Code::ProviderInit:RuntimeErrors::Code::ProviderState,
   s.operation && s.operation[0]?s.operation:"Selected bridge backend rejected this operation",s.native);
 if(fatal)sdkFailed=true;return false;
}
bool Dx(HRESULT result,const char* name){if(SUCCEEDED(result))return true;Error(RuntimeErrors::Code::PresentFailed,name,result);return false;}
bool Vk(VkResult result,const char* name){if(result==VK_SUCCESS)return true;Error(RuntimeErrors::Code::VulkanDevice,name,result);return false;}
bool Retire(uint64_t value){
 if(!value)return true;auto completed=dxDone->GetCompletedValue();
 if(completed==UINT64_MAX){Error(RuntimeErrors::Code::VulkanDevice,"DX12 device removed while retiring bridge copies",DXGI_ERROR_DEVICE_REMOVED);return false;}
 if(completed>=value)return true;
 if(!Dx(dxDone->SetEventOnCompletion(value,dxEvent),"DX12 completion event failed"))return false;
 if(WaitForSingleObject(dxEvent,5000)!=WAIT_OBJECT_0){Error(RuntimeErrors::Code::GenerationStalled,"DX12 bridge copy retirement exceeded 5 seconds");return false;}
 return true;
}
bool Own(VkSwapchainKHR sc){return chain && chain->handle==sc;}
bool OwnImage(VkImage image){
 if(chain)for(auto& f:chain->frames)if(f.image==image)return true;
 for(auto& group:retiredResources)for(auto& f:group.second.chain->frames)if(f.image==image)return true;
 return false;
}
VkImageLayout Layout(VkImageLayout l){return l==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR?VK_IMAGE_LAYOUT_GENERAL:l;}
DXGI_FORMAT Format(VkFormat f){return xess_bridge::TransportFormat(f);}
bool NativeImage(VkExtent2D size,VkFormat format,VkImageUsageFlags usage,VkImage& image,VkDeviceMemory& memory){
 VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={size.width,size.height,1};
 ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
 if(!Vk(Fn<PFN_vkCreateImage>("vkCreateImage")(device,&ci,nullptr,&image),"Virtual swapchain image allocation"))return false;
 VkMemoryRequirements req{};Fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device,image,&req);
 VkPhysicalDeviceMemoryProperties props{};((PFN_vkGetPhysicalDeviceMemoryProperties)gi(instance,"vkGetPhysicalDeviceMemoryProperties"))(physical,&props);
 uint32_t type=UINT32_MAX;for(uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
 if(type==UINT32_MAX)return false;
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
 return Vk(Fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&ai,nullptr,&memory),"Virtual image memory allocation") &&
        Vk(Fn<PFN_vkBindImageMemory>("vkBindImageMemory")(device,image,memory,0),"Virtual image memory binding");
}
VKAPI_ATTR VkResult VKAPI_CALL CopyableImage(VkDevice d,const VkImageCreateInfo* original,const VkAllocationCallbacks* a,VkImage* out){
 if(!original)return VK_ERROR_INITIALIZATION_FAILED;auto ci=*original;bool changed=false;
 if(d==device && physical && ci.samples==VK_SAMPLE_COUNT_1_BIT && !(ci.flags&(VK_IMAGE_CREATE_SPARSE_BINDING_BIT|VK_IMAGE_CREATE_PROTECTED_BIT|VK_IMAGE_CREATE_DISJOINT_BIT)) &&
    (ci.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) && !(ci.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)){
  auto query=(PFN_vkGetPhysicalDeviceFormatProperties)gi(instance,"vkGetPhysicalDeviceFormatProperties");VkFormatProperties props{};
  if(query){query(physical,ci.format,&props);auto features=ci.tiling==VK_IMAGE_TILING_OPTIMAL?props.optimalTilingFeatures:props.linearTilingFeatures;
   if(features&VK_FORMAT_FEATURE_TRANSFER_SRC_BIT){ci.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;changed=true;}}
 }
 auto result=Fn<PFN_vkCreateImage>("vkCreateImage")(d,&ci,a,out);
 if(result!=VK_SUCCESS && changed){ci=*original;result=Fn<PFN_vkCreateImage>("vkCreateImage")(d,&ci,a,out);}
 if(result==VK_SUCCESS && out){std::lock_guard<std::mutex> guard(resource_detail::mutex);resource_detail::images[*out]={ci.initialLayout,ci.usage,ci.format,ci.extent.width,ci.extent.height};}
 return result;
}
bool MakeFrame(Chain& c,Frame& f,VkImageUsageFlags usage){
 if(!NativeImage(c.extent,c.format,usage|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,f.image,f.memory) ||
    !bridge.CreateImage(c.extent.width,c.extent.height,c.dxFormat,c.format,f.transport) || !bridge.CreateFence(f.uploaded))return false;
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=c.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
 VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
 if(!Vk(Fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ai,&f.command),"Bridge command allocation") ||
    !Vk(Fn<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&f.fence),"Bridge command fence"))return false;
 if(!Dx(bridge.Native()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),(void**)&f.allocator),"DX12 bridge allocator") ||
    !Dx(bridge.Native()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator,nullptr,__uuidof(ID3D12GraphicsCommandList),(void**)&f.list),"DX12 bridge command list"))return false;
 return Dx(f.list->Close(),"Initial DX12 command close");
}
void DestroyInput(Input& in){
 if(in.snapshot)in.snapshot->Destroy();
 if(in.available)Fn<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,in.available,nullptr);
 if(in.submittedFence)Fn<PFN_vkDestroyFence>("vkDestroyFence")(device,in.submittedFence,nullptr);
 in=Input{};
}
void DestroyInputs(){
 resumeHistory={};ResetFrameCameraHistory();
 inputDimensions={};
 presentedDimensions={};timings={};timingReportsLeft=3;
 for(auto& in:inputs)DestroyInput(in);
}
void DestroyFrameCopies(Chain& c){
 for(auto& f:c.frames){if(f.list)f.list->Release();if(f.allocator)f.allocator->Release();bridge.DestroyImage(f.transport);bridge.DestroyFence(f.uploaded);
  if(f.fence)Fn<PFN_vkDestroyFence>("vkDestroyFence")(device,f.fence,nullptr);
  f.list=nullptr;f.allocator=nullptr;f.fence=VK_NULL_HANDLE;f.command=VK_NULL_HANDLE;
 }
 if(c.pool)Fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,c.pool,nullptr);c.pool=VK_NULL_HANDLE;
}
void DestroyFrameResources(Chain& c){
 DestroyFrameCopies(c);
 for(auto& f:c.frames){
  if(f.image)Fn<PFN_vkDestroyImage>("vkDestroyImage")(device,f.image,nullptr);
  if(f.memory)Fn<PFN_vkFreeMemory>("vkFreeMemory")(device,f.memory,nullptr);f=Frame{};}
}
bool DestroyChain(bool retainGameResources=false){
 if(!chain)return true;
 if(attached && !sdk.SetEnabled(false))SdkError(false);
 if(!Retire(dxSerial) || !Vk(Fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device),"Bridge lifecycle Vulkan retirement"))return false;
 // SL may still consume its preservation copies on an internal queue. Retire
 // that work too before freeing the provider's resources or presentation queue.
 if(auto fence=sdk.InputFence();fence && sdk.InputFenceValue() && fence->GetCompletedValue()<sdk.InputFenceValue()){
  if(!Dx(fence->SetEventOnCompletion(sdk.InputFenceValue(),dxEvent),"DLSSG input retirement event") || WaitForSingleObject(dxEvent,5000)!=WAIT_OBJECT_0){Error(RuntimeErrors::Code::GenerationStalled,"DLSSG input retirement exceeded 5 seconds");return false;}
 }
 auto& c=*chain;
 // Drop our extra swapchain reference BEFORE the SDK drops its own reference
 // and destroys its context. Keep input/client textures until shutdown retires.
 if(!ReleasePresentationBeforeSDK(c.dx,[&]{return !(attached || sdkAvailable) || sdk.Shutdown();})){SdkError();return false;}
 attached=false;sdkAvailable=false;dxQueue=nativeDxQueue;
 MenuDestroySwapchain(device,c.handle);
 if(c.handle)retiredChains.insert(c.handle);
 if(retainGameResources && c.handle){
  // These private copy objects are referenced only by our retired submissions,
  // not by game recordings. Release them now to avoid retaining output memory.
  DestroyFrameCopies(c);
  const auto handle=c.handle;HudCaptureRetire((uint64_t)handle);
  auto& group=retiredResources[handle];group.inputs.reserve(4);
  for(auto& in:inputs){group.inputs.emplace_back(std::move(in));in=Input{};}
  group.chain=std::move(chain);
  Log("Bridge old resources retained until game destroys swapchain=%p; generations=%zu",(void*)handle,retiredResources.size());
 }else{HudCaptureDestroy();DestroyFrameResources(c);}
 DestroyInputs();
 attached=false;sdkAvailable=false;sdkFailed=false;chain.reset();return true;
}
VKAPI_ATTR VkResult VKAPI_CALL Surface(VkInstance inst,const VkWin32SurfaceCreateInfoKHR* ci,const VkAllocationCallbacks* a,VkSurfaceKHR* out){
 auto result=createSurface(inst,ci,a,out);if(result==VK_SUCCESS && ci && out){std::lock_guard<std::recursive_mutex> guard(lock);windows[*out]=ci->hwnd;}return result;
}
VKAPI_ATTR VkResult VKAPI_CALL Images(VkDevice d,VkSwapchainKHR sc,uint32_t* count,VkImage* out){
 std::lock_guard<std::recursive_mutex> guard(lock);if(retiredChains.count(sc))return VK_ERROR_OUT_OF_DATE_KHR;
 if(!Own(sc))return Fn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR")(d,sc,count,out);
 if(!count)return VK_ERROR_INITIALIZATION_FAILED;const uint32_t size=(uint32_t)chain->frames.size();
 if(!out){*count=size;return VK_SUCCESS;}uint32_t n=(std::min)(*count,size);for(uint32_t i=0;i<n;++i)out[i]=chain->frames[i].image;
 *count=n;return n<size?VK_INCOMPLETE:VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice d,VkSwapchainKHR sc,const VkAllocationCallbacks* a){
 AllQueuesGuard queueGuard;
 std::lock_guard<std::recursive_mutex> guard(lock);
 if(auto group=retiredResources.find(sc);group!=retiredResources.end()){
  // The game may have submitted a previously recorded old-image command after
  // creation of the replacement. Retire it before releasing that generation.
  if(!Vk(Fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device),"Old swapchain resource retirement"))return;
  HudCaptureReleaseRetired((uint64_t)sc);
  for(auto& in:group->second.inputs)DestroyInput(in);
  DestroyFrameResources(*group->second.chain);retiredResources.erase(group);
  Log("Bridge old swapchain resources released at game destroy=%p",(void*)sc);return;
 }
 if(retiredChains.count(sc))return;
 if(!Own(sc)){Fn<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(d,sc,a);return;}
 DestroyChain();SetFPSGenerationActive(false);
}
VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice d,VkSwapchainKHR sc,uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t* index){
 if(restartRequest.Pending()){
  // Validate SDK retirement before telling RDR2 to replace WSI. If an
  // external owner blocks XeSS destruction, cancel the switch on the still
  // existing native output instead of triggering its fatal allocation path.
  SwapchainScope lifecycle;AllQueuesGuard queuesGuard;
  std::lock_guard<std::recursive_mutex> guard(lock);
  if(Own(sc) && !DestroyChain(true)){
   if(sdk.RestoreStoppedXeSS()){
    chain->dx=sdk.Swapchain();chain->dx->AddRef();attached=true;sdkAvailable=true;sdkFailed=true;
    restartRequest.Cancel();resumeHistory.Invalidate();SetFPSGenerationActive(false);
    Error(RuntimeErrors::Code::ProviderInit,"FG restart cancelled: XeSS still has an external presentation reference (-19). Native rendering retained; release conflicting overlays or restart the game before changing provider.",-19);
   }else return VK_ERROR_INITIALIZATION_FAILED;
  }
 }

 QueueGuard queueGuard(graphicsQueue);
 std::lock_guard<std::recursive_mutex> guard(lock);if(retiredChains.count(sc))return VK_ERROR_OUT_OF_DATE_KHR;
 if(!Own(sc))return Fn<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(d,sc,timeout,sem,fence,index);
 if(restartRequest.Pending())return VK_ERROR_OUT_OF_DATE_KHR;
 if(!index || (!sem && !fence))return VK_ERROR_INITIALIZATION_FAILED;
 auto& c=*chain;uint64_t retired[8]{};bool acquired[8]{};
 for(uint32_t n=0;n<c.frames.size();++n){retired[n]=c.frames[n].retired;acquired[n]=c.frames[n].acquired;}
 auto completed=dxDone->GetCompletedValue();if(completed==UINT64_MAX)return VK_ERROR_DEVICE_LOST;
 uint32_t found=SelectAcquireSlot(retired,acquired,(uint32_t)c.frames.size(),c.next,completed);
 if(found==UINT32_MAX)return timeout?VK_TIMEOUT:VK_NOT_READY;
 auto& f=c.frames[found];if(!timeout && dxDone->GetCompletedValue()<f.retired)return VK_NOT_READY;
 double retireStart=ClockMs();if(!Retire(f.retired))return VK_ERROR_DEVICE_LOST;timings.acquireRetire.Add(ClockMs()-retireStart);
 VkQueue q=graphicsQueue;if(!q)return VK_ERROR_INITIALIZATION_FAILED;
 VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};signal.signalSemaphoreCount=sem?1:0;signal.pSignalSemaphores=sem?&sem:nullptr;
 auto result=Fn<PFN_vkQueueSubmit>("vkQueueSubmit")(q,1,&signal,fence);if(result!=VK_SUCCESS)return result;
 f.acquired=true;f.submitMarked=false;f.epoch=++acquireSerial;c.next=(found+1)%(uint32_t)c.frames.size();*index=found;
 // The acquire signal is already submitted. XeLL can pace without holding the
 // graphics queue hostage. Keep the provider lock for SDK lifecycle/marker order.
 queueGuard.Unlock();
 if(attached && (!sdkFailed || sdk.IsDLSS())){uint32_t id=uint32_t((f.epoch-1)%UINT32_MAX)+1;double sleepStart=ClockMs();if(!sdk.Sleep(id,Rt().fpsCap.load(std::memory_order_relaxed)))SdkError(false);timings.sleep.Add(ClockMs()-sleepStart);if(!sdk.Marker(id,XELL_SIMULATION_START))SdkError(false);}
 HudCaptureBeginFrame(f.epoch);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Acquire2(VkDevice d,const VkAcquireNextImageInfoKHR* info,uint32_t* index){
 if(!info)return VK_ERROR_INITIALIZATION_FAILED;
 return Acquire(d,info->swapchain,info->timeout,info->semaphore,info->fence,index);
}
VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
 uint32_t mc,const VkMemoryBarrier* m,uint32_t bc,const VkBufferMemoryBarrier* b,uint32_t count,const VkImageMemoryBarrier* images){
 std::lock_guard<std::recursive_mutex> guard(lock);std::vector<VkImageMemoryBarrier> copy;
 if(count){copy.assign(images,images+count);for(auto& x:copy)if(OwnImage(x.image)){x.oldLayout=Layout(x.oldLayout);x.newLayout=Layout(x.newLayout);}}
 Fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cmd,src,dst,flags,mc,m,bc,b,count,count?copy.data():images);
}
void Barrier2Impl(VkCommandBuffer cmd,const VkDependencyInfo* info,const char* name){
 std::lock_guard<std::recursive_mutex> guard(lock);if(!info)return;auto ci=*info;std::vector<VkImageMemoryBarrier2> images;
 if(info->imageMemoryBarrierCount){images.assign(info->pImageMemoryBarriers,info->pImageMemoryBarriers+info->imageMemoryBarrierCount);
  for(auto& x:images)if(OwnImage(x.image)){x.oldLayout=Layout(x.oldLayout);x.newLayout=Layout(x.newLayout);}ci.pImageMemoryBarriers=images.data();}
 Fn<PFN_vkCmdPipelineBarrier2>(name)(cmd,&ci);
}
VKAPI_ATTR void VKAPI_CALL Barrier2(VkCommandBuffer c,const VkDependencyInfo* i){Barrier2Impl(c,i,"vkCmdPipelineBarrier2");}
VKAPI_ATTR void VKAPI_CALL Barrier2KHR(VkCommandBuffer c,const VkDependencyInfo* i){Barrier2Impl(c,i,"vkCmdPipelineBarrier2KHR");}
VKAPI_ATTR VkResult VKAPI_CALL RenderPass(VkDevice d,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks* a,VkRenderPass* out){
 std::lock_guard<std::recursive_mutex> guard(lock);
 auto result=Fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(d,info,a,out);if(result!=VK_SUCCESS)return result;
 auto ci=*info;std::vector<VkAttachmentDescription> attachments;
 if(info->attachmentCount){attachments.assign(info->pAttachments,info->pAttachments+info->attachmentCount);for(auto& x:attachments){x.initialLayout=Layout(x.initialLayout);x.finalLayout=Layout(x.finalLayout);}ci.pAttachments=attachments.data();}
 VkRenderPass variant{};result=Fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(d,&ci,a,&variant);
 if(result==VK_SUCCESS)passVariants[*out]=variant;else Error(RuntimeErrors::Code::ProviderInit,"XeSS compatible render-pass creation failed",result);
 return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL MenuPass(VkDevice d,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks* a,VkRenderPass* out){
 std::lock_guard<std::recursive_mutex> guard(lock);auto ci=*info;std::vector<VkAttachmentDescription> attachments;
 if(chain && ci.attachmentCount){attachments.assign(ci.pAttachments,ci.pAttachments+ci.attachmentCount);for(auto& x:attachments){x.initialLayout=Layout(x.initialLayout);x.finalLayout=Layout(x.finalLayout);}ci.pAttachments=attachments.data();}
 return Fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(d,&ci,a,out);
}
VkResult RenderPass2Impl(VkDevice d,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks* a,VkRenderPass* out,const char* name){
 std::lock_guard<std::recursive_mutex> guard(lock);
 auto result=Fn<PFN_vkCreateRenderPass2>(name)(d,info,a,out);if(result!=VK_SUCCESS)return result;
 auto ci=*info;std::vector<VkAttachmentDescription2> attachments;
 if(info->attachmentCount){attachments.assign(info->pAttachments,info->pAttachments+info->attachmentCount);for(auto& x:attachments){x.initialLayout=Layout(x.initialLayout);x.finalLayout=Layout(x.finalLayout);}ci.pAttachments=attachments.data();}
 VkRenderPass variant{};result=Fn<PFN_vkCreateRenderPass2>(name)(d,&ci,a,&variant);
 if(result==VK_SUCCESS)passVariants[*out]=variant;else Error(RuntimeErrors::Code::ProviderInit,"XeSS compatible render-pass2 creation failed",result);
 return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPass(VkDevice d,VkRenderPass pass,const VkAllocationCallbacks* a){
 std::lock_guard<std::recursive_mutex> guard(lock);auto found=passVariants.find(pass);
 if(found!=passVariants.end()){Fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(d,found->second,a);passVariants.erase(found);}
 Fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(d,pass,a);
}
VkRenderPassBeginInfo BeginInfo(const VkRenderPassBeginInfo* info){
 auto ci=*info;if((chain || !retiredResources.empty()) && HudCaptureFramebufferIsSwapchain(ci.framebuffer)){auto variant=passVariants.find(ci.renderPass);if(variant!=passVariants.end())ci.renderPass=variant->second;}return ci;
}
VKAPI_ATTR void VKAPI_CALL BeginPass(VkCommandBuffer c,const VkRenderPassBeginInfo* info,VkSubpassContents contents){std::lock_guard<std::recursive_mutex> guard(lock);auto ci=BeginInfo(info);Fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(c,&ci,contents);}
VKAPI_ATTR void VKAPI_CALL BeginPass2(VkCommandBuffer c,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* sub){std::lock_guard<std::recursive_mutex> guard(lock);auto ci=BeginInfo(info);Fn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2")(c,&ci,sub);}
VKAPI_ATTR void VKAPI_CALL BeginPass2KHR(VkCommandBuffer c,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* sub){std::lock_guard<std::recursive_mutex> guard(lock);auto ci=BeginInfo(info);Fn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2KHR")(c,&ci,sub);}
VKAPI_ATTR VkResult VKAPI_CALL RenderPass2(VkDevice d,const VkRenderPassCreateInfo2* c,const VkAllocationCallbacks* a,VkRenderPass* o){return RenderPass2Impl(d,c,a,o,"vkCreateRenderPass2");}
VKAPI_ATTR VkResult VKAPI_CALL RenderPass2KHR(VkDevice d,const VkRenderPassCreateInfo2* c,const VkAllocationCallbacks* a,VkRenderPass* o){return RenderPass2Impl(d,c,a,o,"vkCreateRenderPass2KHR");}
PFN_vkVoidFunction LayoutFn(const char* name){
#define MATCH(n,f) if(!std::strcmp(name,n))return (PFN_vkVoidFunction)f
 MATCH("vkCmdPipelineBarrier",Barrier);MATCH("vkCmdPipelineBarrier2",Barrier2);MATCH("vkCmdPipelineBarrier2KHR",Barrier2KHR);
 MATCH("vkCreateRenderPass",RenderPass);MATCH("vkCreateRenderPass2",RenderPass2);MATCH("vkCreateRenderPass2KHR",RenderPass2KHR);
 MATCH("vkDestroyRenderPass",DestroyPass);MATCH("vkCmdBeginRenderPass",BeginPass);MATCH("vkCmdBeginRenderPass2",BeginPass2);MATCH("vkCmdBeginRenderPass2KHR",BeginPass2KHR);
#undef MATCH
 return nullptr;
}
} // namespace

bool XessHiddenExtension(const char* n){return !std::strcmp(n,"VK_KHR_present_id") || !std::strcmp(n,"VK_KHR_present_wait") ||
 !std::strcmp(n,"VK_EXT_swapchain_maintenance1") || !std::strcmp(n,"VK_KHR_swapchain_maintenance1") ||
 !std::strcmp(n,"VK_EXT_full_screen_exclusive") || !std::strcmp(n,"VK_NV_low_latency2") || !std::strcmp(n,"VK_GOOGLE_display_timing") || !std::strcmp(n,"VK_EXT_hdr_metadata");}
bool XessPlanDevice(VkInstance i,VkPhysicalDevice p,const VkDeviceCreateInfo* ci,VkDeviceCreateInfo& out,PFN_vkGetInstanceProcAddr g){
 std::lock_guard<std::recursive_mutex> guard(lock);planned=false;
 if(!ci || !ci->queueCreateInfoCount)return false;
 auto query=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)g(i,"vkGetPhysicalDeviceQueueFamilyProperties");uint32_t count=0;
 if(!query)return false;query(p,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);query(p,&count,families.data());
 const uint32_t selected=SelectGraphicsFamily(ci->pQueueCreateInfos,ci->queueCreateInfoCount,families.data(),count);queueRequests.clear();queueProperties=families;
 for(uint32_t n=0;n<ci->queueCreateInfoCount;++n){const auto& q=ci->pQueueCreateInfos[n];queueRequests.push_back({q.queueFamilyIndex,q.queueCount,q.flags});}
 if(selected==UINT32_MAX){Error(RuntimeErrors::Code::ProviderInit,"XeSS bridge requires a requested unprotected graphics queue");return false;}
 family=selected;
 planned=plan.Prepare(i,p,ci,g);
 if(!planned){Error(RuntimeErrors::Code::ProviderInit,plan.Error());return false;}out=*plan.Info();
 Log("XeSS queue plan: graphicsFamily=%u requestedFamilies=%u; original queue requests preserved",family,ci->queueCreateInfoCount);return true;
}
void XessDeviceReady(VkInstance i,VkPhysicalDevice p,VkDevice d,uint32_t f,PFN_vkGetInstanceProcAddr g,PFN_vkGetDeviceProcAddr get){
 std::lock_guard<std::recursive_mutex> guard(lock);instance=i;physical=p;device=d;gi=g;gd=get;
 sdk.Select(Cfg().provider);slDevicePrepared=Cfg().provider==FGProvider::DLSSGBridge;
 if(!planned)return;
 queues.clear();graphicsQueue=VK_NULL_HANDLE;auto getQueue=Fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
 if(!getQueue){Error(RuntimeErrors::Code::ProviderInit,"XeSS native queue lookup unavailable");return;}
 for(const auto& request:queueRequests)if(!(request.flags&VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT))for(uint32_t n=0;n<request.count;++n){
  VkQueue queue{};getQueue(d,request.family,n,&queue);if(!queue)continue;
  queues[queue]={request.family,request.flags};QueueMutex(queue);
  if(request.family==family && n==0)graphicsQueue=queue;
  Log("XeSS queue registered: queue=%p family=%u index=%u flags=0x%x",(void*)queue,request.family,n,request.flags);
 }
 if(!graphicsQueue){Error(RuntimeErrors::Code::ProviderInit,"XeSS selected graphics queue unavailable");return;}
 ready=bridge.Initialize(i,p,d,g,get,true);if(!ready){Error(RuntimeErrors::Code::ProviderInit,bridge.Error(),bridge.NativeResult());return;}
 D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
 ready=Dx(bridge.Native()->CreateCommandQueue(&desc,__uuidof(ID3D12CommandQueue),(void**)&dxQueue),"XeSS direct queue creation") &&
       Dx(bridge.Native()->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),(void**)&dxDone),"XeSS retirement fence creation");
 nativeDxQueue=dxQueue;
 dxEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!dxEvent)ready=false;
}
uint32_t XessGraphicsFamily(){std::lock_guard<std::recursive_mutex> guard(lock);return planned?family:UINT32_MAX;}
bool PrepareLiveDLSSDevice(){
 if(slDevicePrepared)return true;
 if(!LoadAmpereUnlock() || !EnsureStreamlineD3D12Init())return false;
 // Called with no chain after GPU retirement. The old XeSS-only DX device
 // predates slInit; create the device that SL will bind AFTER initialization.
 if(dxDone)dxDone->Release();if(nativeDxQueue)nativeDxQueue->Release();if(dxEvent)CloseHandle(dxEvent);
 dxDone=nullptr;nativeDxQueue=dxQueue=nullptr;dxEvent=nullptr;dxSerial=0;
 bridge.Shutdown();ready=bridge.Initialize(instance,physical,device,gi,gd,true);
 if(!ready){Error(RuntimeErrors::Code::ProviderInit,bridge.Error(),bridge.NativeResult());return false;}
 D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
 ready=Dx(bridge.Native()->CreateCommandQueue(&desc,__uuidof(ID3D12CommandQueue),(void**)&nativeDxQueue),"Live DLSS bridge queue creation") &&
       Dx(bridge.Native()->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),(void**)&dxDone),"Live DLSS bridge fence creation");
 dxQueue=nativeDxQueue;dxEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);ready=ready && dxEvent;
 slDevicePrepared=ready;return ready;
}
bool XessValidatePresentQueue(VkQueue q,const VkPresentInfoKHR* info){
 std::lock_guard<std::recursive_mutex> guard(lock);bool virtualList=false;
 if(info && info->pSwapchains)for(uint32_t n=0;n<info->swapchainCount;++n)virtualList|=Own(info->pSwapchains[n]);
 if(!virtualList)return true;
 auto found=queues.find(q);if(found!=queues.end() && found->second.family==family && !(found->second.flags&VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT))return true;
 Error(RuntimeErrors::Code::PresentFailed,"XeSS virtual swapchain requires presentation on its selected unprotected graphics family",VK_ERROR_FEATURE_NOT_PRESENT);return false;
}
PFN_vkVoidFunction XessInstanceFn(const char* name,PFN_vkVoidFunction original){
 std::lock_guard<std::recursive_mutex> guard(lock);if(!std::strcmp(name,"vkCreateWin32SurfaceKHR")){if(!createSurface)createSurface=(PFN_vkCreateWin32SurfaceKHR)original;return (PFN_vkVoidFunction)Surface;}return original;
}
PFN_vkVoidFunction VKAPI_CALL XessMenuGDPA(VkDevice d,const char* name){if(!gd)return nullptr;if(!std::strcmp(name,"vkCreateRenderPass"))return (PFN_vkVoidFunction)MenuPass;auto f=LayoutFn(name);return f?f:gd(d,name);}
VKAPI_ATTR void VKAPI_CALL HdrMetadata(VkDevice d,uint32_t count,const VkSwapchainKHR* chains,const VkHdrMetadataEXT* metadata){
 std::lock_guard<std::recursive_mutex> guard(lock);
 if(!chains || !metadata)return;
 auto native=Fn<PFN_vkSetHdrMetadataEXT>("vkSetHdrMetadataEXT");
 auto units=[](float value,double scale,uint32_t maximum){if(!std::isfinite(value) || value<=0)return 0u;return uint32_t((std::min)(double(maximum),double(value)*scale));};
 for(uint32_t i=0;i<count;++i){
  if(retiredChains.count(chains[i]))continue;
  if(!Own(chains[i])){if(native)native(d,1,chains+i,metadata+i);continue;}
  IDXGISwapChain4* output{};auto hr=chain->dx->QueryInterface(IID_PPV_ARGS(&output));
  if(SUCCEEDED(hr)){
   const auto& m=metadata[i];DXGI_HDR_METADATA_HDR10 hdr{};
   hdr.RedPrimary[0]=uint16_t(units(m.displayPrimaryRed.x,50000,50000));hdr.RedPrimary[1]=uint16_t(units(m.displayPrimaryRed.y,50000,50000));
   hdr.GreenPrimary[0]=uint16_t(units(m.displayPrimaryGreen.x,50000,50000));hdr.GreenPrimary[1]=uint16_t(units(m.displayPrimaryGreen.y,50000,50000));
   hdr.BluePrimary[0]=uint16_t(units(m.displayPrimaryBlue.x,50000,50000));hdr.BluePrimary[1]=uint16_t(units(m.displayPrimaryBlue.y,50000,50000));
   hdr.WhitePoint[0]=uint16_t(units(m.whitePoint.x,50000,50000));hdr.WhitePoint[1]=uint16_t(units(m.whitePoint.y,50000,50000));
   hdr.MaxMasteringLuminance=units(m.maxLuminance,1,UINT32_MAX);hdr.MinMasteringLuminance=units(m.minLuminance,10000,UINT32_MAX);
   hdr.MaxContentLightLevel=uint16_t(units(m.maxContentLightLevel,1,UINT16_MAX));hdr.MaxFrameAverageLightLevel=uint16_t(units(m.maxFrameAverageLightLevel,1,UINT16_MAX));
   hr=output->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,sizeof(hdr),&hdr);output->Release();
  }
  if(FAILED(hr))Error(RuntimeErrors::Code::ProviderState,"DXGI HDR10 metadata update failed",hr);
 }
}
PFN_vkVoidFunction XessOverride(const char* name,PFN_vkVoidFunction original){
 if(!original || !gd)return original;
 if(!std::strcmp(name,"vkSetHdrMetadataEXT"))return (PFN_vkVoidFunction)HdrMetadata;
 if(!std::strcmp(name,"vkCreateCommandPool"))return (PFN_vkVoidFunction)CreatePool;
 if(!std::strcmp(name,"vkDestroyCommandPool"))return (PFN_vkVoidFunction)DestroyPool;
 if(!std::strcmp(name,"vkResetCommandPool"))return (PFN_vkVoidFunction)ResetPool;
 if(!std::strcmp(name,"vkAllocateCommandBuffers"))return (PFN_vkVoidFunction)AllocateCommands;
 if(!std::strcmp(name,"vkFreeCommandBuffers"))return (PFN_vkVoidFunction)FreeCommands;
 if(!std::strcmp(name,"vkCreateImage"))return (PFN_vkVoidFunction)CopyableImage;
 if(!std::strcmp(name,"vkGetSwapchainImagesKHR"))return (PFN_vkVoidFunction)Images;
 if(!std::strcmp(name,"vkDestroySwapchainKHR"))return (PFN_vkVoidFunction)Destroy;
 if(!std::strcmp(name,"vkAcquireNextImageKHR"))return (PFN_vkVoidFunction)Acquire;
 if(!std::strcmp(name,"vkAcquireNextImage2KHR"))return (PFN_vkVoidFunction)Acquire2;
 auto f=LayoutFn(name);return f?f:original;
}
VkResult XessCreateSwapchain(VkDevice d,const VkSwapchainCreateInfoKHR* ci,const VkAllocationCallbacks* a,VkSwapchainKHR* out){
 AllQueuesGuard queueGuard;
 std::lock_guard<std::recursive_mutex> guard(lock);
 const bool oldVirtual=ci && (Own(ci->oldSwapchain) || retiredChains.count(ci->oldSwapchain));
 auto fallback=[&](){if(!ci)return VK_ERROR_INITIALIZATION_FAILED;auto native=*ci;if(oldVirtual)native.oldSwapchain=VK_NULL_HANDLE;return Fn<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(d,&native,a,out);};
 if(!ci || !out || !ready)return fallback();
 auto window=windows.find(ci->surface);
 if(window!=windows.end())MenuBindWindow(window->second);
 const bool replacing=chain && xess_bridge::ReplacePresentation(chain->surface,ci->surface,ci->oldSwapchain==VK_NULL_HANDLE,Own(ci->oldSwapchain));
 if(window==windows.end() || Format(ci->imageFormat)==DXGI_FORMAT_UNKNOWN || ci->imageArrayLayers!=1 ||
    ci->imageSharingMode!=VK_SHARING_MODE_EXCLUSIVE || ci->minImageCount>8 || !SupportedOutputColorSpace(ci->imageFormat,ci->imageColorSpace)){
  char detail[256];snprintf(detail,sizeof(detail),"Bridge output unsupported: format=%d colorSpace=%d layers=%u sharing=%d HWND=%d. Use supported SDR or RGB10 HDR10 output with a single exclusive layer.",int(ci->imageFormat),int(ci->imageColorSpace),ci->imageArrayLayers,int(ci->imageSharingMode),int(window!=windows.end()));Error(RuntimeErrors::Code::ProviderInit,detail);if(replacing && !DestroyChain(true))return VK_ERROR_INITIALIZATION_FAILED;return fallback();}
 if(chain && !replacing)return fallback();
 if(chain && !DestroyChain(true))return VK_ERROR_INITIALIZATION_FAILED;
 const auto restart=restartRequest.Take(!chain);
 if(restart){
  const auto selected=(restart&1)?FGProvider::XeSS:FGProvider::DLSSGBridge;
  if(selected==FGProvider::DLSSGBridge && !PrepareLiveDLSSDevice()){
   Error(RuntimeErrors::Code::ProviderInit,"Live DLSS-G setup failed; check runtime/HAGS details; native presentation retained");return fallback();
  }
  sdk.Select(selected);Rt().activeProvider=selected;Rt().appliedPreset=-1;
  SetFPSGenerationActive(false);
  if(selected==FGProvider::DLSSGBridge && !sdk.RefreshDLSSFeature()){
   SdkError();Error(RuntimeErrors::Code::ProviderInit,"DLSS-G feature restart failed; native presentation retained. Check SDK load error above.");return fallback();
  }
  Log("FG restart: retired old presentation; selected=%s requestedPreset=%u",selected==FGProvider::XeSS?"XeSS":"DLSS-G",(restart>>2)&3);
 }

 sdkFailed=false;attached=false;sdkAvailable=false;
 chain=std::make_unique<Chain>();auto& c=*chain;c.surface=ci->surface;c.window=window->second;c.extent=ci->imageExtent;c.format=ci->imageFormat;c.dxFormat=Format(ci->imageFormat);
 wchar_t runtime[MAX_PATH]{};GetModuleFileNameW(nullptr,runtime,MAX_PATH);auto slash=wcsrchr(runtime,L'\\');if(slash)wcscpy_s(slash+1,MAX_PATH-(slash+1-runtime),L"RDR2VulkanFG\\XeSS");
 sdkAvailable=sdk.Initialize(bridge.Native(),runtime);
 if(!sdkAvailable){SdkError();sdk.Shutdown();}
 if(sdkAvailable && sdk.IsDLSS()){
  const uint32_t preset=restart?((restart>>2)&3):uint32_t(ReadDLSSGPreset());
  const auto core=GetModuleHandleW(L"RTX40MFGCore.dll");
  using SetPreset=BOOL(WINAPI*)(uint32_t);const auto setPreset=core?(SetPreset)GetProcAddress(core,"MfgUnlockSetPreset"):nullptr;
  if(TuringUnlock040LoadedPreset()>=0){
    Rt().appliedPreset=TuringUnlock040LoadedPreset();
    if(int(preset)==Rt().appliedPreset.load())RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderOptions);
    else Error(RuntimeErrors::Code::ProviderOptions,"RTX 20 preset saved. Restart the game to apply it; Restart FG keeps the currently loaded preset.");
  }else if(setPreset && setPreset(preset)){Rt().appliedPreset=int(preset);RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderOptions);}
  else if(restart && preset)Error(RuntimeErrors::Code::ProviderOptions,"Live preset control unavailable: install the matching ASI and MFG core from the full package; effective preset is not inferred");
 }

 IDXGIFactory4* factory{};IDXGISwapChain1* dx{};
 DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=c.extent.width;desc.Height=c.extent.height;desc.Format=c.dxFormat;
 desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=3;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
 if(!Dx(CreateDXGIFactory1(__uuidof(IDXGIFactory4),(void**)&factory),"XeSS DXGI factory")){sdk.Shutdown();sdkAvailable=false;chain.reset();return fallback();}
 IDXGIFactory5* features{};BOOL tearing=FALSE;
 if(SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5),(void**)&features))){
  if(SUCCEEDED(features->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))))c.allowTearing=tearing!=FALSE;
  features->Release();
 }
 desc.Flags=c.allowTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
 DisplayTarget target;target.Resolve(c.window,bridge.Native(),factory);
 // RDR2's tracked SR inputs use inverted depth. Every captured input verifies it;
 // mismatches are reported and presented without interpolation, never guessed.
 if(sdkAvailable){
  attached=sdk.CreatePresentation(c.window,dxQueue,factory,desc,XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH,1,XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS,&target.fullscreen,target.output);
  if(attached){sdkFailed=false;c.dx=sdk.Swapchain();c.dx->AddRef();dxQueue=sdk.Queue(nativeDxQueue);RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderInit);RuntimeErrors::ClearError(RuntimeErrors::Code::HardwareScheduling);}else{SdkError();sdk.Shutdown();sdkAvailable=false;}
 }
 if(!c.dx){
  HRESULT result=factory->CreateSwapChainForHwnd(dxQueue,c.window,&desc,&target.fullscreen,target.output,&dx);
  if(SUCCEEDED(result)){result=dx->QueryInterface(__uuidof(IDXGISwapChain3),(void**)&c.dx);dx->Release();}
  if(!Dx(result,"XeSS native DXGI fallback creation")){factory->Release();DestroyChain();return fallback();}
 }
 if(ci->imageColorSpace==VK_COLOR_SPACE_HDR10_ST2084_EXT){
  UINT support{};const auto colorSpace=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  HRESULT colorResult=c.dx->CheckColorSpaceSupport(colorSpace,&support);
  if(SUCCEEDED(colorResult) && !(support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))colorResult=DXGI_ERROR_UNSUPPORTED;
  if(SUCCEEDED(colorResult))colorResult=c.dx->SetColorSpace1(colorSpace);
  if(FAILED(colorResult)){factory->Release();Error(RuntimeErrors::Code::ProviderInit,"The selected DXGI output cannot present HDR10; use SDR output",colorResult);DestroyChain();return fallback();}
 }
 factory->MakeWindowAssociation(c.window,DXGI_MWA_NO_ALT_ENTER);factory->Release();
 LogPresentationDisplay(c.window,c.dx);
 VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=family;
 if(!Vk(Fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&pc,nullptr,&c.pool),"XeSS bridge command pool")){DestroyChain();return fallback();}
 c.frames.resize((std::max)(3u,ci->minImageCount));
 for(auto& f:c.frames)if(!MakeFrame(c,f,ci->imageUsage)){Error(RuntimeErrors::Code::ProviderInit,bridge.Error(),bridge.NativeResult());DestroyChain();return fallback();}
 c.handle=(VkSwapchainKHR)(uintptr_t)(++nextChainId);*out=c.handle;
 std::vector<VkImage> images;for(auto& f:c.frames)images.push_back(f.image);
 HudCaptureInit(device,physical,XessMenuGDPA,c.format,c.extent,images);
 MenuCaptureImages(device,c.handle,(uint32_t)images.size(),images.data());
 Log("XeSS experimental virtual swapchain: %ux%u images=%zu SDK=%d maxInterpolations=%u allowTearing=%d syncInterval=%u",c.extent.width,c.extent.height,c.frames.size(),(int)attached,sdkAvailable?sdk.MaxInterpolations():0,(int)c.allowTearing,Cfg().antiTearing?1u:0u);
 return VK_SUCCESS;
}
bool XessPrepare(VkCommandBuffer cmd,VkImage depthImage,VkImage motionImage,uint32_t dw,uint32_t dh,uint32_t mw,uint32_t mh,
 float jx,float jy,float sx,float sy,const ImageState& ds,const ImageState& ms,const sl::Constants& constants,int flags,VkOffset2D depthOrigin,VkOffset2D motionOrigin){
 std::lock_guard<std::recursive_mutex> guard(lock);if(!chain || !attached || sdkFailed)return false;
 auto command=commandPools.find(cmd);
 auto pool=command==commandPools.end()?pools.end():pools.find(command->second);
 if(pool==pools.end() || (pool->second.flags&VK_COMMAND_POOL_CREATE_PROTECTED_BIT) || pool->second.family>=queueProperties.size() ||
    !(queueProperties[pool->second.family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))){
  Error(RuntimeErrors::Code::InputInvalid,"XeSS cannot identify an unprotected graphics/compute family for this DLSS command buffer");return false;
 }
 const uint32_t producer=pool->second.family;
 if(constants.reset!=sl::Boolean::eFalse && constants.reset!=sl::Boolean::eTrue){Error(RuntimeErrors::Code::ConstantsFailed,"XeSS history-reset flag is uninitialized");return false;}
 if(!(flags&8) || (flags&4) || dw!=mw || dh!=mh){Error(RuntimeErrors::Code::InputInvalid,"XeSS bridge input flags/extents differ from its initialized inverted-depth, unjittered low-resolution MV contract",flags);return false;}
 if(!InputRegionFits(ds.width,ds.height,depthOrigin.x,depthOrigin.y,dw,dh) || !InputRegionFits(ms.width,ms.height,motionOrigin.x,motionOrigin.y,mw,mh)){
  Error(RuntimeErrors::Code::InputInvalid,"Active DLSS rectangle exceeds the tracked input allocation");return false;
 }
 Input* selected=nullptr;auto completed=dxDone->GetCompletedValue();
 for(auto& in:inputs)if(!in.recorded && in.retired<=completed && (!in.used || (in.submittedFence && Fn<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,in.submittedFence)==VK_SUCCESS))){selected=&in;break;}
 if(!selected){Error(RuntimeErrors::Code::InputInvalid,"XeSS input slots still in use; no snapshot overwritten");return false;}
 auto& in=*selected;
 // The exclusive depth scratch buffer stays on its actual producer family.
 // Changing family retires/recreates the slot, never silently changes ownership.
 if(in.dw && (in.dw!=dw || in.dh!=dh || in.mw!=mw || in.mh!=mh || in.producerFamily!=producer)){in.snapshot->Destroy();in.dw=in.dh=in.mw=in.mh=0;in.used=in.colorUsed=false;}
 if(!in.dw){
  if(!in.snapshot->Create(bridge,device,physical,gd,(PFN_vkGetPhysicalDeviceMemoryProperties)gi(instance,"vkGetPhysicalDeviceMemoryProperties"),chain->extent,{dw,dh},chain->dxFormat,chain->format)){
   Error(RuntimeErrors::Code::InputInvalid,in.snapshot->Error());return false;}
  in.dw=dw;in.dh=dh;in.mw=mw;in.mh=mh;in.producerFamily=producer;
 }
 if(!in.available || !in.submittedFence){VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if(!in.available && !Vk(Fn<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&si,nullptr,&in.available),"XeSS input semaphore"))return false;
  if(!in.submittedFence && !Vk(Fn<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&in.submittedFence),"XeSS input submission fence"))return false;}
 SnapshotInput z{depthImage,ds.format,ds.layout,ds.usage,{ds.width,ds.height},depthOrigin};
 SnapshotInput mv{motionImage,ms.format,ms.layout,ms.usage,{ms.width,ms.height},motionOrigin};
 if(!in.snapshot->Record(cmd,{},z,mv,producer,!in.used)){Error(RuntimeErrors::Code::InputInvalid,in.snapshot->Error());return false;}
 if(loggedProducerFamilies.insert(producer).second)Log("XeSS DLSS snapshot: cmd=%p producerFamily=%u graphicsFamily=%u; external ownership uses producer family",(void*)cmd,producer,family);
 in.constants={};for(uint32_t r=0;r<4;++r){in.constants.viewMatrix[r*4+r]=1;const auto& row=constants.cameraViewToClip[r];
  in.constants.projectionMatrix[r*4]=row.x;in.constants.projectionMatrix[r*4+1]=row.y;in.constants.projectionMatrix[r*4+2]=row.z;in.constants.projectionMatrix[r*4+3]=row.w;}
 // Intel allows zero when precise render time is unavailable. Wall-clock gaps
 // between preparations include scheduling/pacing and have coarse granularity;
 // they must not be supplied as a measured GPU/application frame time.
 in.constants.frameRenderTime=0.f;
 in.constants.jitterOffsetX=jx;in.constants.jitterOffsetY=jy;in.constants.motionVectorScaleX=sx;in.constants.motionVectorScaleY=sy;in.constants.resetHistory=constants.reset==sl::Boolean::eTrue?1u:0u;
 // SL and XeSS use different motion-vector units. SL multiplies these values
 // by the tagged extent; preserve the captured NGX pixel displacement exactly.
 in.slConstants=constants;
 const bool dimensionsChanged=inputDimensions.Changed(dw,dh);
 if(dimensionsChanged){in.constants.resetHistory=1;in.slConstants.reset=sl::Boolean::eTrue;Log("Bridge input size changed: %ux%u; resetting provider history",dw,dh);}
 in.slConstants.jitterOffset={jx,jy};
 auto scale=MvecScaleForStreamline(sx,sy,mw,mh,true);in.slConstants.mvecScale={scale.x,scale.y};
 in.slConstants.depthInverted=sl::Boolean::eTrue;in.slConstants.cameraMotionIncluded=sl::Boolean::eTrue;
 in.slConstants.motionVectors3D=sl::Boolean::eFalse;in.slConstants.motionVectorsJittered=sl::Boolean::eFalse;
 in.slConstants.minRelativeLinearDepthObjectSeparation=Rt().depthSeparation.load(std::memory_order_relaxed);
 in.cameraToken=uint32_t(inputSerial+1);
 if(!PrepareFrameCamera(in.cameraToken,in.slConstants,float(chain->extent.width)/chain->extent.height,true,in.slConstants.reset==sl::Boolean::eTrue))return false;
 if(Cfg().cameraSource==CameraSource::Derived || Cfg().cameraAudit)RecordRenderReprojection(cmd,in.cameraToken,depthImage,ds.format,motionImage,ms.format,dw,dh,ds.layout,ms.layout,in.slConstants,depthOrigin.x,depthOrigin.y,motionOrigin.x,motionOrigin.y);
 in.flags=flags;in.recorded=in.valid=true;in.submitted=false;in.commands.clear();in.commands.insert(cmd);in.order=++inputSerial;
 in.epoch=0;for(auto& f:chain->frames)if(f.acquired)in.epoch=(std::max)(in.epoch,f.epoch);
 // Submit markers belong to the queue boundary, never command recording.
 RuntimeErrors::ClearError(RuntimeErrors::Code::InputInvalid);RuntimeErrors::ClearError(RuntimeErrors::Code::ConstantsFailed);return true;
}
void XessForget(VkCommandBuffer cmd){std::lock_guard<std::recursive_mutex> guard(lock);for(auto& in:inputs)if(in.recorded && !in.submitted){in.commands.erase(cmd);if(in.commands.empty()){in.recorded=false;CommitFrameCamera(in.cameraToken,false);CancelRenderReprojection(in.cameraToken);}}}
void XessExecute(VkCommandBuffer primary,uint32_t count,const VkCommandBuffer* secondary){HudCaptureExecuted(primary,count,secondary);std::lock_guard<std::recursive_mutex> guard(lock);for(auto& in:inputs)if(in.recorded && !in.submitted)for(uint32_t i=0;i<count;++i)if(in.commands.count(secondary[i]))in.commands.insert(primary);}
void XessEvalResult(VkCommandBuffer cmd,bool success){
 std::lock_guard<std::recursive_mutex> guard(lock);
 for(auto& in:inputs)if(in.recorded && in.commands.count(cmd)){
  if(success && in.valid)inputDimensions.Accept(in.dw,in.dh);else in.valid=false;
 }
}
void XessBeforeSubmit(VkQueue q,uint32_t count,const VkCommandBuffer* commands){
 std::lock_guard<std::recursive_mutex> guard(lock);
 if(!chain || !attached || (sdkFailed && !sdk.IsDLSS()) || !commands)return;
 auto queue=queues.find(q);if(queue==queues.end() || (queue->second.flags&VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT))return;
 for(const auto& in:inputs)if(in.recorded && !in.submitted && in.valid && in.epoch && in.producerFamily==queue->second.family){
  for(uint32_t n=0;n<count;++n)if(in.commands.count(commands[n])){
   for(auto& frame:chain->frames)if(frame.acquired && frame.epoch==in.epoch && !frame.submitMarked){
    const auto id=uint32_t((frame.epoch-1)%UINT32_MAX)+1;
    if(!sdk.Marker(id,XELL_SIMULATION_END) || !sdk.Marker(id,XELL_RENDERSUBMIT_START))SdkError(false);
    else frame.submitMarked=true;
   }
   break;
  }
 }
}
void XessSubmitted(VkQueue q,uint32_t count,const VkCommandBuffer* commands){
 QueueGuard queueGuard(q);
 std::lock_guard<std::recursive_mutex> guard(lock);for(auto& in:inputs)if(in.recorded && !in.submitted)for(uint32_t i=0;i<count;++i)if(in.commands.count(commands[i])){
  auto found=queues.find(q);
  if(found==queues.end() || found->second.family!=in.producerFamily || (found->second.flags&VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)){
   in.valid=false;sdkFailed=true;Error(RuntimeErrors::Code::InputInvalid,"XeSS snapshot submission queue does not match its recorded command-pool family");break;
  }
  in.submitted=true;in.used=true;if(!Vk(Fn<PFN_vkResetFences>("vkResetFences")(device,1,&in.submittedFence),"XeSS input fence reset")){in.valid=false;in.submitted=false;sdkFailed=true;break;}
  VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};signal.signalSemaphoreCount=1;signal.pSignalSemaphores=&in.available;
  if(!Vk(Fn<PFN_vkQueueSubmit>("vkQueueSubmit")(q,1,&signal,in.submittedFence),"XeSS input completion submission")){in.valid=false;in.submitted=false;sdkFailed=true;}
  break;
 }
}
VkResult XessPresent(VkQueue q,const VkPresentInfoKHR* pi){
 QueueGuard queueGuard(q);
 std::lock_guard<std::recursive_mutex> guard(lock);
 if(!pi || !pi->pSwapchains || !pi->swapchainCount)return VK_ERROR_INITIALIZATION_FAILED;
 bool virtualList=false,retiredList=false;for(uint32_t i=0;i<pi->swapchainCount;++i){virtualList|=Own(pi->pSwapchains[i]);retiredList|=retiredChains.count(pi->pSwapchains[i])!=0;}
 if(retiredList || (virtualList && pi->swapchainCount!=1)){
  VkResult result=retiredList?VK_ERROR_OUT_OF_DATE_KHR:VK_ERROR_FEATURE_NOT_PRESENT;
  Error(RuntimeErrors::Code::PresentFailed,"Unsupported or retired virtual swapchain presentation rejected before native dispatch",result);
  if(pi->pResults)for(uint32_t i=0;i<pi->swapchainCount;++i)pi->pResults[i]=result;return result;
 }
 if(!virtualList)return Fn<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(q,pi);
 if(!XessValidatePresentQueue(q,pi))return VK_ERROR_FEATURE_NOT_PRESENT;
 auto& c=*chain;if(!pi->pImageIndices || pi->pImageIndices[0]>=c.frames.size() || pi->waitSemaphoreCount>64)return VK_ERROR_INITIALIZATION_FAILED;
 auto& f=c.frames[pi->pImageIndices[0]];if(!f.acquired || !Retire(f.retired))return VK_ERROR_DEVICE_LOST;
 double uploadStart=ClockMs();if(!Vk(Fn<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&f.fence,VK_TRUE,5000000000ull),"XeSS upload command retirement"))return VK_ERROR_DEVICE_LOST;timings.uploadRetire.Add(ClockMs()-uploadStart);
 Input* in=nullptr;std::vector<Input*> drain;
 for(auto& candidate:inputs)if(candidate.recorded && candidate.submitted){
  if(!candidate.epoch || candidate.epoch<=f.epoch){drain.push_back(&candidate);if((!candidate.epoch || candidate.epoch==f.epoch) && (!in || candidate.order<in->order))in=&candidate;}
 }
 bool haveHud=HudCaptureResource().resource!=nullptr && HudCaptureFrameIndex()==f.epoch;
 bool off=Rt().fgUserOff.load();uint32_t multiplier=Rt().frames;bool requestOff{};
 if(ConsumeMenuRequest(requestOff,multiplier)){off=requestOff;Rt().fgUserOff=off;}
 multiplier=(std::max)(1u,(std::min)(multiplier,(std::max)(1u,sdk.MaxInterpolations())));Rt().frames=multiplier;
 Rt().maxFrames=(std::max)(1u,sdk.MaxInterpolations());
 if(attached && !sdkFailed && !sdk.SetInterpolations(multiplier))SdkError(false);
 UpdateMenuRuntime(off,multiplier);
 bool generate=attached && !sdkFailed && in && in->valid && haveHud && !off && HasRecentDLSSInput();
 if(in && in->epoch==0 && drain.size()!=1)generate=false;
 // Loading/menu screens have no SR input and need no HUD-less gameplay frame.
 // Report a missing HUD capture only when a real, valid SR input exists.
 static ULONGLONG missingHudSince=0,missingInputSince=0;
 const auto inputNow=GetTickCount64();
 if(attached && !off && in && in->valid && !haveHud){
  if(!missingHudSince)missingHudSince=inputNow;
  if(inputNow-missingHudSince>=500)Error(RuntimeErrors::Code::InputInvalid,"HUD-less post-tone-map pass missing for over 500 ms; waiting for a matching current-frame snapshot, native rendering and menu continue");
 }else{missingHudSince=0;if(haveHud)RuntimeErrors::ClearError(RuntimeErrors::Code::InputInvalid);}
 if(attached && !off && !in && haveHud){
  if(!missingInputSince)missingInputSince=inputNow;
  if(inputNow-missingInputSince>=15000)Error(RuntimeErrors::Code::InputMissing,"No captured DLSS depth/MV inputs for 15 seconds while final-color rendering continues; verify in-game DLSS and other upscaling hooks");
 }else{missingInputSince=0;if(off || (in && in->valid))RuntimeErrors::ClearError(RuntimeErrors::Code::InputMissing);}
 static bool lastEligible=false;static uint32_t resetStreak=0;
 if(generate)resetStreak=in->constants.resetHistory?resetStreak+1:0;else resetStreak=0;
 if(generate!=lastEligible || (generate && resetStreak==60)){
  LogDebug("XeSS eligibility: epoch=%llu eligible=%d input=%llu valid=%d hud=%d userOff=%d reset=%u resetStreak=%u",(unsigned long long)f.epoch,(int)generate,(unsigned long long)(in?in->order:0),(int)(in&&in->valid),(int)haveHud,(int)off,in?in->constants.resetHistory:0,resetStreak);lastEligible=generate;
 }
 if(!Vk(Fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(f.command,0),"XeSS upload command reset"))return VK_ERROR_DEVICE_LOST;
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
 if(!Vk(Fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(f.command,&begin),"XeSS upload command begin"))return VK_ERROR_DEVICE_LOST;
 auto barrier=Fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");auto copy=Fn<PFN_vkCmdCopyImage>("vkCmdCopyImage");
 if(generate){auto hud=HudCaptureResource();SnapshotInput color{(VkImage)hud.resource,c.format,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_SRC_BIT,c.extent,{0,0}};
  if(!in->snapshot->RecordColor(f.command,color,family,!in->colorUsed))generate=false;}
 VkImageMemoryBarrier bs[2]{};for(auto& b:bs){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
 bs[0].image=f.image;bs[0].oldLayout=VK_IMAGE_LAYOUT_GENERAL;bs[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
 bs[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;bs[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
 bs[0].srcQueueFamilyIndex=bs[0].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 bs[1].image=f.transport.vk;bs[1].oldLayout=f.transportUsed?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;bs[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
 bs[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bs[1].srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;bs[1].dstQueueFamilyIndex=family;
 barrier(f.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,bs);
 VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent={c.extent.width,c.extent.height,1};
 copy(f.command,f.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.transport.vk,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
 bs[0].oldLayout=bs[0].newLayout;bs[0].newLayout=VK_IMAGE_LAYOUT_GENERAL;bs[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;bs[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 bs[1].oldLayout=bs[1].newLayout;bs[1].newLayout=VK_IMAGE_LAYOUT_GENERAL;bs[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bs[1].dstAccessMask=0;bs[1].srcQueueFamilyIndex=family;bs[1].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
 barrier(f.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,bs);
 if(!Vk(Fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(f.command),"XeSS upload command end"))return VK_ERROR_DEVICE_LOST;
 std::vector<VkSemaphore> waits;std::vector<VkPipelineStageFlags> stages;std::vector<uint64_t> values;
 for(uint32_t i=0;i<pi->waitSemaphoreCount;++i)waits.push_back(pi->pWaitSemaphores[i]);for(auto* item:drain)waits.push_back(item->available);
 stages.resize(waits.size(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);values.resize(waits.size(),0);uint64_t serial=++dxSerial;
 VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timeline.waitSemaphoreValueCount=(uint32_t)waits.size();timeline.pWaitSemaphoreValues=values.data();timeline.signalSemaphoreValueCount=1;timeline.pSignalSemaphoreValues=&serial;
 VkSubmitInfo upload{VK_STRUCTURE_TYPE_SUBMIT_INFO};upload.pNext=&timeline;upload.waitSemaphoreCount=(uint32_t)waits.size();upload.pWaitSemaphores=waits.data();upload.pWaitDstStageMask=stages.data();upload.commandBufferCount=1;upload.pCommandBuffers=&f.command;upload.signalSemaphoreCount=1;upload.pSignalSemaphores=&f.uploaded.vk;
 if(!Vk(Fn<PFN_vkResetFences>("vkResetFences")(device,1,&f.fence),"XeSS upload fence reset") || !Vk(Fn<PFN_vkQueueSubmit>("vkQueueSubmit")(q,1,&upload,f.fence),"XeSS upload submission"))return VK_ERROR_DEVICE_LOST;
 f.transportUsed=true;
 if(!Dx(dxQueue->Wait(f.uploaded.dx,serial),"XeSS DX12 waits for Vulkan producer") || !Dx(f.allocator->Reset(),"XeSS DX12 allocator reset") || !Dx(f.list->Reset(f.allocator,nullptr),"XeSS DX12 list reset"))return VK_ERROR_DEVICE_LOST;
 uint32_t id=uint32_t((f.epoch-1)%UINT32_MAX)+1;
 bool markers=attached && (!sdkFailed || sdk.IsDLSS());
 if(markers && !f.submitMarked){sdk.Marker(id,XELL_SIMULATION_END);sdk.Marker(id,XELL_RENDERSUBMIT_START);f.submitMarked=true;}
 ID3D12Resource* back{};if(!Dx(c.dx->GetBuffer(c.dx->GetCurrentBackBufferIndex(),__uuidof(ID3D12Resource),(void**)&back),"XeSS DXGI backbuffer"))return VK_ERROR_DEVICE_LOST;
 auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;b.Transition.StateBefore=from;b.Transition.StateAfter=to;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;f.list->ResourceBarrier(1,&b);};
 transition(f.transport.dx,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);transition(back,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
 f.list->CopyResource(back,f.transport.dx);transition(back,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);transition(f.transport.dx,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);back->Release();
 // Enabling Intel FG after tagging discards the first frame's SDK data.
 // Configure the state first, then tag that same accepted reference frame.
 if(attached && !sdk.IsDLSS() && !sdk.SetEnabled(generate)){SdkError(false);generate=false;sdk.SetEnabled(false);}
 if(generate){
  ResolveFrameCamera(in->cameraToken,in->slConstants);
  if(Cfg().cameraSource==CameraSource::Derived || Cfg().cameraAudit){
   const auto unchanged=in->slConstants;
   const bool fit=ResolveRenderReprojection(in->cameraToken,in->slConstants,Cfg().cameraSource!=CameraSource::Derived);
   if(Cfg().cameraSource==CameraSource::Derived){if(!fit)in->slConstants=unchanged;SetDerivedFrameResult(in->cameraToken,fit);}else in->slConstants=unchanged;
  }
  for(uint32_t r=0;r<4;++r){const auto& row=in->slConstants.cameraViewToClip[r];in->constants.projectionMatrix[r*4]=row.x;in->constants.projectionMatrix[r*4+1]=row.y;in->constants.projectionMatrix[r*4+2]=row.z;in->constants.projectionMatrix[r*4+3]=row.w;}
  if(Cfg().cameraSource!=CameraSource::Neutral && ReadFrameCameraInfo().actual!=CameraSource::Neutral){
   const auto& c=in->slConstants;const sl::float3 axes[]={c.cameraRight,c.cameraUp,c.cameraFwd};
   for(unsigned col=0;col<3;++col){float dot=0;for(unsigned row=0;row<3;++row){in->constants.viewMatrix[row*4+col]=(&axes[col].x)[row];dot+=(&c.cameraPos.x)[row]*(&axes[col].x)[row];}in->constants.viewMatrix[12+col]=-dot;}
  }
  in->constants.resetHistory=in->slConstants.reset==sl::Boolean::eTrue;
  if(resumeHistory.NeedsReset(f.epoch) || presentedDimensions.Changed(in->dw,in->dh)){in->constants.resetHistory=1;in->slConstants.reset=sl::Boolean::eTrue;}
  SharedImage* resources[]={&in->snapshot->hudless,&in->snapshot->depth,&in->snapshot->motion};xefg_swapchain_d3d12_resource_data_t tags[3]{};
  for(uint32_t i=0;i<3;++i){transition(resources[i]->dx,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);tags[i].pResource=resources[i]->dx;tags[i].validity=XEFG_SWAPCHAIN_RV_ONLY_NOW;tags[i].incomingState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;tags[i].resourceSize=i?xefg_swapchain_2d_t{in->dw,in->dh}:xefg_swapchain_2d_t{c.extent.width,c.extent.height};}
  tags[0].type=XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;tags[1].type=XEFG_SWAPCHAIN_RES_DEPTH;tags[2].type=XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
  double tagStart=ClockMs();if(!sdk.TagFrame(id,in->constants,tags,3,f.list,in->slConstants)){SdkError(false);generate=false;}timings.tagCPU.Add(ClockMs()-tagStart);
  for(auto* resource:resources)transition(resource->dx,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 }
 if(attached && !sdk.SetEnabled(generate)){SdkError(false);generate=false;sdk.SetEnabled(false);}
 if(!Dx(f.list->Close(),"XeSS DX12 copy list close"))return VK_ERROR_DEVICE_LOST;
 ID3D12CommandList* list=f.list;dxQueue->ExecuteCommandLists(1,&list);
 if(!Dx(dxQueue->Signal(dxDone,serial),"XeSS copy completion fence"))return VK_ERROR_DEVICE_LOST;
 f.retired=serial;for(auto* item:drain){item->recorded=item->submitted=false;item->retired=serial;item->commands.clear();if(item!=in){CommitFrameCamera(item->cameraToken,false);CancelRenderReprojection(item->cameraToken);}}if(generate)in->colorUsed=true;
 if(markers){sdk.Marker(id,XELL_RENDERSUBMIT_END);sdk.Marker(id,XELL_PRESENT_START);}
 // Native/loading frames also advance the XeLL/FG association. An old ID
 // cannot be carried through a suspension and reused when generation resumes.
 if(attached && !sdk.SetPresentId(id))SdkError(false);
 const UINT interval=Cfg().antiTearing && (!attached || sdk.VSyncSupported())?1u:0u;
 static bool unsupportedVSyncLogged=false;
 if(Cfg().antiTearing && attached && sdk.IsDLSS() && !sdk.VSyncSupported() && !unsupportedVSyncLogged){
  unsupportedVSyncLogged=true;Log("DLSS-G runtime does not report application VSync support; using SyncInterval=0 so the SL pacer controls delivery");
 }
 const UINT presentFlags=!interval && c.allowTearing?DXGI_PRESENT_ALLOW_TEARING:0;
 double presentStart=ClockMs();HRESULT presented=attached?sdk.Present(interval,presentFlags):c.dx->Present(interval,presentFlags);timings.presentCPU.Add(ClockMs()-presentStart);
 // Read the presentation diagnostic before the final marker can replace it.
 auto presentationStatus=sdk.Status();
 resumeHistory.Presented(f.epoch,generate,presented==S_OK);
 if(in){CommitFrameCamera(in->cameraToken,generate && presented==S_OK);CommitRenderReprojection(in->cameraToken,generate && presented==S_OK);}
 if(generate && presented==S_OK)presentedDimensions.Accept(in->dw,in->dh);
 if(markers)sdk.Marker(id,XELL_PRESENT_END);f.acquired=false;
 xefg_swapchain_present_status_t actual{};bool measured=attached && sdk.LastPresent(actual) && presentationStatus.native==0;
 if(attached && sdk.IsDLSS() && presentationStatus.native)Error(RuntimeErrors::Code::ProviderState,presentationStatus.operation?presentationStatus.operation:"DLSSG DX12 state query failed",presentationStatus.native);
 SetFPSGenerationActive(measured && actual.isFrameGenEnabled);
 if(measured){RecordDisplayedFrames(actual.framesPresented);UpdateMenuStats(sdk.MaxInterpolations(),(uint32_t)actual.frameGenResult,(float)actual.framesPresented);}
 else{RecordDisplayedFrames(1);UpdateMenuStats(1,0,1);}
 if(serial==1 || serial%120==0)LogDebug("XeSS telemetry: frame=%llu epoch=%llu input=%llu hud=%llu requested=%d enabled=%u displayed=%u result=%d query=%lld reset=%u resetStreak=%u",(unsigned long long)serial,(unsigned long long)f.epoch,(unsigned long long)(in?in->order:0),(unsigned long long)HudCaptureFrameIndex(),(int)generate,actual.isFrameGenEnabled,actual.framesPresented,(int)actual.frameGenResult,(long long)sdk.StateQueryResult(),in?in->constants.resetHistory:0,resetStreak);
 if(serial%120==0){auto timingLog=timingReportsLeft?&Log:&LogDebug;if(timingReportsLeft)--timingReportsLeft;timingLog("Bridge CPU timing avg/max ms: acquireRetire=%.3f/%.3f XeLLSleep=%.3f/%.3f uploadRetire=%.3f/%.3f tags=%.3f/%.3f proxyPresent=%.3f/%.3f interval=%u presentFlags=0x%x; SDK output counts are queued frames, not scanout timestamps",timings.acquireRetire.Mean(),timings.acquireRetire.maximum,timings.sleep.Mean(),timings.sleep.maximum,timings.uploadRetire.Mean(),timings.uploadRetire.maximum,timings.tagCPU.Mean(),timings.tagCPU.maximum,timings.presentCPU.Mean(),timings.presentCPU.maximum,interval,presentFlags);timings={};}
 static ULONGLONG nativeSince=0;auto now=GetTickCount64();
 if(generate && measured && actual.framesPresented<=1){if(!nativeSince)nativeSince=now;}else nativeSince=0;
 if(generate && measured && (sdk.IsDLSS()?actual.frameGenResult!=0:actual.frameGenResult<0))Error(RuntimeErrors::Code::ProviderState,sdk.IsDLSS()?"DLSSG DX12 reported a nonzero status bitmask; check bridge telemetry":"XeSS interpolation failed",actual.frameGenResult);
 else if(generate && measured && nativeSince && now-nativeSince>=2000)Error(RuntimeErrors::Code::ProviderState,"Bridge FG is enabled but has presented only native frames for over 2 seconds; check SDK result and history reset in the log",actual.frameGenResult);
 else if(generate && measured){RuntimeErrors::ClearError(RuntimeErrors::Code::InputInvalid);RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderState);}
 if(pi->pResults)pi->pResults[0]=SUCCEEDED(presented)?VK_SUCCESS:VK_ERROR_DEVICE_LOST;
 return Dx(presented,"XeSS DXGI presentation")?VK_SUCCESS:VK_ERROR_DEVICE_LOST;
}
void RequestBridgeRestart(FGProvider provider,int preset){
 if(!IsDX12Bridge(Cfg().provider) || !IsDX12Bridge(provider) || preset<0 || preset>3)return;
 std::lock_guard<std::recursive_mutex> guard(lock);
 if(!chain || !ready){Error(RuntimeErrors::Code::ProviderInit,"The bridge device/presentation is unavailable; change an in-game graphics setting to recreate WSI or restart the game; a driver/device loss cannot be repaired by the FG button");return;}
 if(restartRequest.Request(provider==FGProvider::XeSS,unsigned(preset)))Log("FG restart queued; game will recreate presentation at the next acquire");
}
bool BridgeRestartPending(){return restartRequest.Pending();}
void XessCaptureLock(){lock.lock();}
void XessCaptureUnlock(){lock.unlock();}
}
