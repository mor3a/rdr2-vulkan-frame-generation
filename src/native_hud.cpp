#include "native_hud.h"
#include "native_hud_history.h"
#include "config.h"
#include "inputs.h"
#include "slboot.h"
#include "log.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>
namespace fgvk {
namespace {
template<class H> uint64_t Id(H h){return (uint64_t)(uintptr_t)h;}
struct View {VkImage image;VkFormat format;VkImageSubresourceRange range;};
struct Attachment {VkFormat format;VkSampleCountFlagBits samples;VkAttachmentLoadOp load;VkImageLayout layout;bool color=false;};
struct Pass {std::vector<Attachment> attachments;};
struct Framebuffer {uint32_t width,height,layers;std::vector<VkImageView> views;};
struct Snapshot {VkImage source{},image{};VkImageView view{};VkDeviceMemory memory{};};
struct Chain {
  VkDevice device{};VkPhysicalDevice physical{};PFN_vkGetDeviceProcAddr get{};
  VkFormat format{};VkExtent2D extent{};VkImageUsageFlags usage{};bool usable=false;
  std::vector<Snapshot> slots;
};
std::mutex mutex;
std::unordered_map<VkSwapchainKHR,Chain> chains;
std::unordered_map<VkImageView,View> views;
std::unordered_map<VkRenderPass,Pass> passes;
std::unordered_map<VkFramebuffer,Framebuffer> framebuffers;
NativeHudHistory history;
unsigned captures=0,tags=0,misses=0;
PFN_vkCreateImageView realCreateView{};PFN_vkDestroyImageView realDestroyView{};
PFN_vkCreateFramebuffer realCreateFramebuffer{};PFN_vkDestroyFramebuffer realDestroyFramebuffer{};
PFN_vkCreateRenderPass realCreatePass{};PFN_vkCreateRenderPass2 realCreatePass2{},realCreatePass2KHR{};
PFN_vkDestroyRenderPass realDestroyPass{};
PFN_vkCmdBeginRenderPass realBeginPass{};PFN_vkCmdBeginRenderPass2 realBeginPass2{},realBeginPass2KHR{};
PFN_vkCmdBeginRendering realBeginRendering{},realBeginRenderingKHR{};
template<class T>T Fn(const Chain& c,const char* name){return c.get?reinterpret_cast<T>(c.get(c.device,name)):nullptr;}
void Destroy(Chain& c,Snapshot& s){
  if(s.view)if(auto fn=Fn<PFN_vkDestroyImageView>(c,"vkDestroyImageView"))fn(c.device,s.view,nullptr);
  if(s.image)if(auto fn=Fn<PFN_vkDestroyImage>(c,"vkDestroyImage"))fn(c.device,s.image,nullptr);
  if(s.memory)if(auto fn=Fn<PFN_vkFreeMemory>(c,"vkFreeMemory"))fn(c.device,s.memory,nullptr);
  history.Remove(Id(s.source));s={};
}
bool Allocate(Chain& c,Snapshot& s){
  auto create=Fn<PFN_vkCreateImage>(c,"vkCreateImage");auto requirements=Fn<PFN_vkGetImageMemoryRequirements>(c,"vkGetImageMemoryRequirements");
  auto allocate=Fn<PFN_vkAllocateMemory>(c,"vkAllocateMemory");auto bind=Fn<PFN_vkBindImageMemory>(c,"vkBindImageMemory");
  auto createView=Fn<PFN_vkCreateImageView>(c,"vkCreateImageView");
  auto properties=(PFN_vkGetPhysicalDeviceMemoryProperties)LoaderFn("vkGetPhysicalDeviceMemoryProperties");
  if(!create||!requirements||!allocate||!bind||!createView||!properties || !Fn<PFN_vkCmdCopyImage>(c,"vkCmdCopyImage") || !Fn<PFN_vkCmdPipelineBarrier>(c,"vkCmdPipelineBarrier"))return false;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=c.format;ci.extent={c.extent.width,c.extent.height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
  ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  // Streamline can read on another family, so use concurrent sharing just as
  // the copied depth/MV inputs do; there is no CPU readback or queue wait.
  auto queues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)LoaderFn("vkGetPhysicalDeviceQueueFamilyProperties");
  std::vector<uint32_t> families;
  if(queues){uint32_t n=0;queues(c.physical,&n,nullptr);if(n<=64){std::vector<VkQueueFamilyProperties> q(n);queues(c.physical,&n,q.data());for(uint32_t i=0;i<n;++i)if(q[i].queueCount)families.push_back(i);}}
  if(families.size()>1){ci.sharingMode=VK_SHARING_MODE_CONCURRENT;ci.queueFamilyIndexCount=(uint32_t)families.size();ci.pQueueFamilyIndices=families.data();}
  if(create(c.device,&ci,nullptr,&s.image)!=VK_SUCCESS)return false;
  VkMemoryRequirements req{};requirements(c.device,s.image,&req);VkPhysicalDeviceMemoryProperties mp{};properties(c.physical,&mp);
  uint32_t type=UINT32_MAX;for(uint32_t i=0;i<mp.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
  if(type==UINT32_MAX)return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
  if(allocate(c.device,&ai,nullptr,&s.memory)!=VK_SUCCESS || bind(c.device,s.image,s.memory,0)!=VK_SUCCESS)return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=s.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=c.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  return createView(c.device,&vi,nullptr,&s.view)==VK_SUCCESS;
}
bool Record(VkCommandBuffer cmd,VkImageView view,VkImageLayout layout,VkExtent2D area){
  auto v=views.find(view);if(v==views.end() || v->second.range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || v->second.range.baseMipLevel || v->second.range.levelCount!=1 || v->second.range.baseArrayLayer || v->second.range.layerCount!=1)return false;
  if(layout==VK_IMAGE_LAYOUT_UNDEFINED || layout==VK_IMAGE_LAYOUT_PREINITIALIZED)return false;
  for(auto& item:chains){auto& c=item.second;if(!c.usable || v->second.format!=c.format || area.width!=c.extent.width || area.height!=c.extent.height)continue;
    for(auto& s:c.slots){if(s.source!=v->second.image || !s.image || !history.Record(Id(cmd),Id(s.source)))continue;
      auto barrier=Fn<PFN_vkCmdPipelineBarrier>(c,"vkCmdPipelineBarrier");auto copy=Fn<PFN_vkCmdCopyImage>(c,"vkCmdCopyImage");
      VkImageMemoryBarrier b[2]{};for(auto& x:b){x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
      b[0].image=s.source;b[0].oldLayout=layout;b[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      // Discard the old snapshot contents. A reset/unsubmitted command cannot
      // leave a guessed image layout behind for the next recording.
      b[1].image=s.image;b[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b);
      VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent={c.extent.width,c.extent.height,1};
      copy(cmd,s.source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
      b[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[0].newLayout=layout;b[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
      b[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b[1].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
      barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,b);
      if(++captures<=3)Log("Native HUD-less snapshot recorded: image=%p cmd=%p %ux%u format=%d",(void*)s.source,(void*)cmd,c.extent.width,c.extent.height,(int)c.format);
      return true;
    }
  }
  return false;
}
void BeforePass(VkCommandBuffer cmd,const VkRenderPassBeginInfo* bi){
  if(!bi || (Rt().fgUserOff.load(std::memory_order_relaxed) || !HasRecentDLSSInput()))return;std::lock_guard<std::mutex> lock(mutex);
  auto p=passes.find(bi->renderPass);auto f=framebuffers.find(bi->framebuffer);
  if(p==passes.end() || f==framebuffers.end() || f->second.layers!=1 || bi->renderArea.offset.x || bi->renderArea.offset.y || bi->renderArea.extent.width!=f->second.width || bi->renderArea.extent.height!=f->second.height)return;
  for(size_t i=0;i<p->second.attachments.size() && i<f->second.views.size();++i){auto& a=p->second.attachments[i];if(a.color && a.load==VK_ATTACHMENT_LOAD_OP_LOAD && a.samples==VK_SAMPLE_COUNT_1_BIT)if(Record(cmd,f->second.views[i],a.layout,bi->renderArea.extent))return;}
}
VKAPI_ATTR VkResult VKAPI_CALL CreateView(VkDevice d,const VkImageViewCreateInfo* ci,const VkAllocationCallbacks* a,VkImageView* out){auto r=realCreateView(d,ci,a,out);if(r==VK_SUCCESS && ci && out){std::lock_guard<std::mutex> l(mutex);views[*out]={ci->image,ci->format,ci->subresourceRange};}return r;}
VKAPI_ATTR void VKAPI_CALL DestroyView(VkDevice d,VkImageView v,const VkAllocationCallbacks* a){realDestroyView(d,v,a);std::lock_guard<std::mutex> l(mutex);views.erase(v);}
VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo* ci,const VkAllocationCallbacks* a,VkFramebuffer* out){auto r=realCreateFramebuffer(d,ci,a,out);if(r==VK_SUCCESS && ci && out && !(ci->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)){Framebuffer f{ci->width,ci->height,ci->layers,{}};if(ci->pAttachments)f.views.assign(ci->pAttachments,ci->pAttachments+ci->attachmentCount);std::lock_guard<std::mutex> l(mutex);framebuffers[*out]=std::move(f);}return r;}
VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice d,VkFramebuffer f,const VkAllocationCallbacks* a){realDestroyFramebuffer(d,f,a);std::lock_guard<std::mutex> l(mutex);framebuffers.erase(f);}
template<class Info>void RememberPass(VkRenderPass handle,const Info* ci){Pass p;for(uint32_t i=0;i<ci->attachmentCount;++i){auto& a=ci->pAttachments[i];p.attachments.push_back({a.format,a.samples,a.loadOp,a.initialLayout,false});}if(ci->subpassCount){auto& s=ci->pSubpasses[0];for(uint32_t i=0;i<s.colorAttachmentCount;++i)if(s.pColorAttachments[i].attachment<p.attachments.size())p.attachments[s.pColorAttachments[i].attachment].color=true;}std::lock_guard<std::mutex> l(mutex);passes[handle]=std::move(p);}
VKAPI_ATTR VkResult VKAPI_CALL CreatePass(VkDevice d,const VkRenderPassCreateInfo* ci,const VkAllocationCallbacks* a,VkRenderPass* out){auto r=realCreatePass(d,ci,a,out);if(r==VK_SUCCESS && ci && out)RememberPass(*out,ci);return r;}
VKAPI_ATTR VkResult VKAPI_CALL CreatePass2(VkDevice d,const VkRenderPassCreateInfo2* ci,const VkAllocationCallbacks* a,VkRenderPass* out){auto r=realCreatePass2(d,ci,a,out);if(r==VK_SUCCESS && ci && out)RememberPass(*out,ci);return r;}
VKAPI_ATTR VkResult VKAPI_CALL CreatePass2KHR(VkDevice d,const VkRenderPassCreateInfo2* ci,const VkAllocationCallbacks* a,VkRenderPass* out){auto r=realCreatePass2KHR(d,ci,a,out);if(r==VK_SUCCESS && ci && out)RememberPass(*out,ci);return r;}
VKAPI_ATTR void VKAPI_CALL DestroyPass(VkDevice d,VkRenderPass p,const VkAllocationCallbacks* a){realDestroyPass(d,p,a);std::lock_guard<std::mutex> l(mutex);passes.erase(p);}
VKAPI_ATTR void VKAPI_CALL BeginPass(VkCommandBuffer c,const VkRenderPassBeginInfo* bi,VkSubpassContents s){BeforePass(c,bi);realBeginPass(c,bi,s);}
VKAPI_ATTR void VKAPI_CALL BeginPass2(VkCommandBuffer c,const VkRenderPassBeginInfo* bi,const VkSubpassBeginInfo* si){BeforePass(c,bi);realBeginPass2(c,bi,si);}
VKAPI_ATTR void VKAPI_CALL BeginPass2KHR(VkCommandBuffer c,const VkRenderPassBeginInfo* bi,const VkSubpassBeginInfo* si){BeforePass(c,bi);realBeginPass2KHR(c,bi,si);}
void BeforeRendering(VkCommandBuffer c,const VkRenderingInfo* info){if(!info || Rt().fgUserOff.load(std::memory_order_relaxed) || !HasRecentDLSSInput() || info->renderArea.offset.x || info->renderArea.offset.y || info->layerCount!=1 || info->viewMask)return;std::lock_guard<std::mutex> l(mutex);for(uint32_t i=0;i<info->colorAttachmentCount;++i){auto& a=info->pColorAttachments[i];if(a.loadOp==VK_ATTACHMENT_LOAD_OP_LOAD)Record(c,a.imageView,a.imageLayout,info->renderArea.extent);}}
VKAPI_ATTR void VKAPI_CALL BeginRendering(VkCommandBuffer c,const VkRenderingInfo* i){BeforeRendering(c,i);realBeginRendering(c,i);}
VKAPI_ATTR void VKAPI_CALL BeginRenderingKHR(VkCommandBuffer c,const VkRenderingInfo* i){BeforeRendering(c,i);realBeginRenderingKHR(c,i);}
}
bool NativeHudEnabled(){return Cfg().provider==FGProvider::DLSSG && Cfg().nativeHudProtection;}
void NativeHudRegister(VkDevice d,VkPhysicalDevice pd,VkSwapchainKHR sc,const VkSwapchainCreateInfoKHR* ci,PFN_vkGetDeviceProcAddr get){
  if(!NativeHudEnabled() || !ci)return;std::lock_guard<std::mutex> l(mutex);if(chains.count(sc))return;
  Chain c;c.device=d;c.physical=pd;c.get=get;c.format=ci->imageFormat;c.extent=ci->imageExtent;c.usage=ci->imageUsage;
  auto format=(PFN_vkGetPhysicalDeviceFormatProperties)LoaderFn("vkGetPhysicalDeviceFormatProperties");VkFormatProperties props{};if(format)format(pd,c.format,&props);
  c.usable=ci->imageArrayLayers==1 && (ci->imageUsage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (props.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (props.optimalTilingFeatures&VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
  Log("Native HUD capture registered: swapchain=%p usable=%d usage=0x%x format=%d",(void*)sc,c.usable,(unsigned)ci->imageUsage,(int)c.format);chains.emplace(sc,std::move(c));
}
void NativeHudImages(VkDevice d,VkSwapchainKHR sc,uint32_t count,const VkImage* images){
  if(!NativeHudEnabled() || !images || count<2 || count>8)return;std::lock_guard<std::mutex> l(mutex);auto found=chains.find(sc);if(found==chains.end() || found->second.device!=d || !found->second.usable || !found->second.slots.empty())return;
  auto& c=found->second;c.slots.resize(count);bool ok=true;
  for(uint32_t i=0;i<count;++i){auto& s=c.slots[i];s.source=images[i];if(!Allocate(c,s)){ok=false;break;}history.Add(Id(s.source));}
  if(!ok){for(auto& s:c.slots)Destroy(c,s);c.slots.clear();c.usable=false;Log("Native HUD capture allocation unavailable; frame generation preserved");}
}
void NativeHudDestroy(VkDevice d,VkSwapchainKHR sc){if(!NativeHudEnabled())return;std::lock_guard<std::mutex> l(mutex);auto i=chains.find(sc);if(i!=chains.end() && i->second.device==d){for(auto& s:i->second.slots)Destroy(i->second,s);chains.erase(i);}}
void NativeHudAcquire(VkSwapchainKHR sc,uint32_t index){if(!NativeHudEnabled())return;std::lock_guard<std::mutex> l(mutex);auto c=chains.find(sc);if(c!=chains.end() && index<c->second.slots.size())history.Acquire(Id(c->second.slots[index].source));}
void NativeHudForget(VkCommandBuffer cmd){if(!NativeHudEnabled())return;std::lock_guard<std::mutex> l(mutex);history.Forget(Id(cmd));}
void NativeHudExecute(VkCommandBuffer c,uint32_t count,const VkCommandBuffer* children){if(!NativeHudEnabled())return;std::vector<uint64_t> cs;for(uint32_t i=0;i<count;++i)cs.push_back(Id(children[i]));std::lock_guard<std::mutex> l(mutex);history.Execute(Id(c),cs);}
void NativeHudSubmit(uint32_t count,const VkCommandBuffer* cmds,uint32_t token){if(!NativeHudEnabled())return;std::lock_guard<std::mutex> l(mutex);for(uint32_t i=0;i<count;++i)history.Submit(Id(cmds[i]),token);}
void NativeHudTag(const VkPresentInfoKHR* pi,sl::FrameToken& frame,uint32_t token){
  if(!NativeHudEnabled())return;auto& f=GetSlFns();if(!f.setTagForFrame)return;
  sl::Resource resource{};sl::Extent extent{};bool matched=false;
  {std::lock_guard<std::mutex> l(mutex);if(!Rt().fgUserOff.load(std::memory_order_relaxed) && pi && pi->swapchainCount==1 && pi->pSwapchains && pi->pImageIndices){auto c=chains.find(pi->pSwapchains[0]);if(c!=chains.end() && pi->pImageIndices[0]<c->second.slots.size()){
    auto& chain=c->second;auto& s=chain.slots[pi->pImageIndices[0]];
    if(s.image && history.Matches(Id(s.source),token)){resource=sl::Resource(sl::ResourceType::eTex2d,(void*)s.image,(void*)s.memory,(void*)s.view,(uint32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);resource.width=chain.extent.width;resource.height=chain.extent.height;resource.nativeFormat=chain.format;resource.mipLevels=resource.arrayLayers=1;resource.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;extent={0,0,chain.extent.width,chain.extent.height};matched=true;}
  }}}
  // Null on every miss: Streamline must not carry a previous frame's tag
  // through a quality change, command reset, loading screen or restart.
  sl::ResourceTag tag(matched?&resource:nullptr,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent,matched?&extent:nullptr);
  auto result=f.setTagForFrame(frame,sl::ViewportHandle{0},&tag,1,nullptr);
  if(matched && result!=sl::Result::eOk){sl::ResourceTag clear(nullptr,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent);f.setTagForFrame(frame,sl::ViewportHandle{0},&clear,1,nullptr);}
  if(matched && result==sl::Result::eOk){if(++tags<=3 || (DebugLoggingEnabled() && tags%300==0))Log("Native HUD-less tag matched: token=%u image=%u size=%ux%u",token,pi->pImageIndices[0],extent.width,extent.height);}
  else if(++misses<=3 || (DebugLoggingEnabled() && misses%300==0))Log("Native HUD-less tag unavailable: token=%u matched=%d result=%d; original FG preserved",token,matched,(int)result);
}
PFN_vkVoidFunction NativeHudWrap(const char* name,PFN_vkVoidFunction original){
  if(!NativeHudEnabled() || !original || !name)return original;
#define HUD_HOOK(api,real,wrap) if(!strcmp(name,api)){real=reinterpret_cast<decltype(real)>(original);return reinterpret_cast<PFN_vkVoidFunction>(wrap);}
  HUD_HOOK("vkCreateImageView",realCreateView,CreateView) HUD_HOOK("vkDestroyImageView",realDestroyView,DestroyView)
  HUD_HOOK("vkCreateFramebuffer",realCreateFramebuffer,CreateFramebuffer) HUD_HOOK("vkDestroyFramebuffer",realDestroyFramebuffer,DestroyFramebuffer)
  HUD_HOOK("vkCreateRenderPass",realCreatePass,CreatePass) HUD_HOOK("vkCreateRenderPass2",realCreatePass2,CreatePass2) HUD_HOOK("vkCreateRenderPass2KHR",realCreatePass2KHR,CreatePass2KHR) HUD_HOOK("vkDestroyRenderPass",realDestroyPass,DestroyPass)
  HUD_HOOK("vkCmdBeginRenderPass",realBeginPass,BeginPass) HUD_HOOK("vkCmdBeginRenderPass2",realBeginPass2,BeginPass2) HUD_HOOK("vkCmdBeginRenderPass2KHR",realBeginPass2KHR,BeginPass2KHR)
  HUD_HOOK("vkCmdBeginRendering",realBeginRendering,BeginRendering) HUD_HOOK("vkCmdBeginRenderingKHR",realBeginRenderingKHR,BeginRenderingKHR)
#undef HUD_HOOK
  return original;
}
}
