#include "camera_frame.h"
#include "rdr_camera.h"
#include "neutral_camera.h"
#include "config.h"
#include "log.h"
#include <array>
#include <mutex>
#include <cmath>

namespace fgvk { namespace {
using Matrix=sl::float4x4;
struct Entry {uint64_t token{};Matrix worldClip{};sl::Constants camera{};FrameCameraInfo info{};bool resolved{};};
std::array<Entry,32> frames;
std::mutex mutex;
Matrix acceptedClip{};uint64_t acceptedToken{},acceptedPoseTime{};
CameraSource acceptedSource=CameraSource::Neutral;FrameCameraInfo latest;
Matrix Identity(){Matrix m{};for(unsigned i=0;i<4;++i){sl::float4 row{};(&row.x)[i]=1;m.setRow(i,row);}return m;}
Matrix Multiply(const Matrix& a,const Matrix& b){Matrix result{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)(&result[i].x)[j]+=(&a[i].x)[k]*(&b[k].x)[j];return result;}
bool Inverse(const Matrix& source,Matrix& out){
 double a[4][8]{};for(unsigned i=0;i<4;++i){for(unsigned j=0;j<4;++j)a[i][j]=(&source[i].x)[j];a[i][i+4]=1;}
 for(unsigned column=0;column<4;++column){unsigned pivot=column;for(unsigned row=column+1;row<4;++row)if(std::abs(a[row][column])>std::abs(a[pivot][column]))pivot=row;
  if(std::abs(a[pivot][column])<1e-12)return false;if(pivot!=column)for(unsigned j=0;j<8;++j)std::swap(a[pivot][j],a[column][j]);
  const double scale=a[column][column];for(auto& value:a[column])value/=scale;
  for(unsigned row=0;row<4;++row)if(row!=column){const double scale=a[row][column];for(unsigned j=0;j<8;++j)a[row][j]-=scale*a[column][j];}}
 for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)(&out[i].x)[j]=float(a[i][j+4]);return true;
}
bool Sampled(const CameraPose& pose,float aspect,bool inverted,sl::Constants& c,Matrix& worldClip){
 constexpr float rad=.017453292519943295f;
 if(!pose.valid || !std::isfinite(pose.fov) || pose.fov<=1 || pose.fov>=175)return false;
 const float pitch=pose.rotation[0]*rad,roll=pose.rotation[1]*rad,yaw=pose.rotation[2]*rad;
 const float cp=std::cos(pitch),sp=std::sin(pitch),cy=std::cos(yaw),sy=std::sin(yaw),cr=std::cos(roll),sr=std::sin(roll);
 c.cameraFwd={-sy*cp,cy*cp,sp};c.cameraRight={cy*cr-sy*sp*sr,sy*cr+cy*sp*sr,-cp*sr};
 c.cameraUp={cy*sr+sy*sp*cr,sy*sr-cy*sp*cr,cp*cr};c.cameraPos={pose.position[0],pose.position[1],pose.position[2]};
 Matrix view=Identity();const sl::float3 axes[]={c.cameraRight,c.cameraUp,c.cameraFwd};
 for(unsigned column=0;column<3;++column){float dot=0;for(unsigned row=0;row<3;++row){(&view[row].x)[column]=(&axes[column].x)[row];dot+=pose.position[row]*(&axes[column].x)[row];}(&view[3].x)[column]=-dot;}
 const float fov=pose.fov*rad,f=1/std::tan(fov*.5f),nearPlane=.05f,farPlane=10000.f;
 Matrix projection{};projection[0].x=f/aspect;projection[1].y=f;projection[2].w=1;
 projection[2].z=inverted?nearPlane/(nearPlane-farPlane):farPlane/(farPlane-nearPlane);projection[3].z=inverted?nearPlane*farPlane/(farPlane-nearPlane):-nearPlane*farPlane/(farPlane-nearPlane);
 if(!Inverse(projection,c.clipToCameraView))return false;
 c.cameraViewToClip=projection;c.cameraFOV=fov;c.cameraAspectRatio=aspect;c.cameraNear=nearPlane;c.cameraFar=farPlane;
 worldClip=Multiply(view,projection);return true;
}
void CopyCamera(const sl::Constants& source,sl::Constants& out){
 out.cameraViewToClip=source.cameraViewToClip;out.clipToCameraView=source.clipToCameraView;out.clipToLensClip=source.clipToLensClip;
 out.cameraPos=source.cameraPos;out.cameraFwd=source.cameraFwd;out.cameraRight=source.cameraRight;out.cameraUp=source.cameraUp;
 out.cameraNear=source.cameraNear;out.cameraFar=source.cameraFar;out.cameraFOV=source.cameraFOV;out.cameraAspectRatio=source.cameraAspectRatio;
 out.cameraPinholeOffset=source.cameraPinholeOffset;
}
}
bool PrepareFrameCamera(uint64_t token,sl::Constants& c,float aspect,bool inverted,bool reset){
 if(!BuildNeutralCamera(c,aspect,inverted,reset))return false;
 std::lock_guard<std::mutex> guard(mutex);Entry* entry=nullptr;
 for(auto& candidate:frames)if(!candidate.token || candidate.token==token){entry=&candidate;break;}
 if(!entry)return true; // finite neutral metadata; do not disable working FG
 *entry=Entry{};entry->token=token;entry->worldClip=Identity();entry->info={token,CameraNowUs(),0,0,Cfg().cameraSource,Cfg().cameraSource,false};
 if(Cfg().cameraSource==CameraSource::Sampled){CameraPose pose;
  if(ReadCameraPoseAt(entry->info.targetUs,acceptedPoseTime,Cfg().cameraMaxAgeMs,pose) && Sampled(pose,aspect,inverted,c,entry->worldClip)){
   entry->info.poseUs=pose.timeUs;entry->info.ageMs=(entry->info.targetUs-pose.timeUs)/1000.;entry->info.fresh=true;
  }else entry->info.actual=CameraSource::Neutral;
 }
 entry->camera=c;return true;
}
bool ResolveFrameCamera(uint64_t token,sl::Constants& c){
 std::lock_guard<std::mutex> guard(mutex);Entry* entry=nullptr;for(auto& candidate:frames)if(candidate.token==token){entry=&candidate;break;}
 if(!entry)return false;
 CopyCamera(entry->camera,c);
 bool reset=c.reset==sl::Boolean::eTrue || !acceptedToken || token<=acceptedToken || (entry->info.requested!=CameraSource::Derived && acceptedSource!=entry->info.actual);
 c.clipToPrevClip=c.prevClipToClip=Identity();
 if(entry->info.actual==CameraSource::Sampled && !reset){Matrix currentInverse{},previousInverse{};
  if(Inverse(entry->worldClip,currentInverse) && Inverse(acceptedClip,previousInverse)){c.clipToPrevClip=Multiply(currentInverse,acceptedClip);c.prevClipToClip=Multiply(previousInverse,entry->worldClip);}else reset=true;}
 c.reset=reset?sl::Boolean::eTrue:sl::Boolean::eFalse;entry->resolved=true;latest=entry->info;
 if(DebugLoggingEnabled())Log("Camera frame: token=%llu targetUs=%llu poseUs=%llu poseAgeMs=%.3f requested=%s actual=%s fresh=%d reset=%d",(unsigned long long)token,(unsigned long long)entry->info.targetUs,(unsigned long long)entry->info.poseUs,entry->info.ageMs,CameraSourceName(entry->info.requested),CameraSourceName(entry->info.actual),entry->info.fresh,reset);
 return true;
}
void SetDerivedFrameResult(uint64_t token,bool accepted){std::lock_guard<std::mutex> guard(mutex);for(auto& entry:frames)if(entry.token==token){entry.info.actual=accepted?CameraSource::Derived:CameraSource::Neutral;latest=entry.info;break;}}
void CommitFrameCamera(uint64_t token,bool presented){
 std::lock_guard<std::mutex> guard(mutex);for(auto& entry:frames)if(entry.token==token){
  if(presented && entry.resolved && token>acceptedToken){acceptedToken=token;acceptedClip=entry.worldClip;acceptedSource=entry.info.actual;if(entry.info.poseUs)acceptedPoseTime=entry.info.poseUs;}
  else if(!presented)acceptedToken=0;
  entry=Entry{};break;
 }
}
void ResetFrameCameraHistory(){std::lock_guard<std::mutex> guard(mutex);acceptedToken=acceptedPoseTime=0;for(auto& entry:frames)entry=Entry{};latest={};}
FrameCameraInfo ReadFrameCameraInfo(){std::lock_guard<std::mutex> guard(mutex);return latest;}
}
