#include "log.h"
#include "diagnostic_channel.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <delayimp.h>
LONG CALLBACK HandledTest(EXCEPTION_POINTERS* p){auto c=p->ExceptionRecord->ExceptionCode;return c==EXCEPTION_ACCESS_VIOLATION || c==0xC06D007F || c==0xC06D007E?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;}
int main(){
 char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);auto slash=strrchr(exe,92);if(!slash)return 1;*slash=0;SetCurrentDirectoryA(exe);
 FILE* fixture=fopen("ScriptHookRDR2.log","w");if(fixture){fputs("SCRIPT_HOOK_IMPORT_FIXTURE\n",fixture);fclose(fixture);}
 fixture=fopen("DiagnosticFixture.log","w");if(fixture){fputs("EXTERNAL_MODULE_ERROR_FIXTURE\n",fixture);fclose(fixture);}
 fgvk::LogInit();fgvk::LogDebug("SHOULD_NOT_APPEAR");fgvk::StartDiagnosticCollector();
 wchar_t name[128]{};fgvk::DiagnosticName(name,128,L"ready",GetCurrentProcessId());
 HANDLE absent=OpenEventW(SYNCHRONIZE,FALSE,name);if(absent){CloseHandle(absent);return 1;}
 fgvk::SetDebugLogging(true);fgvk::LogDebug("DEBUG_ENABLED_CHECK");fgvk::LogModules();fgvk::StartDiagnosticCollector();
 HANDLE ready=OpenEventW(SYNCHRONIZE,FALSE,name);if(!ready || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0)return 1;CloseHandle(ready);
 fixture=fopen("sl.log","a");if(fixture){fputs("SL_FALLBACK_IMPORT_FIXTURE\n",fixture);fclose(fixture);}
 auto handler=AddVectoredExceptionHandler(0,HandledTest);ULONG_PTR parameters[2]={0,0x1234};RaiseException(EXCEPTION_ACCESS_VIOLATION,0,2,parameters);RemoveVectoredExceptionHandler(handler);
 fgvk::DiagnosticName(name,128,L"mapping",GetCurrentProcessId());HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
 auto state=mapping?(fgvk::DiagnosticSnapshot*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(fgvk::DiagnosticSnapshot)):nullptr;if(!state)return 1;
 for(int i=0;i<500 && InterlockedCompareExchange(&state->busy,0,0);++i)Sleep(10);
 fgvk::RequestDiagnosticStacks();for(int i=0;i<500 && InterlockedCompareExchange(&state->busy,0,0);++i)Sleep(10);
 handler=AddVectoredExceptionHandler(0,HandledTest);
 DelayLoadInfo delay{};delay.cb=sizeof(delay);delay.szDll="diagnostic-fixture.dll";delay.dlp.fImportByName=TRUE;delay.dlp.szProcName="MissingTestExport";delay.dwLastError=ERROR_PROC_NOT_FOUND;
 ULONG_PTR pointer=(ULONG_PTR)&delay;RaiseException(0xC06D007F,0,1,&pointer);
 for(int i=0;i<500 && InterlockedCompareExchange(&state->busy,0,0);++i)Sleep(10);
 delay.dlp.fImportByName=FALSE;delay.dlp.dwOrdinal=42;RaiseException(0xC06D007F,0,1,&pointer);
 for(int i=0;i<500 && InterlockedCompareExchange(&state->busy,0,0);++i)Sleep(10);
 pointer=1;RaiseException(0xC06D007F,0,1,&pointer);
 for(int i=0;i<500 && InterlockedCompareExchange(&state->busy,0,0);++i)Sleep(10);
 pointer=(ULONG_PTR)&delay;delay.dwLastError=ERROR_MOD_NOT_FOUND;RaiseException(0xC06D007E,0,1,&pointer);
 for(int i=0;i<500 && InterlockedCompareExchange(&state->busy,0,0);++i)Sleep(10);
 RemoveVectoredExceptionHandler(handler);
 bool idle=InterlockedCompareExchange(&state->busy,0,0)==0;UnmapViewOfFile(state);CloseHandle(mapping);
 FILE* file=fopen("RDR2VulkanFG.log","rb");if(!file)return 1;char content[1048576]{};fread(content,1,sizeof(content)-1,file);fclose(file);
 bool ok=strstr(content,"SCRIPT_HOOK_IMPORT_FIXTURE") && GetFileAttributesA("RDR2VulkanFG.exceptions.log")==INVALID_FILE_ATTRIBUTES && GetFileAttributesA("RDR2VulkanFG.stacks.log")==INVALID_FILE_ATTRIBUTES && idle && strstr(content,"FIRST-CHANCE") && strstr(content,"target=0x1234");
 ok=ok && strstr(content,"EXTERNAL_MODULE_ERROR_FIXTURE") && strstr(content,"SL_FALLBACK_IMPORT_FIXTURE");
 ok=ok && strstr(content,"DLL=diagnostic-fixture.dll function=MissingTestExport WindowsError=127") && strstr(content,"ordinal=42") && strstr(content,"metadata unavailable") && strstr(content,"code=0xc06d007e");
 FILE* normal=fopen("RDR2VulkanFG.log","rb");char text[1048576]{};if(normal){fread(text,1,sizeof(text)-1,normal);fclose(normal);}ok=ok && strstr(text,"DEBUG_ENABLED_CHECK") && !strstr(text,"SHOULD_NOT_APPEAR");
 FILE* stacks=fopen("RDR2VulkanFG.log","rb");char details[1048576]{};if(stacks){fread(details,1,sizeof(details)-1,stacks);fclose(stacks);}
 ok=ok && strstr(details,"FIRST-CHANCE") && strstr(details,"presentation stall") && strstr(details,"exception=0xc0000005") && strstr(details,"threads dumped") && !strstr(details,"WARNING: ResumeThread");
 printf("Debug gating, handled exceptions, unified thread stacks and delay-load name/ordinal/invalid-pointer decoding: %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
