#include <windows.h>
#include <delayimp.h>
#include "log.h"
#include "diagnostic_channel.h"
#include <intrin.h>
#include <cstring>
int main(){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  SetEnvironmentVariableW(L"RDR2FG_EXTERNAL_DEBUGGER",L"1");
  fgvk::LogInit();fgvk::SetDebugLogging(true);fgvk::StartDiagnosticCollector();
  wchar_t name[128]{};fgvk::DiagnosticName(name,128,L"ready",GetCurrentProcessId());
  HANDLE ready=OpenEventW(SYNCHRONIZE,FALSE,name);if(!ready || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0)return 2;CloseHandle(ready);
  if(strstr(GetCommandLineA(),"--fastfail"))__fastfail(7);
  DelayLoadInfo detail{};detail.cb=sizeof(detail);detail.szDll="fatal-fixture.dll";detail.dlp.fImportByName=TRUE;detail.dlp.szProcName="FatalMissingExport";detail.dwLastError=ERROR_PROC_NOT_FOUND;
  ULONG_PTR argument=(ULONG_PTR)&detail;
  RaiseException(0xC06D007F,EXCEPTION_NONCONTINUABLE,1,&argument);
  return 3;
}
