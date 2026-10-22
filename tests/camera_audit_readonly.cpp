// Run the production audit resolver with in-memory samples and fake fence callbacks.
// No Vulkan device/driver, game, SDK context or injected module is created.
#include "../src/render_reprojection.cpp"
#include "neutral_camera.h"
#include <cassert>
namespace fgvk {
VkInstance gInstance{};VkPhysicalDevice gPhysicalDevice{};VkDevice gDevice{};VkQueue gGraphicsQueue{};uint32_t gGraphicsFamily{};
Config config;
const Config& Cfg(){return config;}
void* DeviceFn(const char*){return nullptr;}
void* LoaderFn(const char*){return nullptr;}
VkResult SubmitInternalReadbackFence(VkQueue,VkFence){return VK_SUCCESS;}
void Log(const char*,...){}
bool DebugLoggingEnabled(){return false;}
}
static VkResult VKAPI_CALL Complete(VkDevice,VkFence){return VK_SUCCESS;}
static VkResult VKAPI_CALL Reset(VkDevice,uint32_t,const VkFence*){return VK_SUCCESS;}
int main(){
 using namespace fgvk;
 vk.GetFenceStatus=Complete;vk.ResetFences=Reset;
 std::array<float,Count*2> samples{};for(unsigned i=0;i<Count;++i)samples[i]=.5f;
 sl::Constants current{},recorded{};assert(BuildNeutralCamera(current,16.f/9,true,false));
 current.mvecScale={1.f/1920,1.f/1080};current.clipToPrevClip[3].x=.125f;
 assert(BuildNeutralCamera(recorded,16.f/9,true,true));recorded.mvecScale=current.mvecScale;
 slots[0].token=1;slots[0].submitted=true;slots[0].mapped=samples.data();slots[0].w=1920;slots[0].h=1080;slots[0].constants=recorded;
 const auto before=current;
 assert(ResolveRenderReprojection(1,current,true));
 assert(std::memcmp(&before,&current,sizeof(current))==0); // audit must not change any camera/reset field
 const auto info=ReadReprojectionMetrics();assert(info.token==1 && info.sampledError>100 && info.gpuWaitMs>=0 && info.cpuFitMs>=0);
 puts("Production camera audit: uses present-resolved camera for residuals; leaves constants/reset byte-identical; fake completed fence only: PASS");
}
