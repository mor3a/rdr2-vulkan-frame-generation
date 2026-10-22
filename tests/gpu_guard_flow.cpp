// Exercise the real guard control flow with non-interactive notification/exit
// substitutes. No game, window, thread suspension or process kill is executed.
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
static bool simulated{},notified{},closed{},attempted{},valid=true;
static int WINAPI GuardTestMessageBox(HWND,const wchar_t* text,const wchar_t*,UINT flags){
 valid=valid && !notified && !attempted && (flags&MB_TASKMODAL) &&
  wcsstr(text,L"RDR2VulkanFG.asi") && wcsstr(text,L"RDR2VulkanFGCamera.asi") &&
  wcsstr(text,L"RDR2VulkanFG.ini") && wcsstr(text,L"RDR2VulkanFG (folder)") &&
  wcsstr(text,L"close Red Dead Redemption 2") && bool(wcsstr(text,L"TEST MODE"))==simulated;
 notified=true;closed=true;return IDOK;
}
static BOOL WINAPI GuardTestTerminateProcess(HANDLE process,UINT code){
 valid=valid && notified && closed && process==GetCurrentProcess() && code==ERROR_NOT_SUPPORTED;
 attempted=true;return FALSE; // Also exercise the ExitProcess fallback.
}
[[noreturn]] static void WINAPI GuardTestExitProcess(UINT code){
 valid=valid && notified && closed && attempted && code==ERROR_NOT_SUPPORTED;
 std::printf("AMD guard %s flow: warning/removal instructions before current-process termination, fallback verified: %s\n",simulated?"simulated":"real",valid?"PASS":"FAIL");
 std::exit(valid?0:1);
}
#define MessageBoxW GuardTestMessageBox
#define TerminateProcess GuardTestTerminateProcess
#define ExitProcess GuardTestExitProcess
#include "../src/gpu_guard.cpp"
#undef MessageBoxW
#undef TerminateProcess
#undef ExitProcess
namespace fgvk {void Log(const char*,...) {}}
int main(int argc,char**){simulated=argc>1;if(fgvk::IsGpuBlocked())return 2;fgvk::BlockUnsupportedGpu(simulated);}
