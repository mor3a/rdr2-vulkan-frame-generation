#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sl.h"
#include <cstdio>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vector>
int wmain(int argc,wchar_t** argv){
 if(argc<2)return 2;
 if(argc>2){
  auto core=LoadLibraryExW(argv[2],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
  auto wait=core?(BOOL(WINAPI*)(DWORD))GetProcAddress(core,"MfgUnlockWaitStartup"):nullptr;
  if(!wait || !wait(15000)){puts("Unlock startup failed");return 9;}
 }
 auto module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
 if(!module){printf("Load error %lu\n",GetLastError());return 3;}
 auto init=(PFun_slInit*)GetProcAddress(module,"slInit");
 auto shutdown=(PFun_slShutdown*)GetProcAddress(module,"slShutdown");
 if(!init || !shutdown)return 4;
 wchar_t folder[1024]{};wcscpy_s(folder,argv[1]);auto slash=wcsrchr(folder,L'\\');if(!slash)return 5;*slash=0;
 const wchar_t* paths[]={folder};
 const sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
 sl::Preferences p{};p.pathsToPlugins=paths;p.numPathsToPlugins=1;
 p.featuresToLoad=features;p.numFeaturesToLoad=3;p.renderAPI=sl::RenderAPI::eVulkan;
 p.flags=sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseManualHooking|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
 p.logLevel=sl::LogLevel::eOff;
 p.logMessageCallback=[](sl::LogType type,const char* message){if(type==sl::LogType::eError)printf("SL: %s\n",message);};
 auto result=init(p,sl::kSDKVersion);printf("slInit result=%d\n",int(result));
 if(result!=sl::Result::eOk)return 6;
 for(const auto* name:{L"sl.common.dll",L"sl.pcl.dll",L"sl.reflex.dll",L"sl.dlss_g.dll"})
  printf("Plugin %ls: %s\n",name,GetModuleHandleW(name)?"loaded":"not loaded");
 if(argc>3){
  auto createInstance=(PFN_vkCreateInstance)GetProcAddress(module,"vkCreateInstance");
  auto enumerate=(PFN_vkEnumeratePhysicalDevices)GetProcAddress(module,"vkEnumeratePhysicalDevices");
  auto properties=(PFN_vkGetPhysicalDeviceProperties)GetProcAddress(module,"vkGetPhysicalDeviceProperties");
  auto families=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(module,"vkGetPhysicalDeviceQueueFamilyProperties");
  auto createDevice=(PFN_vkCreateDevice)GetProcAddress(module,"vkCreateDevice");
  auto destroyDevice=(PFN_vkDestroyDevice)GetProcAddress(module,"vkDestroyDevice");
  auto destroyInstance=(PFN_vkDestroyInstance)GetProcAddress(module,"vkDestroyInstance");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="RDR2VulkanFG queue allocation fixture";app.apiVersion=VK_API_VERSION_1_3;
  const char* exts[]={VK_KHR_SURFACE_EXTENSION_NAME,"VK_KHR_win32_surface"};
  VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;ic.enabledExtensionCount=2;ic.ppEnabledExtensionNames=exts;
  VkInstance instance{};auto vr=createInstance(&ic,nullptr,&instance);printf("vkCreateInstance=%d\n",vr);if(vr!=VK_SUCCESS)return 10;
  uint32_t n=0;enumerate(instance,&n,nullptr);std::vector<VkPhysicalDevice> physicals(n);enumerate(instance,&n,physicals.data());
  VkPhysicalDevice physical{};for(auto candidate:physicals){VkPhysicalDeviceProperties prop{};properties(candidate,&prop);if(prop.vendorID==0x10de){physical=candidate;break;}}
  if(!physical)return 11;
  n=0;families(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> qprops(n);families(physical,&n,qprops.data());
  if(n<3 || qprops[0].queueCount<4 || qprops[2].queueCount<8)return 12;
  float priority[8]={1,1,1,1,1,1,1,1};VkDeviceQueueCreateInfo q[3]{};
  for(auto& x:q){x.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;x.pQueuePriorities=priority;}
  q[0].queueFamilyIndex=0;q[0].queueCount=1;q[1].queueFamilyIndex=2;q[1].queueCount=8;q[2].queueFamilyIndex=1;q[2].queueCount=2;
  const char* devExts[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=3;dc.pQueueCreateInfos=q;dc.enabledExtensionCount=1;dc.ppEnabledExtensionNames=devExts;
  VkDevice device{};vr=createDevice(physical,&dc,nullptr,&device);printf("vkCreateDevice exact full-family request=%d\n",vr);
  if(vr!=VK_SUCCESS)return 13;
  // Streamline resources must be retired while the device is still alive.
  auto end=shutdown();printf("slShutdown before vkDestroyDevice=%d\n",int(end));
  destroyDevice(device,nullptr);destroyInstance(instance,nullptr);
  return end==sl::Result::eOk?0:14;
 }
 auto end=shutdown();printf("slShutdown result=%d\n",int(end));return end==sl::Result::eOk?0:8;
}
