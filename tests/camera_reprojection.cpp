#include "../src/rdr_camera.cpp"
#include <cstdio>
namespace fgvk {void Log(const char*,...) {}}
static bool identity(const sl::float4x4& m){for(int i=0;i<4;++i){const float* p=&m[i].x;for(int j=0;j<4;++j)if(std::fabs(p[j]-(i==j?1.f:0.f))>.001f)return false;}return true;}
int main(){using namespace fgvk;camera.valid=true;camera.time=GetTickCount64();camera.fov=60;
 sl::Constants a{};if(!ApplyRdrCamera(a,16.f/9,true,true)||!identity(a.clipToPrevClip))return 1;
 CommitRdrCamera(true);
 camera.position.x=1;camera.rotation.z=15;camera.time=GetTickCount64();sl::Constants b{};
 if(!ApplyRdrCamera(b,16.f/9,true,false)||identity(b.clipToPrevClip))return 2;
 Matrix f{},inv{};for(int i=0;i<4;++i){memcpy(f.m[i],&b.clipToPrevClip[i].x,16);memcpy(inv.m[i],&b.prevClipToClip[i].x,16);}
 if(!identity(ToSl(Multiply(f,inv))))return 3;
 sl::Constants c{};if(!ApplyRdrCamera(c,16.f/9,true,true)||!identity(c.clipToPrevClip))return 4;
 CommitRdrCamera(false);
 sl::Constants rejected{};if(!ApplyRdrCamera(rejected,16.f/9,true,false) || rejected.reset!=sl::Boolean::eTrue)return 5;
 CommitRdrCamera(true);
 InvalidateRdrCameraHistory();sl::Constants dropped{};
 if(!ApplyRdrCamera(dropped,16.f/9,true,false) || dropped.reset!=sl::Boolean::eTrue)return 6;
 CommitRdrCamera(true);camera.position.x=2;camera.time=GetTickCount64();
 sl::Constants resumed{};if(!ApplyRdrCamera(resumed,16.f/9,true,false) || resumed.reset!=sl::Boolean::eFalse || identity(resumed.clipToPrevClip))return 7;
 puts("Camera motion reprojection / inverse / reset checks: PASS");}
