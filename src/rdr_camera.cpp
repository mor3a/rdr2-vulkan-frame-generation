#include "rdr_camera.h"
#include "log.h"
#include "runtime_errors.h"
#include "neutral_camera.h"
#include "config.h"
#include <cmath>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <array>
namespace fgvk {
static std::atomic<bool> cameraErrors[RuntimeErrors::Capacity]{};
static void CameraError(RuntimeErrors::Code code,const char* detail){RuntimeErrors::ReportError(code,detail);if(!cameraErrors[(unsigned)code].exchange(true))Log("%s %s",RuntimeErrors::CodeId(code),detail);}
static void CameraRecovered(RuntimeErrors::Code code){RuntimeErrors::ClearError(code);if(cameraErrors[(unsigned)code].exchange(false))Log("%s recovered",RuntimeErrors::CodeId(code));}
struct Vec {float x=0,y=0,z=0;};
struct Snapshot {Vec position,rotation;float fov=0;ULONGLONG time=0;uint64_t timeUs=0;bool valid=false;};
static Snapshot camera;static std::array<Snapshot,64> poseRing{};static size_t poseNext{};static std::mutex cameraMutex;
bool RegisterRdrCamera(HMODULE){return false;}
uint64_t CameraNowUs(){static const double factor=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return 1000000./f.QuadPart;}();LARGE_INTEGER q{};QueryPerformanceCounter(&q);return uint64_t(q.QuadPart*factor);}
bool ReadCameraPoseAt(uint64_t target,uint64_t previous,uint32_t maxAgeMs,CameraPose& out){
 std::lock_guard<std::mutex> guard(cameraMutex);const Snapshot* best=nullptr;
 for(const auto& pose:poseRing)if(pose.valid && CameraTimeEligible(pose.timeUs,target,previous,maxAgeMs) && (!best || pose.timeUs>best->timeUs))best=&pose;
 out={};if(!best)return false;
 out.position[0]=best->position.x;out.position[1]=best->position.y;out.position[2]=best->position.z;
 out.rotation[0]=best->rotation.x;out.rotation[1]=best->rotation.y;out.rotation[2]=best->rotation.z;
 out.fov=best->fov;out.timeUs=best->timeUs;out.valid=true;return true;
}
struct Matrix {float m[4][4]{};};
static Matrix Identity(){Matrix r;for(int i=0;i<4;i++)r.m[i][i]=1.f;return r;}
static Matrix Multiply(const Matrix& a,const Matrix& b){Matrix r;for(int i=0;i<4;i++)for(int j=0;j<4;j++)for(int k=0;k<4;k++)r.m[i][j]+=a.m[i][k]*b.m[k][j];return r;}
static bool Inverse(const Matrix& a,Matrix& inverse){
  double v[4][8]{};for(int i=0;i<4;i++){for(int j=0;j<4;j++)v[i][j]=a.m[i][j];v[i][i+4]=1;}
  for(int c=0;c<4;c++){
    int pivot=c;for(int i=c+1;i<4;i++)if(std::abs(v[i][c])>std::abs(v[pivot][c]))pivot=i;
    if(std::abs(v[pivot][c])<1e-12)return false;
    if(pivot!=c)for(int j=0;j<8;j++)std::swap(v[pivot][j],v[c][j]);
    double scale=v[c][c];for(int j=0;j<8;j++)v[c][j]/=scale;
    for(int i=0;i<4;i++)if(i!=c){double factor=v[i][c];for(int j=0;j<8;j++)v[i][j]-=factor*v[c][j];}
  }
  for(int i=0;i<4;i++)for(int j=0;j<4;j++){inverse.m[i][j]=(float)v[i][j+4];if(!std::isfinite(inverse.m[i][j]))return false;}return true;
}
static sl::float4x4 ToSl(const Matrix& m){sl::float4x4 r{};for(int i=0;i<4;i++)r.setRow(i,{m.m[i][0],m.m[i][1],m.m[i][2],m.m[i][3]});return r;}
static float Dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static Vec AddScaled(Vec a,float sa,Vec b,float sb){return {a.x*sa+b.x*sb,a.y*sa+b.y*sb,a.z*sa+b.z*sb};}
static Matrix acceptedCamera{}, pendingCamera{};
static Snapshot acceptedPose{}, pendingPose{};
static bool haveAcceptedCamera=false,havePendingCamera=false;
static std::atomic<bool> historyInterrupted{false};
void InvalidateRdrCameraHistory(){historyInterrupted.store(true,std::memory_order_release);}
void CommitRdrCamera(bool accepted){
  if(accepted && havePendingCamera){acceptedCamera=pendingCamera;acceptedPose=pendingPose;haveAcceptedCamera=true;}
  else if(!accepted)InvalidateRdrCameraHistory();
  havePendingCamera=false;
}
bool ApplyNeutralCamera(sl::Constants& c,float aspect,bool invertedDepth,bool reset,bool historyProtection){
  havePendingCamera=false;
  const bool interrupted=historyInterrupted.exchange(false,std::memory_order_acq_rel);
  if(!BuildNeutralCamera(c,aspect,invertedDepth,reset || !haveAcceptedCamera || (historyProtection && interrupted)))return false;
  // Keep the existing accepted-input/reset bookkeeping without sampling a pose.
  pendingCamera=Identity();pendingPose=Snapshot{};havePendingCamera=true;
  static bool noted=false;
  if(!noted){noted=true;Log("DLSS-G camera source=neutral; ScriptHook camera unused; nominal FOV=60 near=0.05 far=10000; real motion comes from NGX motion vectors");}
  return true;
}
bool ApplyRdrCamera(sl::Constants& c,float aspect,bool invertedDepth,bool reset,bool historyProtection,bool requireFreshPose){
  havePendingCamera=false;

  Snapshot s;{std::lock_guard<std::mutex> lock(cameraMutex);s=camera;}
  if(!s.valid||(requireFreshPose && GetTickCount64()-s.time>250)||!std::isfinite(s.fov)||s.fov<=1.f||s.fov>=179.f||!std::isfinite(aspect)||aspect<=0){char detail[256];snprintf(detail,sizeof(detail),"Camera valid=%d ageMs=%llu FOV=%.3f aspect=%.4f",s.valid,(unsigned long long)(s.time?GetTickCount64()-s.time:0),s.fov,aspect);CameraError(RuntimeErrors::Code::CameraStale,detail);return false;}
  // Use the captured pose, not a wall-clock extrapolation unrelated to GPU frame time.
  Vec posePos=s.position,poseRot=s.rotation;
  constexpr float rad=0.017453292519943295f;
  float pitch=poseRot.x*rad,roll=poseRot.y*rad,yaw=poseRot.z*rad;
  float cp=std::cos(pitch),sp=std::sin(pitch),cy=std::cos(yaw),sy=std::sin(yaw);
  Vec forward{-sy*cp,cy*cp,sp};Vec baseRight{cy,sy,0};Vec baseUp{sy*sp,-cy*sp,cp};
  Vec right=AddScaled(baseRight,std::cos(roll),baseUp,-std::sin(roll));
  Vec up=AddScaled(baseRight,std::sin(roll),baseUp,std::cos(roll));
  Matrix view=Identity();
  view.m[0][0]=right.x;view.m[0][1]=up.x;view.m[0][2]=forward.x;
  view.m[1][0]=right.y;view.m[1][1]=up.y;view.m[1][2]=forward.y;
  view.m[2][0]=right.z;view.m[2][1]=up.z;view.m[2][2]=forward.z;
  view.m[3][0]=-Dot(posePos,right);view.m[3][1]=-Dot(posePos,up);view.m[3][2]=-Dot(posePos,forward);
  // Near/farPlane are explicit fallbacks until engine projection extraction is available.
  // FOV and the view transform come from the actual final rendered camera.
  float nearPlane=0.05f,farPlane=10000.f,fov=s.fov*rad,f=1.f/std::tan(fov*0.5f);
  Matrix projection{};projection.m[0][0]=f/aspect;projection.m[1][1]=f;projection.m[2][3]=1;
  projection.m[2][2]=invertedDepth?nearPlane/(nearPlane-farPlane):farPlane/(farPlane-nearPlane);
  projection.m[3][2]=invertedDepth?nearPlane*farPlane/(farPlane-nearPlane):-nearPlane*farPlane/(farPlane-nearPlane);
  Matrix invProjection{},current=Multiply(view,projection),invCurrent{};
  if(!Inverse(projection,invProjection)||!Inverse(current,invCurrent)){CameraError(RuntimeErrors::Code::CameraStale,"Camera projection or view-projection matrix cannot be inverted.");return false;}
  CameraRecovered(RuntimeErrors::Code::CameraStale);
  Matrix previous=acceptedCamera;Snapshot last=acceptedPose;bool hadPrevious=haveAcceptedCamera;
  const bool interrupted=historyInterrupted.exchange(false,std::memory_order_acq_rel);
  reset=reset || (historyProtection && interrupted);
  if(historyProtection && interrupted){static unsigned count=0;++count;if(count==1 || (count&(count-1))==0)Log("FG history protection: reset after rejected/dropped input; occurrences=%u",count);}
  Vec delta{s.position.x-last.position.x,s.position.y-last.position.y,s.position.z-last.position.z};
  bool cut=reset||!hadPrevious||Dot(delta,delta)>2500.f||std::abs(s.fov-last.fov)>20.f;
  Matrix invPrevious{};if(cut||!Inverse(previous,invPrevious)){previous=current;invPrevious=invCurrent;}
  c.cameraViewToClip=ToSl(projection);c.clipToCameraView=ToSl(invProjection);
  c.clipToLensClip=ToSl(Identity());
  // Streamline requires the current-to-previous clip transform even when the
  // motion-vector buffer includes camera motion. Reset frames use identity.
  c.clipToPrevClip=ToSl(Multiply(invCurrent,previous));
  c.prevClipToClip=ToSl(Multiply(invPrevious,current));
  c.cameraPos={posePos.x,posePos.y,posePos.z};c.cameraRight={right.x,right.y,right.z};
  c.cameraUp={up.x,up.y,up.z};c.cameraFwd={forward.x,forward.y,forward.z};
  c.cameraNear=nearPlane;c.cameraFar=farPlane;c.cameraFOV=fov;c.cameraAspectRatio=aspect;
  c.reset=cut?sl::Boolean::eTrue:sl::Boolean::eFalse;
  pendingCamera=current;pendingPose=s;havePendingCamera=true;return true;
}
}

