#include "xess_bridge_device.h"
#include "bridge_format.h"
#include <cstring>

namespace fgvk::xess_bridge {
bool Device::Initialize(VkInstance instance,VkPhysicalDevice physical,VkDevice vk,
                       PFN_vkGetInstanceProcAddr gi,PFN_vkGetDeviceProcAddr gd,bool enabled){
  if(dx_ || vk_)return Fail("Bridge device already initialized");
  if(!instance || !physical || !vk || !gi || !gd || !enabled)
    return Fail("External-memory extensions must be enabled on the game device first");
  auto props=(PFN_vkGetPhysicalDeviceProperties2)gi(instance,"vkGetPhysicalDeviceProperties2");
  memoryProperties_=(PFN_vkGetPhysicalDeviceMemoryProperties)gi(instance,"vkGetPhysicalDeviceMemoryProperties");
  imageProperties_=(PFN_vkGetPhysicalDeviceImageFormatProperties2)gi(instance,"vkGetPhysicalDeviceImageFormatProperties2");
  if(!props || !memoryProperties_ || !imageProperties_)return Fail("Vulkan adapter query unavailable");
#define LOAD(member,type,name) member=(type)gd(vk,name);if(!member)return Fail("Missing " name)
  LOAD(createImage_,PFN_vkCreateImage,"vkCreateImage");
  LOAD(destroyImage_,PFN_vkDestroyImage,"vkDestroyImage");
  LOAD(imageRequirements_,PFN_vkGetImageMemoryRequirements,"vkGetImageMemoryRequirements");
  LOAD(handleProperties_,PFN_vkGetMemoryWin32HandlePropertiesKHR,"vkGetMemoryWin32HandlePropertiesKHR");
  LOAD(allocate_,PFN_vkAllocateMemory,"vkAllocateMemory");
  LOAD(free_,PFN_vkFreeMemory,"vkFreeMemory");
  LOAD(bind_,PFN_vkBindImageMemory,"vkBindImageMemory");
  LOAD(createSemaphore_,PFN_vkCreateSemaphore,"vkCreateSemaphore");
  LOAD(destroySemaphore_,PFN_vkDestroySemaphore,"vkDestroySemaphore");
  LOAD(importSemaphore_,PFN_vkImportSemaphoreWin32HandleKHR,"vkImportSemaphoreWin32HandleKHR");
#undef LOAD
  VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 pd{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};pd.pNext=&id;props(physical,&pd);
  if(!id.deviceLUIDValid)return Fail("Vulkan adapter does not report a valid Windows LUID");
  if(id.deviceNodeMask!=1)return Fail("Bridge currently requires a single-node Vulkan adapter");
  LUID luid{};static_assert(sizeof(luid)==VK_LUID_SIZE);std::memcpy(&luid,id.deviceLUID,sizeof(luid));
  IDXGIFactory4* factory=nullptr;
  HRESULT hr=CreateDXGIFactory1(__uuidof(IDXGIFactory4),(void**)&factory);
  if(FAILED(hr))return Fail("DXGI factory creation failed",hr);
  IDXGIAdapter1* adapter=nullptr;
  hr=factory->EnumAdapterByLuid(luid,__uuidof(IDXGIAdapter1),(void**)&adapter);factory->Release();
  if(FAILED(hr))return Fail("No DXGI adapter matches the game's Vulkan device LUID",hr);
  hr=D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),(void**)&dx_);adapter->Release();
  if(FAILED(hr)){dx_=nullptr;return Fail("Matched adapter cannot create a DirectX12 device",hr);}
  vk_=vk;physical_=physical;error_="";nativeResult_=0;return true;
}

bool Device::CreateImage(uint32_t w,uint32_t h,DXGI_FORMAT df,VkFormat vf,SharedImage& out){
  if(!dx_ || !vk_ || !w || !h || out.dx || out.vk || out.memory)return Fail("Invalid shared-image request");
  // No format reinterpretation: depth/stencil inputs need a separate conversion
  // path before crossing to an R32_FLOAT image. They must not be passed here.
  if(!CompatibleTransportFormats(df,vf))return Fail("Unsupported or mismatched DX12/Vulkan image formats");
  // Packed RGB10 and BGRA transport is sampled/copied only and does not
  // require optional typed UAV support on either API.
  const bool storage=TransportNeedsStorage(df);
  const VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
    (storage?VK_IMAGE_USAGE_STORAGE_BIT:0);
  VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  external.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
  VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  query.pNext=&external;query.format=vf;query.type=VK_IMAGE_TYPE_2D;query.tiling=VK_IMAGE_TILING_OPTIMAL;query.usage=usage;
  VkExternalImageFormatProperties capabilities{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};properties.pNext=&capabilities;
  VkResult result=imageProperties_(physical_,&query,&properties);
  if(result!=VK_SUCCESS)return Fail("External texture format query failed",result);
  if(!(capabilities.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
    return Fail("Format does not support importing a shared D3D12 texture");
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width=w;desc.Height=h;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=df;
  desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags=storage?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
  SharedImage image{};HANDLE handle=nullptr;
  HRESULT hr=dx_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),(void**)&image.dx);
  if(FAILED(hr))return Fail("Shared D3D12 texture allocation failed",hr);
  auto fail=[&](const char* reason,int64_t native=0){if(handle)CloseHandle(handle);DestroyImage(image);return Fail(reason,native);};
  hr=dx_->CreateSharedHandle(image.dx,nullptr,GENERIC_ALL,nullptr,&handle);
  if(FAILED(hr))return fail("D3D12 texture handle export failed",hr);
  VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.pNext=&ext;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=vf;
  ci.extent={w,h,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
  ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  result=createImage_(vk_,&ci,nullptr,&image.vk);
  if(result!=VK_SUCCESS)return fail("Shared Vulkan image creation failed",result);
  VkMemoryRequirements requirements{};imageRequirements_(vk_,image.vk,&requirements);
  VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
  result=handleProperties_(vk_,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,handle,&hp);
  if(result!=VK_SUCCESS)return fail("Shared texture memory compatibility query failed",result);
  VkPhysicalDeviceMemoryProperties mp{};memoryProperties_(physical_,&mp);uint32_t type=UINT32_MAX;
  for(uint32_t i=0;i<mp.memoryTypeCount;++i)if((requirements.memoryTypeBits&hp.memoryTypeBits&(1u<<i)) &&
      (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
  if(type==UINT32_MAX)return fail("No compatible shared texture memory type");
  VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
  imp.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;imp.handle=handle;
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.pNext=&imp;dedicated.image=image.vk;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.pNext=&dedicated;ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
  result=allocate_(vk_,&ai,nullptr,&image.memory);
  if(result!=VK_SUCCESS)return fail("D3D12 texture memory import failed",result);
  result=bind_(vk_,image.vk,image.memory,0);
  if(result!=VK_SUCCESS)return fail("Shared Vulkan texture memory binding failed",result);
  CloseHandle(handle);out=image;error_="";nativeResult_=0;return true;
}
void Device::DestroyImage(SharedImage& image){
  if(image.vk && destroyImage_)destroyImage_(vk_,image.vk,nullptr);
  if(image.memory && free_)free_(vk_,image.memory,nullptr);
  if(image.dx)image.dx->Release();image=SharedImage{};
}
bool Device::CreateFence(SharedFence& out){
  if(!dx_ || !vk_ || out.dx || out.vk)return Fail("Invalid shared fence request");
  SharedFence fence{};HANDLE handle=nullptr;
  HRESULT hr=dx_->CreateFence(0,D3D12_FENCE_FLAG_SHARED,__uuidof(ID3D12Fence),(void**)&fence.dx);
  if(FAILED(hr))return Fail("D3D12 shared fence creation failed",hr);
  auto fail=[&](const char* reason,int64_t native=0){if(handle)CloseHandle(handle);DestroyFence(fence);return Fail(reason,native);};
  hr=dx_->CreateSharedHandle(fence.dx,nullptr,GENERIC_ALL,nullptr,&handle);
  if(FAILED(hr))return fail("D3D12 shared fence export failed",hr);
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};ci.pNext=&type;
  VkResult result=createSemaphore_(vk_,&ci,nullptr,&fence.vk);
  if(result!=VK_SUCCESS)return fail("Vulkan timeline semaphore creation failed",result);
  VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
  imp.semaphore=fence.vk;imp.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;imp.handle=handle;
  result=importSemaphore_(vk_,&imp);
  if(result!=VK_SUCCESS)return fail("D3D12 fence import into Vulkan failed",result);
  CloseHandle(handle);out=fence;error_="";nativeResult_=0;return true;
}
void Device::DestroyFence(SharedFence& fence){
  if(fence.vk && destroySemaphore_)destroySemaphore_(vk_,fence.vk,nullptr);
  if(fence.dx)fence.dx->Release();fence=SharedFence{};
}
void Device::Shutdown(){if(dx_)dx_->Release();dx_=nullptr;vk_=VK_NULL_HANDLE;physical_=VK_NULL_HANDLE;}
}
