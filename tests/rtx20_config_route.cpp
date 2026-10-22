#include "config.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <string>
namespace fgvk {void Log(const char*,...){} bool ConsumeMenuRequest(bool&,uint32_t&){return false;}void UpdateMenuRuntime(bool,uint32_t){}}
int main(){wchar_t self[MAX_PATH]{};GetModuleFileNameW(nullptr,self,MAX_PATH);auto slash=wcsrchr(self,L'\\');assert(slash);*slash=0;auto ini=std::wstring(self)+L"\\RDR2VulkanFG.ini";assert(GetFileAttributesW(ini.c_str())==INVALID_FILE_ATTRIBUTES);
 WritePrivateProfileStringW(L"fgvk",L"ConfigVersion",L"2",ini.c_str());WritePrivateProfileStringW(L"fgvk",L"Enabled",L"1",ini.c_str());WritePrivateProfileStringW(L"fgvk",L"ApplicationId",L"0x60E8AB7",ini.c_str());WritePrivateProfileStringW(L"fgvk",L"DLSSGBackend",L"Native",ini.c_str());
 auto test=[&](const wchar_t* provider,const wchar_t* opt,fgvk::FGProvider expected,bool native){WritePrivateProfileStringW(L"fgvk",L"FGProvider",provider,ini.c_str());WritePrivateProfileStringW(L"fgvk",L"EnableExperimentalRTX20",opt,ini.c_str());fgvk::LoadConfig();assert(fgvk::Cfg().provider==expected && fgvk::Cfg().nativeDLSSGBackend==native);};
 test(L"DLSSG",L"0",fgvk::FGProvider::DLSSG,true);
 test(L"DLSSG",L"1",fgvk::FGProvider::DLSSGBridge,false);
 test(L"XeSS",L"0",fgvk::FGProvider::XeSS,true);
 test(L"XeSS",L"1",fgvk::FGProvider::XeSS,false);
 test(L"OFF",L"1",fgvk::FGProvider::Off,false);assert(!fgvk::Cfg().enabled);
 assert(DeleteFileW(ini.c_str()));puts("Actual config parser: default Native preserved; RTX20 opt-in routes DLSS-G bridge; XeSS remains XeSS; OFF remains disabled: PASS");}
