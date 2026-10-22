#include "../vendor/ampere-core/dlssg_provider_policy.h"
#include <winver.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>

int wmain(int argc,wchar_t** argv){
 using namespace dlssg_provider_policy;
 // Root VERSIONINFO with a fixed binary value, followed by unrelated bytes.
 std::array<unsigned char,160> resource{};
 constexpr wchar_t key[]=L"VS_VERSION_INFO";
 constexpr size_t offset=(6+sizeof(key)+3)&~size_t(3);
 const uint16_t header[]={uint16_t(resource.size()),sizeof(VS_FIXEDFILEINFO),0};
 std::memcpy(resource.data(),header,sizeof(header));std::memcpy(resource.data()+6,key,sizeof(key));
 VS_FIXEDFILEINFO info{};info.dwSignature=VS_FFI_SIGNATURE;
 info.dwFileVersionMS=(310u<<16)|9;info.dwFileVersionLS=(16u<<16)|42;
 std::memcpy(resource.data()+offset,&info,sizeof(info));VersionTriplet v{};
 assert(DecodeVersionResource(resource.data(),resource.size(),v));
 assert(v.major==310 && v.minor==9 && v.build==16 && IsSupportedVersion(v));
 for(size_t n=0;n<offset+sizeof(info);++n)assert(!DecodeVersionResource(resource.data(),n,v));
 assert(!v.major && !DecodeVersionResource(nullptr,resource.size(),v));
 for(size_t n:{size_t(0),size_t(2),size_t(4),size_t(6),offset}){
  auto broken=resource;broken[n]^=0x80;assert(!DecodeVersionResource(broken.data(),broken.size(),v));
 }
 assert(!ReadLoadedProviderVersion(nullptr,v));
 // Map only the provider's resources: no DllMain, imports, SDK or GPU work.
 assert(argc==2);wchar_t path[32768]{};assert(GetFullPathNameW(argv[1],32768,path,nullptr));
 HMODULE system=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
 assert(system);VersionTriplet file{},mapped{};
 assert(ReadProviderVersion(path,file) && ReadLoadedProviderVersion(system,mapped));
 std::fprintf(stderr,"Provider VERSIONINFO file=%u.%u.%u mapped=%u.%u.%u\n",file.major,file.minor,file.build,mapped.major,mapped.minor,mapped.build);
 assert(file.major==mapped.major && file.minor==mapped.minor && file.build==mapped.build);
 LARGE_INTEGER a{},b{},c{},frequency{};QueryPerformanceFrequency(&frequency);
 QueryPerformanceCounter(&a);
 for(int n=0;n<300;++n)assert(ReadProviderVersion(path,file));
 QueryPerformanceCounter(&b);
 for(int n=0;n<300;++n)assert(ReadLoadedProviderVersion(system,mapped));
 QueryPerformanceCounter(&c);
 std::printf("Mapped provider VERSIONINFO: malformed resources rejected, live image matches file version; 300 warm file reads %.3fms, mapped reads %.3fms (not a game benchmark)\n",
  (b.QuadPart-a.QuadPart)*1000./frequency.QuadPart,(c.QuadPart-b.QuadPart)*1000./frequency.QuadPart);
 assert(FreeLibrary(system));
}
