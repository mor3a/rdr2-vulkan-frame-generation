#include "hud_mask.h"
#include "hud_mask_spv.h"
#include "log.h"
#include "ffx_api/vk/ffx_api_vk.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fgvk {
namespace {
constexpr uint32_t SetCount=3;
VkDevice device{}; VkPhysicalDevice physical{}; PFN_vkGetDeviceProcAddr gdpa{};
VkFormat format{}; VkExtent2D extent{}; VkImage outputImage{}; VkDeviceMemory outputMemory{};
VkImageView outputView{}; VkSampler sampler{}; VkDescriptorSetLayout setLayout{}; VkDescriptorPool pool{};
VkDescriptorSet sets[SetCount]{}; VkPipelineLayout pipelineLayout{}; VkPipeline pipeline{};
uint32_t setCursor{}; bool outputInitialized{}; bool failedLogged{}; bool successLogged{};
std::unordered_map<VkImage,VkImageView> imageViews;
std::mutex gate;

template<class T> T D(const char* n){return gdpa&&device?reinterpret_cast<T>(gdpa(device,n)):nullptr;}
void FailOnce(const char* message){if(!failedLogged){failedLogged=true;Log("HUD mask unavailable: %s",message);}}

void DestroyAll(){
  if(device&&gdpa){
    auto destroyView=D<PFN_vkDestroyImageView>("vkDestroyImageView");
    auto destroySampler=D<PFN_vkDestroySampler>("vkDestroySampler");
    auto destroyPool=D<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    auto destroyLayout=D<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    auto destroyPipeline=D<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    auto destroyPipelineLayout=D<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    auto destroyImage=D<PFN_vkDestroyImage>("vkDestroyImage");
    auto freeMemory=D<PFN_vkFreeMemory>("vkFreeMemory");
    if(destroyView){for(auto& item:imageViews)if(item.second)destroyView(device,item.second,nullptr);if(outputView)destroyView(device,outputView,nullptr);}
    if(destroySampler&&sampler)destroySampler(device,sampler,nullptr);
    if(destroyPool&&pool)destroyPool(device,pool,nullptr);
    if(destroyPipeline&&pipeline)destroyPipeline(device,pipeline,nullptr);
    if(destroyPipelineLayout&&pipelineLayout)destroyPipelineLayout(device,pipelineLayout,nullptr);
    if(destroyLayout&&setLayout)destroyLayout(device,setLayout,nullptr);
    if(destroyImage&&outputImage)destroyImage(device,outputImage,nullptr);
    if(freeMemory&&outputMemory)freeMemory(device,outputMemory,nullptr);
  }
  imageViews.clear();outputImage={};outputMemory={};outputView={};sampler={};setLayout={};pool={};
  for(auto& s:sets)s={};pipelineLayout={};pipeline={};outputInitialized=false;
}

bool CreateOutput(){
  auto create=D<PFN_vkCreateImage>("vkCreateImage");auto getReq=D<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
  auto allocate=D<PFN_vkAllocateMemory>("vkAllocateMemory");auto bind=D<PFN_vkBindImageMemory>("vkBindImageMemory");
  if(!create||!getReq||!allocate||!bind)return false;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent={extent.width,extent.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
  ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(create(device,&ci,nullptr,&outputImage)!=VK_SUCCESS)return false;
  VkMemoryRequirements req{};getReq(device,outputImage,&req);
  uint32_t type=UINT32_MAX;for(uint32_t i=0;i<32;++i)if(req.memoryTypeBits&(1u<<i)){type=i;break;}
  if(type==UINT32_MAX)return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
  if(allocate(device,&ai,nullptr,&outputMemory)!=VK_SUCCESS||bind(device,outputImage,outputMemory,0)!=VK_SUCCESS)return false;
  auto createView=D<PFN_vkCreateImageView>("vkCreateImageView");if(!createView)return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=outputImage;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};return createView(device,&vi,nullptr,&outputView)==VK_SUCCESS;
}

VkImageView GetView(VkImage image){
  auto found=imageViews.find(image);if(found!=imageViews.end())return found->second;
  auto create=D<PFN_vkCreateImageView>("vkCreateImageView");if(!create)return VK_NULL_HANDLE;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
  vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view{};
  if(create(device,&vi,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
  imageViews.emplace(image,view);return view;
}

bool CreatePipeline(){
  auto createSetLayout=D<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
  auto createPool=D<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool");auto allocSets=D<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
  auto createSampler=D<PFN_vkCreateSampler>("vkCreateSampler");auto createPL=D<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
  auto createShader=D<PFN_vkCreateShaderModule>("vkCreateShaderModule");auto destroyShader=D<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
  auto createPipelines=D<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
  if(!createSetLayout||!createPool||!allocSets||!createSampler||!createPL||!createShader||!destroyShader||!createPipelines)return false;
  VkDescriptorSetLayoutBinding bindings[4]{};
  for(uint32_t i=0;i<3;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
  bindings[3].binding=3;bindings[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;bindings[3].descriptorCount=1;bindings[3].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sli.bindingCount=4;sli.pBindings=bindings;
  if(createSetLayout(device,&sli,nullptr,&setLayout)!=VK_SUCCESS)return false;
  VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3*SetCount},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,SetCount}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=SetCount;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;
  if(createPool(device,&dpi,nullptr,&pool)!=VK_SUCCESS)return false;
  VkDescriptorSetLayout layouts[SetCount]{setLayout,setLayout,setLayout};VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=pool;dai.descriptorSetCount=SetCount;dai.pSetLayouts=layouts;
  if(allocSets(device,&dai,sets)!=VK_SUCCESS)return false;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
  if(createSampler(device,&si,nullptr,&sampler)!=VK_SUCCESS)return false;
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,12};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount=1;pli.pSetLayouts=&setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
  if(createPL(device,&pli,nullptr,&pipelineLayout)!=VK_SUCCESS)return false;
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=HudMaskSpirvSize;smi.pCode=HudMaskSpirv;
  VkShaderModule shader{};if(createShader(device,&smi,nullptr,&shader)!=VK_SUCCESS)return false;
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;stage.module=shader;stage.pName="main";
  VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.stage=stage;pci.layout=pipelineLayout;
  VkResult result=createPipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline);destroyShader(device,shader,nullptr);return result==VK_SUCCESS;
}

void Barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage){
  auto fn=D<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");if(!fn)return;
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=oldLayout;b.newLayout=newLayout;
  b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  fn(cmd,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&b);
}
}

void HudMaskInit(VkDevice d,VkPhysicalDevice pd,PFN_vkGetDeviceProcAddr getProc,VkFormat fmt,VkExtent2D size){
  std::lock_guard<std::mutex> lock(gate);DestroyAll();device=d;physical=pd;gdpa=getProc;format=fmt;extent=size;failedLogged=false;successLogged=false;setCursor=0;
  if((fmt!=VK_FORMAT_R8G8B8A8_UNORM&&fmt!=VK_FORMAT_B8G8R8A8_UNORM)||!size.width||!size.height){FailOnce("only R8G8B8A8_UNORM and B8G8R8A8_UNORM SDR are supported");return;}
  if(!CreateOutput()||!CreatePipeline()){DestroyAll();FailOnce("Vulkan resource or pipeline creation failed");return;}
}

void HudMaskDestroy(){std::lock_guard<std::mutex> lock(gate);DestroyAll();device={};physical={};gdpa=nullptr;format=VK_FORMAT_UNDEFINED;extent={};}

bool HudMaskApply(VkCommandBuffer cmd,FfxApiResource real,FfxApiResource hudless,FfxApiResource generated){
  std::lock_guard<std::mutex> lock(gate);
  if(!pipeline||!cmd||!real.resource||!hudless.resource||!generated.resource)return false;
  if(real.description.width!=extent.width||real.description.height!=extent.height||hudless.description.width!=extent.width||hudless.description.height!=extent.height||
     generated.description.width!=extent.width||generated.description.height!=extent.height||real.description.format!=ffxApiGetSurfaceFormatVK(format)||
     hudless.description.format!=real.description.format||generated.description.format!=real.description.format)return false;
  auto realImage=(VkImage)real.resource;auto hudlessImage=(VkImage)hudless.resource;auto generatedImage=(VkImage)generated.resource;
  VkImageView rv=GetView(realImage),hv=GetView(hudlessImage),gv=GetView(generatedImage);
  if(!rv||!hv||!gv){FailOnce("input image view creation failed");return false;}
  auto update=D<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");auto bindPipeline=D<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
  auto bindSets=D<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");auto push=D<PFN_vkCmdPushConstants>("vkCmdPushConstants");
  auto dispatch=D<PFN_vkCmdDispatch>("vkCmdDispatch");auto copy=D<PFN_vkCmdCopyImage>("vkCmdCopyImage");auto barrier=D<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  if(!update||!bindPipeline||!bindSets||!push||!dispatch||!copy||!barrier){FailOnce("required Vulkan commands are unavailable");return false;}
  uint32_t slot=setCursor++%SetCount;VkDescriptorImageInfo images[4]{{sampler,rv,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,hv,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,gv,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,outputView,VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[4]{};for(uint32_t i=0;i<4;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=sets[slot];writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i==3?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];}
  update(device,4,writes,0,nullptr);
  Barrier(cmd,outputImage,outputInitialized?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
          outputInitialized?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  Barrier(cmd,generatedImage,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  bindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bindSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&sets[slot],0,nullptr);
  uint32_t params[3]={extent.width,extent.height,format==VK_FORMAT_B8G8R8A8_UNORM?1u:0u};push(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),params);
  dispatch(cmd,(extent.width+7)/8,(extent.height+7)/8,1);
  Barrier(cmd,outputImage,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  Barrier(cmd,generatedImage,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;region.extent={extent.width,extent.height,1};
  copy(cmd,outputImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,generatedImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
  Barrier(cmd,generatedImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  outputInitialized=true;
  if(!successLogged){successLogged=true;Log("HUD mask first success: image=%ux%u format=%u",extent.width,extent.height,(uint32_t)format);}
  return true;
}
}
