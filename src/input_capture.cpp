#include "input_capture.h"
#include "vkhooks.h"
#include "log.h"
#include <windows.h>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <cstdio>
#include <cstring>
namespace fgvk { namespace {
#define CAPTURE_FUNCTIONS(X) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(QueueSubmit) X(CmdPipelineBarrier) X(CmdCopyImageToBuffer)
struct Functions {
#define FIELD(n) PFN_vk##n n{};
CAPTURE_FUNCTIONS(FIELD)
#undef FIELD
} vk;
struct Slot {VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkFence fence{};uint32_t dw{},dh{},mw{},mh{},token{},serial{};size_t bytes{};bool recorded=false,submitted=false;std::unordered_set<VkCommandBuffer> commands;sl::Constants constants{};};
Slot slots[4];std::mutex mutex;bool initialized=false,failed=false,armed=false;unsigned recorded=0;ULONGLONG nextCheck=0;wchar_t directory[MAX_PATH]{};
bool Init(){
 if(initialized)return true;if(failed)return false;
#define LOAD(n) vk.n=(PFN_vk##n)DeviceFn("vk" #n);if(!vk.n){failed=true;return false;}
CAPTURE_FUNCTIONS(LOAD)
#undef LOAD
 GetModuleFileNameW(nullptr,directory,MAX_PATH);wchar_t* slash=wcsrchr(directory,L'\\');if(!slash)return false;*slash=0;
 wcscat_s(directory,L"\\RDR2VulkanFG\\Capture");CreateDirectoryW(directory,nullptr);initialized=true;return true;
}
bool Allocate(Slot& s,uint32_t dw,uint32_t dh,uint32_t mw,uint32_t mh){
 if(s.buffer)return s.dw==dw && s.dh==dh && s.mw==mw && s.mh==mh;
 s.dw=dw;s.dh=dh;s.mw=mw;s.mh=mh;s.bytes=(size_t(dw)*dh+size_t(mw)*mh)*4;
 VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=s.bytes;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 if(vk.CreateBuffer(gDevice,&ci,nullptr,&s.buffer)!=VK_SUCCESS)return false;
 VkMemoryRequirements req{};vk.GetBufferMemoryRequirements(gDevice,s.buffer,&req);
 auto properties=(PFN_vkGetPhysicalDeviceMemoryProperties)LoaderFn("vkGetPhysicalDeviceMemoryProperties");if(!properties)return false;
 VkPhysicalDeviceMemoryProperties memory{};properties(gPhysicalDevice,&memory);unsigned type=UINT32_MAX;
 for(unsigned i=0;i<memory.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
 if(type==UINT32_MAX)return false;
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
 if(vk.AllocateMemory(gDevice,&ai,nullptr,&s.memory)!=VK_SUCCESS || vk.BindBufferMemory(gDevice,s.buffer,s.memory,0)!=VK_SUCCESS || vk.MapMemory(gDevice,s.memory,0,VK_WHOLE_SIZE,0,&s.mapped)!=VK_SUCCESS)return false;
 VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};return vk.CreateFence(gDevice,&fi,nullptr,&s.fence)==VK_SUCCESS;
}
void Write(Slot& s){
 wchar_t path[MAX_PATH]{};swprintf_s(path,L"%s\\input-%03u.bin",directory,s.serial);FILE* f=nullptr;_wfopen_s(&f,path,L"wb");if(!f)return;
 uint32_t header[]={0x4647494e,1,s.serial,s.token,s.dw,s.dh,s.mw,s.mh};fwrite(header,sizeof(header),1,f);fwrite(s.mapped,s.bytes,1,f);fclose(f);
 swprintf_s(path,L"%s\\input-%03u.txt",directory,s.serial);_wfopen_s(&f,path,L"w");if(!f)return;
 const auto& c=s.constants;fprintf(f,"token=%u\nsize=%u,%u,%u,%u\njitter=%g,%g\nscale=%g,%g\nposition=%g,%g,%g\nfov=%g\nreset=%u\n",s.token,s.dw,s.dh,s.mw,s.mh,c.jitterOffset.x,c.jitterOffset.y,c.mvecScale.x,c.mvecScale.y,c.cameraPos.x,c.cameraPos.y,c.cameraPos.z,c.cameraFOV,(unsigned)c.reset);
 const sl::float4x4* matrices[]={&c.cameraViewToClip,&c.clipToPrevClip,&c.prevClipToClip};const char* names[]={"projection","currentToPrevious","previousToCurrent"};
 for(unsigned i=0;i<3;++i){fprintf(f,"%s=",names[i]);for(unsigned row=0;row<4;++row){const auto& v=(*matrices[i])[row];fprintf(f,"%.9g,%.9g,%.9g,%.9g%s",v.x,v.y,v.z,v.w,row==3?"\n":",");}}
 fclose(f);Log("Input capture saved: frame=%u token=%u depth=%ux%u motion=%ux%u",s.serial,s.token,s.dw,s.dh,s.mw,s.mh);
}
}
void InputCaptureRecord(VkCommandBuffer cmd,uint32_t token,VkImage depth,VkImage motion,uint32_t dw,uint32_t dh,uint32_t mw,uint32_t mh,VkImageLayout dl,VkImageLayout ml,const sl::Constants& c){
 std::lock_guard<std::mutex> lock(mutex);
 if(!armed && GetTickCount64()>=nextCheck){nextCheck=GetTickCount64()+1000;wchar_t marker[MAX_PATH]{};GetModuleFileNameW(nullptr,marker,MAX_PATH);auto slash=wcsrchr(marker,L'\\');if(slash)wcscpy_s(slash+1,MAX_PATH-(slash+1-marker),L"RDR2VulkanFG.capture");if(GetFileAttributesW(marker)!=INVALID_FILE_ATTRIBUTES){DeleteFileW(marker);armed=true;recorded=0;Log("Input capture armed: 24 consecutive DLSS input pairs, fence-verified readback");}}
 if(!armed || recorded>=24 || !Init())return;
 Slot* slot=nullptr;for(auto& s:slots)if(!s.recorded){slot=&s;break;}if(!slot)return;
 auto& s=*slot;if(!Allocate(s,dw,dh,mw,mh)){failed=true;Log("Input capture allocation unavailable; capture stopped");armed=false;return;}
 s.token=token;s.constants=c;s.serial=++recorded;s.commands.clear();s.commands.insert(cmd);s.recorded=true;s.submitted=false;
 VkImageMemoryBarrier barriers[2]{};VkImage images[]={depth,motion};VkImageLayout layouts[]={dl,ml};
 for(unsigned i=0;i<2;++i){auto& b=barriers[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=images[i];b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.oldLayout=layouts[i];b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={i==0?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
 // Depth/stencil layouts are coupled on devices without separateDepthStencilLayouts.
 barriers[0].subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
 vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers);
 for(unsigned i=0;i<2;++i){VkBufferImageCopy region{};region.bufferOffset=i?size_t(dw)*dh*4:0;region.imageSubresource={i?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.imageExtent={i?mw:dw,i?mh:dh,1};vk.CmdCopyImageToBuffer(cmd,images[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.buffer,1,&region);}
 for(unsigned i=0;i<2;++i){auto& b=barriers[i];b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=layouts[i];}
 VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=s.buffer;host.size=VK_WHOLE_SIZE;
 vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,2,barriers);
}
void InputCaptureExecuted(VkCommandBuffer primary,uint32_t n,const VkCommandBuffer* secondary){std::lock_guard<std::mutex> lock(mutex);for(auto& s:slots)if(s.recorded && !s.submitted)for(uint32_t i=0;i<n;++i)if(s.commands.count(secondary[i]))s.commands.insert(primary);}
void InputCaptureForget(VkCommandBuffer cmd){std::lock_guard<std::mutex> lock(mutex);for(auto& s:slots)if(s.recorded && !s.submitted)s.commands.erase(cmd);}
void InputCaptureSubmitted(VkQueue queue,uint32_t n,const VkCommandBuffer* commands){
 static thread_local bool inside=false;if(inside)return;inside=true;
 {std::lock_guard<std::mutex> lock(mutex);if(initialized)for(auto& s:slots)if(s.recorded && !s.submitted)for(uint32_t i=0;i<n;++i)if(s.commands.count(commands[i])){VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};if(SubmitInternalReadbackFence(queue,s.fence)==VK_SUCCESS)s.submitted=true;break;}}
 inside=false;
}
void InputCapturePoll(){std::lock_guard<std::mutex> lock(mutex);if(!initialized)return;for(auto& s:slots)if(s.submitted && vk.GetFenceStatus(gDevice,s.fence)==VK_SUCCESS){Write(s);vk.ResetFences(gDevice,1,&s.fence);s.submitted=s.recorded=false;s.commands.clear();}if(recorded>=24)armed=false;}
}
