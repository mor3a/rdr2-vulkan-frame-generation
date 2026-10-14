#include <windows.h>
#include <cstring>
#include "log.h"
#include "vkhooks.h"
#include "config.h"
#include "ngx_capture.h"
#include "inputs.h"
#include "rdr_camera.h"
static bool installed=false;
static HMODULE fgModule{};
extern "C" __declspec(dllexport) void __cdecl RDR2FGRegisterCamera(){
  static bool registered=false;
  if(installed && !registered)registered=fgvk::RegisterRdrCamera(fgModule);
}
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
  if(reason==DLL_PROCESS_ATTACH){
    DisableThreadLibraryCalls(h);
    fgModule=h;
    char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr,exe,MAX_PATH);
    const char* base=strrchr(exe,'\\'); base=base?base+1:exe;
    if(_stricmp(base,"RDR2.exe")!=0) return TRUE;
    fgvk::LogInit(); fgvk::LoadConfig();
    fgvk::SetDebugLogging(fgvk::Cfg().debugLogging);
    if(!fgvk::Cfg().enabled){ fgvk::Log("FG disabled; native rendering, no diagnostic hooks"); return TRUE; }
    if(!fgvk::Cfg().applicationId){ fgvk::Log("ApplicationId missing; refusing BG3 identity and leaving game untouched"); return TRUE; }
    fgvk::Log("mor3a RDR2 Vulkan FG 0.30; provider=%s; installing early hooks",fgvk::Cfg().provider==fgvk::FGProvider::FSR3?"FSR3":"DLSSG");
    if(!fgvk::InstallEarlyNgxHooks()){fgvk::Log("Early NGX routing failed; Vulkan hooks not installed");return TRUE;}
    fgvk::InstallVkHooks(); installed=true;
  }else if(reason==DLL_PROCESS_DETACH && installed && !reserved){
    // Explicit unloading is unsupported while presents or the upstream watchdog run.
    // Process termination needs no detour teardown under the Windows loader lock.
  }
  return TRUE;
}

