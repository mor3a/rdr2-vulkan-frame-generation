#include "turing_unlock.h"
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
namespace fgvk {
namespace {
std::atomic<int> loadedPreset{-1};
struct FilePin{const wchar_t* path;const char* sha;};
constexpr FilePin files[]={
 {L"version.dll","7b3f90435be4f94348bf8d128c84871c7f904c704f7690601a263277c327a7b2"},
 {L"dlssg_sm86\\backend\\sm86_backend.dll","30ffc6e09a347ccde87ecc6fa54fd7818f5ed8a9353f5c31c2a90835c7f0ca9b"},
 {L"dlssg_sm86\\runtime\\nvngx_dlssg.dll","cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72"},
 {L"dlssg_sm86\\kernels\\fg_kernels.dll","1394c2d5d9cc8938ba3e52b043048d20d6d4c13a019c161b11a3dfd16343ec24"},
};
bool HashMatches(const std::wstring& path,const char* expected){
 HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;
 BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};DWORD size{},read{};bool ok=false;
 std::vector<unsigned char> state;unsigned char digest[32]{},buffer[65536];
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
    BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&read,0)>=0){
  state.resize(size);
  if(BCryptCreateHash(algorithm,&hash,state.data(),size,nullptr,0,0)>=0){
   ok=true;while(true){if(!ReadFile(file,buffer,sizeof(buffer),&read,nullptr)){ok=false;break;}if(!read)break;if(BCryptHashData(hash,buffer,read,0)<0){ok=false;break;}}
   ok=ok && BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
  }
 }
 if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);CloseHandle(file);
 if(!ok)return false;char hex[65]{};for(unsigned i=0;i<32;++i)snprintf(hex+2*i,3,"%02x",digest[i]);
 return strcmp(hex,expected)==0;
}
}
bool VerifyTuringUnlock040(const wchar_t* directory,char* error,size_t capacity){
 if(!directory || !directory[0])return false;
 for(const auto& file:files){
  if(!HashMatches(std::wstring(directory)+L"\\"+file.path,file.sha)){
   snprintf(error,capacity,"RTX 20 unlock 0.4.0 file missing or mismatched: %ls; reinstall the complete RTX 20 package",file.path);
   SetLastError(ERROR_INVALID_DATA);return false;
  }
 }
 return true;
}
int TuringUnlock040LoadedPreset(){return loadedPreset.load(std::memory_order_acquire);}
bool LoadTuringUnlock040(const wchar_t* directory,char* error,size_t capacity,unsigned preset){
 if(!VerifyTuringUnlock040(directory,error,capacity))return false;
 auto modules=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
 if(modules==INVALID_HANDLE_VALUE){snprintf(error,capacity,"RTX 20 unlock: loaded-module check failed");return false;}
 MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);bool duplicate=false;
 if(Module32FirstW(modules,&entry))do{if(GetProcAddress(entry.hModule,"DlssgProxy_Role")){duplicate=true;break;}}while(Module32NextW(modules,&entry));
 CloseHandle(modules);
 if(duplicate){snprintf(error,capacity,"Another DLSSG SM86 proxy is already loaded; remove duplicate unlock proxies before using the RTX 20 package");return false;}
 const wchar_t* presets[]={L"Auto",L"A",L"B",L"C"};
 if(preset>3 || !WritePrivateProfileStringW(L"DLSSG",L"Preset",presets[preset],(std::wstring(directory)+L"\\dlssg_sm86.ini").c_str())){
  snprintf(error,capacity,"RTX 20 unlock preset configuration could not be prepared");return false;
 }
 // Arm architecture compatibility before Streamline can reject its DLSS-G plugin.
 // Both libraries remain resident; unloading live loader/NGX hooks is unsupported.
 if(!LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)){
  snprintf(error,capacity,"RTX 20 unlock: NVIDIA nvapi64.dll unavailable (Windows error %lu)",GetLastError());return false;
 }
 auto proxy=LoadLibraryExW((std::wstring(directory)+L"\\version.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
 if(!proxy){snprintf(error,capacity,"RTX 20 unlock 0.4.0 could not load (Windows error %lu)",GetLastError());return false;}
 if(!GetProcAddress(proxy,"DlssgProxy_Name") || !GetProcAddress(proxy,"DlssgProxy_Role")){
  snprintf(error,capacity,"RTX 20 unlock proxy identity exports missing");return false;
 }
 loadedPreset.store(int(preset),std::memory_order_release);
 return true; // Provider support and creation are validated separately by Streamline.
}
}
