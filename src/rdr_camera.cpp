#include "rdr_camera.h"
#include "log.h"
#include <cmath>
#include <cstring>
#include <mutex>
#include <algorithm>
namespace fgvk {
using NativeInit=void(*)(unsigned long long);
using NativePush=void(*)(unsigned long long);
using NativeCall=unsigned long long*(*)();
using ScriptWait=void(*)(DWORD);
using ScriptRegister=void(*)(HMODULE,void(*)());
static NativeInit init=nullptr;static NativePush push=nullptr;static NativeCall call=nullptr;static ScriptWait wait=nullptr;
struct Vec {float x=0,y=0,z=0;};
struct Snapshot {Vec position,rotation;float fov=0;ULONGLONG time=0;bool valid=false;};
static Snapshot camera;static std::mutex cameraMutex;
// ABI from the RDR2 ScriptHook SDK: vector components occupy separate 8-byte slots.
static Vec Vector(unsigned long long hash,bool arg=false,unsigned long long value=0){
  init(hash);if(arg)push(value);auto result=call();Vec v{};
  if(result){std::memcpy(&v.x,result,4);std::memcpy(&v.y,result+1,4);std::memcpy(&v.z,result+2,4);}return v;
}
static bool Finite(Vec v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
static void CameraScript(){
  bool logged=false;
  while(true){
    Snapshot s;
    s.position=Vector(0x5352E025EC2B416FULL);
    s.rotation=Vector(0x602685BD85DD26CAULL,true,2); // ZXY rotation order
    init(0x04AF77971E508F6AULL);auto result=call();if(result)std::memcpy(&s.fov,result,4);
    s.valid=Finite(s.position)&&Finite(s.rotation)&&std::isfinite(s.fov)&&s.fov>1.f&&s.fov<175.f;
    s.time=GetTickCount64();
    {std::lock_guard<std::mutex> lock(cameraMutex);camera=s;}
    if(s.valid&&!logged){logged=true;Log("RDR2 native camera available: FOV=%.2f position=(%.2f,%.2f,%.2f)",s.fov,s.position.x,s.position.y,s.position.z);}
    wait(0);
  }
}
template<class T>static T Resolve(HMODULE module,const char* plain,const char* decorated){
  FARPROC p=GetProcAddress(module,plain);if(!p)p=GetProcAddress(module,decorated);return reinterpret_cast<T>(p);
}
bool RegisterRdrCamera(HMODULE self){
  HMODULE sdk=GetModuleHandleA("ScriptHookRDR2.dll");if(!sdk){Log("ScriptHook not loaded: native camera required for FG");return false;}
  init=Resolve<NativeInit>(sdk,"nativeInit","?nativeInit@@YAX_K@Z");
  push=Resolve<NativePush>(sdk,"nativePush64","?nativePush64@@YAX_K@Z");
  call=Resolve<NativeCall>(sdk,"nativeCall","?nativeCall@@YAPEA_KXZ");
  wait=Resolve<ScriptWait>(sdk,"scriptWait","?scriptWait@@YAXK@Z");
  auto reg=Resolve<ScriptRegister>(sdk,"scriptRegister","?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");
  if(!init||!push||!call||!wait||!reg){Log("ScriptHook camera API unavailable");return false;}
  reg(self,CameraScript);Log("RDR2 camera fiber registered");return true;
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
bool ApplyRdrCamera(sl::Constants& c,float aspect,bool invertedDepth,bool reset){
  Snapshot s;{std::lock_guard<std::mutex> lock(cameraMutex);s=camera;}
  if(!s.valid||GetTickCount64()-s.time>250||!std::isfinite(aspect)||aspect<=0)return false;
  constexpr float rad=0.017453292519943295f;
  float pitch=s.rotation.x*rad,roll=s.rotation.y*rad,yaw=s.rotation.z*rad;
  float cp=std::cos(pitch),sp=std::sin(pitch),cy=std::cos(yaw),sy=std::sin(yaw);
  Vec forward{-sy*cp,cy*cp,sp};Vec baseRight{cy,sy,0};Vec baseUp{sy*sp,-cy*sp,cp};
  Vec right=AddScaled(baseRight,std::cos(roll),baseUp,-std::sin(roll));
  Vec up=AddScaled(baseRight,std::sin(roll),baseUp,std::cos(roll));
  Matrix view=Identity();
  view.m[0][0]=right.x;view.m[0][1]=up.x;view.m[0][2]=forward.x;
  view.m[1][0]=right.y;view.m[1][1]=up.y;view.m[1][2]=forward.y;
  view.m[2][0]=right.z;view.m[2][1]=up.z;view.m[2][2]=forward.z;
  view.m[3][0]=-Dot(s.position,right);view.m[3][1]=-Dot(s.position,up);view.m[3][2]=-Dot(s.position,forward);
  // Near/farPlane are explicit fallbacks until engine projection extraction is available.
  // FOV and the view transform come from the actual final rendered camera.
  float nearPlane=0.05f,farPlane=10000.f,fov=s.fov*rad,f=1.f/std::tan(fov*0.5f);
  Matrix projection{};projection.m[0][0]=f/aspect;projection.m[1][1]=f;projection.m[2][3]=1;
  projection.m[2][2]=invertedDepth?nearPlane/(nearPlane-farPlane):farPlane/(farPlane-nearPlane);
  projection.m[3][2]=invertedDepth?nearPlane*farPlane/(farPlane-nearPlane):-nearPlane*farPlane/(farPlane-nearPlane);
  Matrix invProjection{},current=Multiply(view,projection),invCurrent{};
  if(!Inverse(projection,invProjection)||!Inverse(current,invCurrent))return false;
  static Matrix previous{};static Snapshot last{};static bool hadPrevious=false;
  Vec delta{s.position.x-last.position.x,s.position.y-last.position.y,s.position.z-last.position.z};
  bool cut=reset||!hadPrevious||Dot(delta,delta)>2500.f||std::abs(s.fov-last.fov)>20.f;
  Matrix invPrevious{};if(cut||!Inverse(previous,invPrevious)){previous=current;invPrevious=invCurrent;}
  c.cameraViewToClip=ToSl(projection);c.clipToCameraView=ToSl(invProjection);
  c.clipToPrevClip=ToSl(Multiply(invCurrent,previous));c.prevClipToClip=ToSl(Multiply(invPrevious,current));
  c.cameraPos={s.position.x,s.position.y,s.position.z};c.cameraRight={right.x,right.y,right.z};
  c.cameraUp={up.x,up.y,up.z};c.cameraFwd={forward.x,forward.y,forward.z};
  c.cameraNear=nearPlane;c.cameraFar=farPlane;c.cameraFOV=fov;c.cameraAspectRatio=aspect;
  c.reset=cut?sl::Boolean::eTrue:sl::Boolean::eFalse;
  previous=current;last=s;hadPrevious=true;return true;
}
}

