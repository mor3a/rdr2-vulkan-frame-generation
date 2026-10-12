#include <windows.h>
// Loaded by the existing dinput8 ASI loader, after ScriptHook dependencies of
// the other game scripts. Never register a script from a polling worker thread.
BOOL APIENTRY DllMain(HMODULE self,DWORD reason,LPVOID){
  if(reason==DLL_PROCESS_ATTACH){
    DisableThreadLibraryCalls(self);
    HMODULE fg=GetModuleHandleW(L"RDR2VulkanFG.asi");
    if(fg){
      auto registerCamera=(void(__cdecl*)())GetProcAddress(fg,"RDR2FGRegisterCamera");
      if(registerCamera)registerCamera();
    }
  }
  return TRUE;
}
