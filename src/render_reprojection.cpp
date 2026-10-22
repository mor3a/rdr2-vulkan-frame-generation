#include "render_reprojection.h"
#include "motion_reprojection.h"
#include "vkhooks.h"
#include "runtime_errors.h"
#include "log.h"
#include "config.h"
#include <windows.h>
#include <mutex>
#include <unordered_set>
#include <cstring>
#include <cstdio>
namespace fgvk { namespace {
constexpr unsigned GridX=24,GridY=14,Count=GridX*GridY;
#define REPROJECTION_FUNCTIONS(X) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences) X(QueueSubmit) X(CmdPipelineBarrier) X(CmdCopyImageToBuffer)
struct Functions {
#define FIELD(n) PFN_vk##n n{};
REPROJECTION_FUNCTIONS(FIELD)
#undef FIELD
} vk;
struct Slot { VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkFence fence{};uint32_t token=0,w=0,h=0;bool submitted=false,failed=false,cancelled=false;std::unordered_set<VkCommandBuffer> commands;sl::Constants constants{}; };
struct Pose {double p[3]{},basis[3][3]{};bool valid=false;};
ReprojectionMetrics metrics;
Pose renderPose;VkDevice owner{};uint32_t lastResolvedToken=0;double previousProjectionY=0;
Pose pendingPose;uint32_t pendingToken=0;double pendingProjectionY=0;
Slot slots[6];std::mutex mutex;bool initialized=false;uint32_t consumedToken=0;VkPhysicalDeviceMemoryProperties memory{};
bool Init(){
 if(owner!=gDevice){for(auto& slot:slots)slot=Slot{};renderPose=Pose{};pendingPose=Pose{};pendingToken=0;initialized=false;owner=gDevice;consumedToken=0;lastResolvedToken=0;previousProjectionY=0;}
 if(initialized)return true;
#define LOAD(n) vk.n=(PFN_vk##n)DeviceFn("vk" #n);if(!vk.n)return false;
REPROJECTION_FUNCTIONS(LOAD)
#undef LOAD
 auto getMemory=(PFN_vkGetPhysicalDeviceMemoryProperties)LoaderFn("vkGetPhysicalDeviceMemoryProperties");if(!getMemory)return false;getMemory(gPhysicalDevice,&memory);initialized=true;return true;
}
bool Allocate(Slot& s){
 if(s.mapped&&s.fence)return true;
 VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=Count*8;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 if(vk.CreateBuffer(gDevice,&ci,nullptr,&s.buffer)!=VK_SUCCESS)return false;
 VkMemoryRequirements req{};vk.GetBufferMemoryRequirements(gDevice,s.buffer,&req);unsigned type=UINT32_MAX;
 const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 for(unsigned i=0;i<memory.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
 if(type==UINT32_MAX)return false;
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
 if(vk.AllocateMemory(gDevice,&ai,nullptr,&s.memory)!=VK_SUCCESS || vk.BindBufferMemory(gDevice,s.buffer,s.memory,0)!=VK_SUCCESS || vk.MapMemory(gDevice,s.memory,0,VK_WHOLE_SIZE,0,&s.mapped)!=VK_SUCCESS)return false;
 VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};return vk.CreateFence(gDevice,&fi,nullptr,&s.fence)==VK_SUCCESS;
}
float Half(uint16_t h){uint32_t sign=uint32_t(h&0x8000)<<16,exponent=(h>>10)&31,mantissa=h&1023,bits;
 if(exponent==0){if(!mantissa)bits=sign;else{int e=-14;while(!(mantissa&1024)){mantissa<<=1;--e;}bits=sign|uint32_t(e+127)<<23|(mantissa&1023)<<13;}}
 else if(exponent==31)bits=sign|0x7f800000|mantissa<<13;else bits=sign|(exponent+112)<<23|mantissa<<13;
 float f;memcpy(&f,&bits,4);return f;
}
bool Inverse(const double source[4][4],sl::float4x4& out){double a[4][8]{};for(unsigned i=0;i<4;++i){for(unsigned j=0;j<4;++j)a[i][j]=source[i][j];a[i][i+4]=1.;}
 for(unsigned c=0;c<4;++c){unsigned pivot=c;for(unsigned r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;if(std::abs(a[pivot][c])<1e-12)return false;if(pivot!=c)for(unsigned j=0;j<8;++j)std::swap(a[pivot][j],a[c][j]);double div=a[c][c];for(unsigned j=0;j<8;++j)a[c][j]/=div;for(unsigned r=0;r<4;++r)if(r!=c){double f=a[r][c];for(unsigned j=0;j<8;++j)a[r][j]-=f*a[c][j];}}
 for(unsigned i=0;i<4;++i)out.setRow(i,{float(a[i][4]),float(a[i][5]),float(a[i][6]),float(a[i][7])});return true;
}
double SampledCameraError(const std::vector<motion::Sample>& samples,const sl::Constants& c,unsigned w,unsigned h){
 std::vector<double> errors;errors.reserve(samples.size());for(const auto& s:samples){const double v[]={s.x,s.y,s.depth,1.};double q[4]{};for(unsigned i=0;i<4;++i){const auto& row=c.clipToPrevClip[i];const float* values=&row.x;for(unsigned j=0;j<4;++j)q[j]+=v[i]*values[j];}if(std::abs(q[3])>1e-9)errors.push_back(std::hypot((q[0]/q[3]-s.x-s.mx)*w*.5,(q[1]/q[3]-s.y-s.my)*h*.5));}
 return errors.empty()?0.:motion::Median(errors);
}
void ApplyRenderPose(sl::Constants& c,const motion::Fit& fit){
 if(!renderPose.valid || c.reset==sl::Boolean::eTrue){const sl::float3 axes[]={c.cameraRight,c.cameraUp,c.cameraFwd};for(unsigned i=0;i<3;++i){renderPose.p[i]=(&c.cameraPos.x)[i];for(unsigned j=0;j<3;++j)renderPose.basis[i][j]=(&axes[j].x)[i];}renderPose.valid=true;return;}
 const double b=c.cameraViewToClip[3].z/c.cameraViewToClip[2].w;
 const auto& rotation=fit.rotation;double translation[3]{};
 for(unsigned i=0;i<3;++i)translation[i]=b*fit.translationCoefficient[i];
 Pose next{};for(unsigned i=0;i<3;++i){next.p[i]=renderPose.p[i];for(unsigned j=0;j<3;++j){next.p[i]+=renderPose.basis[i][j]*translation[j];for(unsigned z=0;z<3;++z)next.basis[i][j]+=renderPose.basis[i][z]*rotation[z][j];}}
 // Remove half-float measurement noise from the integrated orientation.
 auto norm=[&](unsigned column){double length=0;for(unsigned i=0;i<3;++i)length+=next.basis[i][column]*next.basis[i][column];length=std::sqrt(length);if(length>1e-8)for(unsigned i=0;i<3;++i)next.basis[i][column]/=length;};
 norm(0);double dot=0;for(unsigned i=0;i<3;++i)dot+=next.basis[i][0]*next.basis[i][1];for(unsigned i=0;i<3;++i)next.basis[i][1]-=dot*next.basis[i][0];norm(1);dot=0;for(unsigned i=0;i<3;++i)dot+=next.basis[i][0]*next.basis[i][2];for(unsigned i=0;i<3;++i)next.basis[i][2]-=dot*next.basis[i][0];dot=0;for(unsigned i=0;i<3;++i)dot+=next.basis[i][1]*next.basis[i][2];for(unsigned i=0;i<3;++i)next.basis[i][2]-=dot*next.basis[i][1];norm(2);
 next.valid=true;renderPose=next;c.cameraPos={float(next.p[0]),float(next.p[1]),float(next.p[2])};c.cameraRight={float(next.basis[0][0]),float(next.basis[1][0]),float(next.basis[2][0])};c.cameraUp={float(next.basis[0][1]),float(next.basis[1][1]),float(next.basis[2][1])};c.cameraFwd={float(next.basis[0][2]),float(next.basis[1][2]),float(next.basis[2][2])};
}
void Report(const char* detail,int native=0){RuntimeErrors::ReportError(RuntimeErrors::Code::ConstantsFailed,detail,native);static unsigned count=0;if(++count<=5 || count%300==0)Log("FG render reprojection unavailable: %s native=%d",detail,native);}
}
bool RecordRenderReprojection(VkCommandBuffer cmd,uint32_t token,VkImage depth,VkFormat depthFormat,VkImage motion,VkFormat motionFormat,uint32_t w,uint32_t h,VkImageLayout dl,VkImageLayout ml,const sl::Constants& c,uint32_t depthX,uint32_t depthY,uint32_t motionX,uint32_t motionY){
 std::lock_guard<std::mutex> lock(mutex);
 if((depthFormat!=VK_FORMAT_D32_SFLOAT_S8_UINT && depthFormat!=VK_FORMAT_D32_SFLOAT)||motionFormat!=VK_FORMAT_R16G16_SFLOAT||!w||!h){Report("Unsupported depth/motion format for render-frame reprojection");return false;}
 if(!Init()){Report("Required Vulkan readback functions are missing");return false;}
 Slot* target=nullptr;for(auto& s:slots){
  if(s.token && (s.token<=consumedToken || s.cancelled) && s.submitted && vk.GetFenceStatus(gDevice,s.fence)==VK_SUCCESS){vk.ResetFences(gDevice,1,&s.fence);s.token=0;s.submitted=false;s.commands.clear();}
  if(!target && !s.token)target=&s;
 }if(!target){Report("Render-frame reprojection slots are still in use");return false;}
 auto& s=*target;if(!Allocate(s)){Report("Unable to allocate coherent camera readback memory");return false;}
 s.token=token;s.w=w;s.h=h;s.constants=c;s.submitted=s.failed=s.cancelled=false;s.commands.clear();s.commands.insert(cmd);
 VkImageMemoryBarrier b[2]{};const VkImage images[]={depth,motion};const VkImageLayout layouts[]={dl,ml};
 for(unsigned i=0;i<2;++i){auto& t=b[i];t.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;t.image=images[i];t.oldLayout=layouts[i];t.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;t.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;t.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;t.srcQueueFamilyIndex=t.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;t.subresourceRange={i?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};}
 if(depthFormat==VK_FORMAT_D32_SFLOAT_S8_UINT)b[0].subresourceRange.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;
 vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b);
 VkBufferImageCopy regions[Count]{};
 for(unsigned image=0;image<2;++image){for(unsigned y=0;y<GridY;++y)for(unsigned x=0;x<GridX;++x){unsigned i=y*GridX+x;auto& r=regions[i];r.bufferOffset=(image*Count+i)*4;r.imageSubresource={image?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};r.imageOffset={int((x+.5)*w/GridX)+int(image?motionX:depthX),int((y+.5)*h/GridY)+int(image?motionY:depthY),0};r.imageExtent={1,1,1};}vk.CmdCopyImageToBuffer(cmd,images[image],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.buffer,Count,regions);}
 for(unsigned i=0;i<2;++i){auto& t=b[i];t.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;t.newLayout=layouts[i];t.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;t.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
 VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.buffer=s.buffer;host.size=VK_WHOLE_SIZE;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vk.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,2,b);return true;
}
void RenderReprojectionExecuted(VkCommandBuffer primary,uint32_t n,const VkCommandBuffer* secondary){std::lock_guard<std::mutex> lock(mutex);for(auto& s:slots)if(s.token&&!s.submitted)for(unsigned i=0;i<n;++i)if(s.commands.count(secondary[i]))s.commands.insert(primary);}
void RenderReprojectionForget(VkCommandBuffer cmd){std::lock_guard<std::mutex> lock(mutex);for(auto& s:slots)if(s.token&&!s.submitted){s.commands.erase(cmd);if(s.commands.empty())s.token=0;}}
void RenderReprojectionSubmitted(VkQueue queue,uint32_t n,const VkCommandBuffer* commands){static thread_local bool inside=false;if(inside)return;inside=true;
 {std::lock_guard<std::mutex> lock(mutex);if(initialized)for(auto& s:slots)if(s.token&&!s.submitted&&!s.failed)for(unsigned i=0;i<n;++i)if(s.commands.count(commands[i])){VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};auto r=SubmitInternalReadbackFence(queue,s.fence);s.submitted=r==VK_SUCCESS;s.failed=!s.submitted;if(s.failed)Report("Camera readback completion fence submission failed",r);break;}}
 inside=false;
}
bool ResolveRenderReprojection(uint32_t token,sl::Constants& c,bool auditOnly){std::lock_guard<std::mutex> lock(mutex);Slot* found=nullptr;for(auto& s:slots)if(s.token==token){found=&s;break;}
 consumedToken=std::max(consumedToken,token);
 if(!found || !found->submitted){Report("Presented frame has no submitted camera readback");return false;}auto& s=*found;
 LARGE_INTEGER begin{},gpuDone{},end{},frequency{};QueryPerformanceCounter(&begin);auto status=vk.GetFenceStatus(gDevice,s.fence);if(status==VK_NOT_READY)status=vk.WaitForFences(gDevice,1,&s.fence,VK_TRUE,250000000ull);
 if(status!=VK_SUCCESS){Report("Camera readback GPU completion failed",status);return false;}
 QueryPerformanceCounter(&gpuDone);
 if(!auditOnly)c=s.constants;
 if(!auditOnly && (!renderPose.valid || (lastResolvedToken && token!=lastResolvedToken+1)))c.reset=sl::Boolean::eTrue;
 const auto* depths=static_cast<const float*>(s.mapped);const auto* vectors=reinterpret_cast<const uint16_t*>(depths+Count);std::vector<motion::Sample> samples;samples.reserve(Count);
 for(unsigned y=0;y<GridY;++y)for(unsigned x=0;x<GridX;++x){unsigned i=y*GridX+x;double d=depths[i],mx=Half(vectors[2*i]),my=Half(vectors[2*i+1]);
  if(!std::isfinite(d)||!std::isfinite(mx)||!std::isfinite(my)||d<0. || d>1. ||std::abs(mx*c.mvecScale.x)>4.||std::abs(my*c.mvecScale.y)>4.)continue;
  // Positions exactly match the texels copied above, and NGX-derived scales
  // already convert the dense field to normalized current-to-previous motion.
  double px=int((x+.5)*s.w/GridX)+.5,py=int((y+.5)*s.h/GridY)+.5;
  if(Cfg().cameraSource==CameraSource::Derived && px/s.w>.32 && px/s.w<.68 && py/s.h>.38)continue; // Experimental foreground exclusion, not a character stencil.
  samples.push_back({px/s.w*2.-1.,1.-py/s.h*2.,d,mx*c.mvecScale.x*2.,-my*c.mvecScale.y*2.});
 }
 const double sampledError=SampledCameraError(samples,c,s.w,s.h);
 // A small residual is evidence of agreement, not proof that ScriptHook's
 // projection is the renderer's exact projection. Audit uses the constants
 // resolved for this Present; Derived starts from nominal neutral metadata.
 const bool scriptUsable=samples.size()>=32 && sampledError<=8.0;
 const double a=c.cameraViewToClip[2].z/c.cameraViewToClip[2].w;
 const double priorFocal=c.reset==sl::Boolean::eTrue?0.:previousProjectionY;
 auto fit=motion::Reconstruct(samples,s.w,s.h,c.cameraViewToClip[0].x,c.cameraViewToClip[1].y,a,priorFocal);
 // Rejected fits are returned to the caller, which retains finite neutral
 // metadata for the same token. Do not replay an old fitted camera delta.
 bool fellBack=false;
 if(auditOnly){
  vk.ResetFences(gDevice,1,&s.fence);s.token=0;s.submitted=false;s.commands.clear();
  QueryPerformanceCounter(&end);QueryPerformanceFrequency(&frequency);
  metrics={token,sampledError,fit.medianPixels,(gpuDone.QuadPart-begin.QuadPart)*1000./frequency.QuadPart,(end.QuadPart-gpuDone.QuadPart)*1000./frequency.QuadPart,fit.valid};
  if(DebugLoggingEnabled())Log("Camera audit: token=%u sampledCameraError=%.3fpx reconstructedError=%.3fpx fitValid=%d gpuWait=%.3fms cpuFit=%.3fms",token,metrics.sampledError,metrics.reconstructedError,metrics.valid,metrics.gpuWaitMs,metrics.cpuFitMs);
  return true;
 }
 bool useScript=false;
 if(Cfg().cameraSource==CameraSource::Sampled && !fit.valid && scriptUsable)useScript=true;
 if(useScript)fit.valid=false; // script constants are sent unchanged
 if(fit.valid){
  c.cameraViewToClip[0].x=float(fit.projectionX);c.cameraViewToClip[1].y=float(fit.projectionY);
  c.cameraFOV=float(2.*std::atan(1./fit.projectionY));
  double projection[4][4]{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)projection[i][j]=(&c.cameraViewToClip[i].x)[j];
  fit.valid=Inverse(projection,c.clipToCameraView);if(!fit.valid)fit.failure="projection inverse failed";
 }
 // A reset clears temporal history; it must never turn a failed fit into a
 // successful input frame. The old bypass accepted invalid depth/motion data.
 if(fit.valid && c.reset==sl::Boolean::eTrue){for(unsigned row=0;row<4;++row){sl::float4 v{0,0,0,0};(&v.x)[row]=1.f;c.clipToPrevClip.setRow(row,v);c.prevClipToClip.setRow(row,v);}}
 else if(fit.valid){for(unsigned row=0;row<4;++row)c.clipToPrevClip.setRow(row,{float(fit.matrix[row][0]),float(fit.matrix[row][1]),float(fit.matrix[row][2]),float(fit.matrix[row][3])});fit.valid=Inverse(fit.matrix,c.prevClipToClip);if(!fit.valid)fit.failure="clip reprojection inverse failed";}
 // Preparing constants is not a successful presentation. Stage the new pose
 // and advance the immutable history only after vkQueuePresentKHR succeeds.
 pendingToken=0;
 if(useScript){Pose p{};const sl::float3 axes[]={c.cameraRight,c.cameraUp,c.cameraFwd};for(unsigned i=0;i<3;++i){p.p[i]=(&c.cameraPos.x)[i];for(unsigned j=0;j<3;++j)p.basis[i][j]=(&axes[j].x)[i];}p.valid=true;pendingPose=p;pendingToken=token;pendingProjectionY=c.cameraViewToClip[1].y;}
 else if(fit.valid){const Pose previous=renderPose;ApplyRenderPose(c,fit);pendingPose=renderPose;renderPose=previous;pendingToken=token;pendingProjectionY=fit.projectionY;}
 vk.ResetFences(gDevice,1,&s.fence);s.token=0;s.submitted=false;s.commands.clear();QueryPerformanceCounter(&end);QueryPerformanceFrequency(&frequency);
 metrics={token,sampledError,fit.medianPixels,(gpuDone.QuadPart-begin.QuadPart)*1000./frequency.QuadPart,(end.QuadPart-gpuDone.QuadPart)*1000./frequency.QuadPart,useScript||fit.valid};
 if(DebugLoggingEnabled())Log("FG render reprojection: token=%u samples=%zu sampledCameraError=%.3fpx reconstructedError=%.3fpx inliers=%.1f%% rigidity=%.6f renderFOV=%.3f reset=%u accepted=%d fallback=%d source=%s gpuWait=%.3fms cpuFit=%.3fms",token,samples.size(),sampledError,fit.medianPixels,fit.inliers*100.,fit.rigidity,c.cameraFOV*57.2957795,(unsigned)c.reset,useScript||fit.valid,(int)fellBack,useScript?"script":(fellBack?"fallback":"fit"),(gpuDone.QuadPart-begin.QuadPart)*1000./frequency.QuadPart,(end.QuadPart-gpuDone.QuadPart)*1000./frequency.QuadPart);
 if(!useScript && !fit.valid){char detail[256];snprintf(detail,sizeof(detail),"%s: token=%u determinant=%.9g error=%.4fpx inliers=%.3f rigidity=%.6f",fit.failure,token,fit.determinant,fit.medianPixels,fit.inliers,fit.rigidity);Report(detail);return false;}
 return true;
}
ReprojectionMetrics ReadReprojectionMetrics(){std::lock_guard<std::mutex> lock(mutex);return metrics;}
void CommitRenderReprojection(uint32_t token,bool presented){
 std::lock_guard<std::mutex> lock(mutex);
 if(pendingToken!=token)return;
 if(presented){renderPose=pendingPose;lastResolvedToken=token;previousProjectionY=pendingProjectionY;}
 else {renderPose.valid=false;previousProjectionY=0;}
 pendingToken=0;
}
void CancelRenderReprojection(uint32_t token){
 std::lock_guard<std::mutex> lock(mutex);
 for(auto& slot:slots)if(slot.token==token)slot.cancelled=true;
}
}
