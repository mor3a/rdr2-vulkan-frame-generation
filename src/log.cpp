#include "log.h"
#include "update_check.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <atomic>
#include <tlhelp32.h>
#include <delayimp.h>
#include <vector>
#include <string>
#include "diagnostic_channel.h"
namespace { std::mutex g_m; char g_path[MAX_PATH]{}; LARGE_INTEGER g_t0{}, g_freq{}; }
namespace {
fgvk::DiagnosticSnapshot* diagnosticSnapshot{};
HANDLE diagnosticRequest{},diagnosticDone{},diagnosticReady{},diagnosticMapping{},diagnosticProcess{};
void SignalSnapshot(EXCEPTION_POINTERS* info,DWORD reason=2){
  if(!diagnosticSnapshot || !diagnosticProcess || WaitForSingleObject(diagnosticReady,0)!=WAIT_OBJECT_0 || WaitForSingleObject(diagnosticProcess,0)!=WAIT_TIMEOUT)return;
  if(InterlockedCompareExchange(&diagnosticSnapshot->busy,1,0)!=0)return;
  ResetEvent(diagnosticDone);
  diagnosticSnapshot->threadId=GetCurrentThreadId();diagnosticSnapshot->reason=info?1:reason;
  if(info){diagnosticSnapshot->exception=*info->ExceptionRecord;diagnosticSnapshot->exception.ExceptionRecord=nullptr;diagnosticSnapshot->context=*info->ContextRecord;}
  MemoryBarrier();SetEvent(diagnosticRequest);
  // Keep the fault context alive briefly, without changing exception handling.
  if(info || reason==3)WaitForSingleObject(diagnosticDone,1500);
}
HANDLE exceptionFile=INVALID_HANDLE_VALUE;
std::atomic<unsigned> exceptionCount{0};
std::atomic<bool> debugEnabled{false};
constexpr DWORD missingDelayModule=0xC06D007E,missingDelayProcedure=0xC06D007F;
bool ReadDiagnosticString(const char* source,char* output,size_t capacity){
  if(!source || !capacity)return false;
  for(size_t i=0;i+1<capacity;++i){SIZE_T read{};char value{};
    if(!ReadProcessMemory(GetCurrentProcess(),source+i,&value,1,&read) || read!=1){output[i]=0;return false;}
    output[i]=value;if(!value)return true;
  }
  output[capacity-1]=0;return true;
}
LONG CALLBACK RecordException(EXCEPTION_POINTERS* info){
  if(!debugEnabled.load(std::memory_order_relaxed))return EXCEPTION_CONTINUE_SEARCH;
  if(!info || !info->ExceptionRecord || !info->ContextRecord)return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code=info->ExceptionRecord->ExceptionCode;
  const bool delayFailure=code==missingDelayModule || code==missingDelayProcedure;
  if(!delayFailure && !(code & 0x80000000))return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<unsigned> delayCount{0};
  if(exceptionFile==INVALID_HANDLE_VALUE || (delayFailure?delayCount.fetch_add(1)>=16:exceptionCount.fetch_add(1)>=64))return EXCEPTION_CONTINUE_SEARCH;
  // First-chance observation only. Do not swallow exceptions or use the normal
  // logger's mutex: the faulting thread may already own it.
  char record[4096]{};
  auto* c=info->ContextRecord;auto* e=info->ExceptionRecord;
  int used=snprintf(record,sizeof(record),"FIRST-CHANCE exception (may be handled by the game): tid=%lu code=0x%08lx address=%p RIP=%llx RSP=%llx\r\n",GetCurrentThreadId(),code,e->ExceptionAddress,(unsigned long long)c->Rip,(unsigned long long)c->Rsp);
  if(delayFailure){
    DelayLoadInfo detail{};SIZE_T copied{};
    bool readable=e->NumberParameters>=1 && ReadProcessMemory(GetCurrentProcess(),(const void*)e->ExceptionInformation[0],&detail,sizeof(detail),&copied) && copied==sizeof(detail) && detail.cb==sizeof(detail);
    if(readable){char dll[256]{},procedure[256]{};bool dllRead=ReadDiagnosticString(detail.szDll,dll,sizeof(dll));
      if(detail.dlp.fImportByName){bool procRead=ReadDiagnosticString(detail.dlp.szProcName,procedure,sizeof(procedure));used+=snprintf(record+used,sizeof(record)-used,"delay-load DLL=%s function=%s WindowsError=%lu loadedModule=%p\r\n",dllRead?dll:"<unreadable>",procRead?procedure:"<unreadable>",detail.dwLastError,detail.hmodCur);}
      else used+=snprintf(record+used,sizeof(record)-used,"delay-load DLL=%s ordinal=%lu WindowsError=%lu loadedModule=%p\r\n",dllRead?dll:"<unreadable>",detail.dlp.dwOrdinal,detail.dwLastError,detail.hmodCur);
    }else used+=snprintf(record+used,sizeof(record)-used,"delay-load metadata unavailable; exception-context dump requested\r\n");
  }
  if((code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) && e->NumberParameters>=2 && used>0)used+=snprintf(record+used,sizeof(record)-used,"memory operation=%llu target=0x%llx (0=read,1=write,8=execute)\r\n",(unsigned long long)e->ExceptionInformation[0],(unsigned long long)e->ExceptionInformation[1]);
  for(DWORD i=0;i<e->NumberParameters && i<EXCEPTION_MAXIMUM_PARAMETERS && used<(int)sizeof(record)-100;++i)used+=snprintf(record+used,sizeof(record)-used,"exception parameter[%lu]=0x%llx\r\n",i,(unsigned long long)e->ExceptionInformation[i]);
  if(code!=EXCEPTION_STACK_OVERFLOW){void* frames[32]{};auto count=CaptureStackBackTrace(0,32,frames,nullptr);for(unsigned i=0;i<count && used>0 && used<(int)sizeof(record)-80;++i)used+=snprintf(record+used,sizeof(record)-used,"exception stack[%u]=%p\r\n",i,frames[i]);}
  if(used>0){DWORD written{};WriteFile(exceptionFile,record,(DWORD)used,&written,nullptr);FlushFileBuffers(exceptionFile);}
  SignalSnapshot(info);
  return EXCEPTION_CONTINUE_SEARCH;
}
}
namespace fgvk {
void StartDiagnosticCollector(){
  if(!DebugLoggingEnabled())return;
  static std::atomic<bool> started{false};if(started.exchange(true))return;
  OSVERSIONINFOW os{};os.dwOSVersionInfoSize=sizeof(os);using QueryVersion=LONG(WINAPI*)(OSVERSIONINFOW*);
  auto query=(QueryVersion)GetProcAddress(GetModuleHandleA("ntdll.dll"),"RtlGetVersion");if(query && query(&os)==0)Log("OS version=%lu.%lu build=%lu",os.dwMajorVersion,os.dwMinorVersion,os.dwBuildNumber);
  SYSTEM_INFO system{};GetNativeSystemInfo(&system);MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);GlobalMemoryStatusEx(&memory);
  Log("host: processors=%lu architecture=%u RAM-total=%llu RAM-available=%llu",system.dwNumberOfProcessors,system.wProcessorArchitecture,memory.ullTotalPhys,memory.ullAvailPhys);
  wchar_t name[128]{},selfPath[MAX_PATH]{};HMODULE self{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&StartDiagnosticCollector,&self);
  GetModuleFileNameW(self,selfPath,MAX_PATH);auto slash=wcsrchr(selfPath,L'\\');if(!slash)return;*(slash+1)=0;
  wchar_t ini[MAX_PATH]{};swprintf(ini,MAX_PATH,L"%sRDR2VulkanFG.ini",selfPath);
  HANDLE settings=CreateFileW(ini,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(settings!=INVALID_HANDLE_VALUE){char contents[16384]{};DWORD read{};if(ReadFile(settings,contents,sizeof(contents)-1,&read,nullptr))Log("[INI snapshot; diagnostic logging is forced on]\n%.*s",(int)read,contents);CloseHandle(settings);}
  wchar_t helper[MAX_PATH]{};swprintf(helper,MAX_PATH,L"%sRDR2VulkanFG\\RDR2VulkanFGDiagnostics.dll",selfPath);
  if(GetFileAttributesW(helper)==INVALID_FILE_ATTRIBUTES){Log("Debug stack collector unavailable: bundled helper missing");return;}
  auto pid=GetCurrentProcessId();DiagnosticName(name,128,L"mapping",pid);
  diagnosticMapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(DiagnosticSnapshot),name);
  if(diagnosticMapping)diagnosticSnapshot=(DiagnosticSnapshot*)MapViewOfFile(diagnosticMapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(DiagnosticSnapshot));
  DiagnosticName(name,128,L"request",pid);diagnosticRequest=CreateEventW(nullptr,FALSE,FALSE,name);
  DiagnosticName(name,128,L"done",pid);diagnosticDone=CreateEventW(nullptr,TRUE,FALSE,name);
  DiagnosticName(name,128,L"ready",pid);diagnosticReady=CreateEventW(nullptr,TRUE,FALSE,name);
  if(!diagnosticSnapshot || !diagnosticRequest || !diagnosticDone || !diagnosticReady){Log("Debug stack channel creation failed: error=%lu",GetLastError());return;}
  wchar_t game[MAX_PATH]{};GetModuleFileNameW(nullptr,game,MAX_PATH);slash=wcsrchr(game,L'\\');if(!slash)return;*(slash+1)=0;
  wchar_t fixture[8]{};bool earlyDebug=GetEnvironmentVariableW(L"RDR2FG_EXTERNAL_DEBUGGER",fixture,8) && fixture[0]==L'1';
  wchar_t host[MAX_PATH]{};UINT hostLength=GetSystemDirectoryW(host,MAX_PATH);if(!hostLength || hostLength+14>=MAX_PATH){Log("Collector system host path unavailable");return;}wcscat_s(host,MAX_PATH,L"\\rundll32.exe");
  wchar_t cmd[2048]{};swprintf(cmd,2048,L"\"%s\" \"%s\",Collect %s %lu \"%sRDR2VulkanFG.log\"",host,helper,earlyDebug?L"--monitor-debug":L"--monitor",pid,game);
  STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
  if(CreateProcessW(host,cmd,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){diagnosticProcess=pi.hProcess;CloseHandle(pi.hThread);Log("Debug stack collector started; first-chance exceptions and presentation stalls recorded externally");}
  else Log("Debug stack collector start failed: error=%lu",GetLastError());
}
void RequestDiagnosticStacks(){if(DebugLoggingEnabled())SignalSnapshot(nullptr);}
void ArmDiagnosticDebugger(){Log("Diagnostic external fail-fast capture requested immediately before feature startup");SignalSnapshot(nullptr,3);}
void LogModules(){
  if(!DebugLoggingEnabled())return;
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
  if(snapshot==INVALID_HANDLE_VALUE){Log("module snapshot failed: Windows error=%lu",GetLastError());return;}
  MODULEENTRY32 module{};module.dwSize=sizeof(module);unsigned count=0;
  if(Module32First(snapshot,&module))do{
    DWORD ignored{};auto size=GetFileVersionInfoSizeA(module.szExePath,&ignored);VS_FIXEDFILEINFO* info{};UINT bytes{};std::vector<char> version(size);
    bool found=size && GetFileVersionInfoA(module.szExePath,0,size,version.data()) && VerQueryValueA(version.data(),"\\",(void**)&info,&bytes) && bytes>=sizeof(*info);
    Log("module: base=%p size=0x%lx name=%s path=%s version=%u.%u.%u.%u",module.modBaseAddr,module.modBaseSize,module.szModule,module.szExePath,found?HIWORD(info->dwFileVersionMS):0,found?LOWORD(info->dwFileVersionMS):0,found?HIWORD(info->dwFileVersionLS):0,found?LOWORD(info->dwFileVersionLS):0);
  }while(++count<512 && Module32Next(snapshot,&module));
  CloseHandle(snapshot);
}
void LogInit() {
  QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
  char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
  char* slash = strrchr(exe, '\\'); if (slash) *(slash+1) = 0;
  snprintf(g_path, sizeof(g_path), "%sRDR2VulkanFG.log", exe);
  char previous[MAX_PATH]{};snprintf(previous,sizeof(previous),"%sRDR2VulkanFG.previous.log",exe);
  MoveFileExA(g_path,previous,MOVEFILE_REPLACE_EXISTING);
  FILE* f=nullptr; if (fopen_s(&f,g_path,"w")==0 && f){ fprintf(f,"fgvk log start; pid=%lu; architecture=x64; diagnostic schema=5; %s\n",GetCurrentProcessId(),kModVersion); fclose(f);}
}
bool DebugLoggingEnabled(){return debugEnabled.load(std::memory_order_relaxed);}
void SetDebugLogging(bool enabled){
  debugEnabled.store(enabled,std::memory_order_relaxed);
  Log("DebugLogging=%d",(int)enabled);
  if(!enabled || exceptionFile!=INVALID_HANDLE_VALUE)return;
  exceptionFile=CreateFileA(g_path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(exceptionFile!=INVALID_HANDLE_VALUE)AddVectoredExceptionHandler(0,RecordException);
}
// Every line carries wall-clock time (to line up with sl.log's HH-MM-SS stamps) and seconds
// since DllMain.
static void WriteLog(const char* fmt,va_list ap) {
  std::lock_guard<std::mutex> lk(g_m);
  HANDLE file=CreateFileA(g_path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return;
  SYSTEMTIME st; GetLocalTime(&st);
  LARGE_INTEGER now; QueryPerformanceCounter(&now);
  double el = g_freq.QuadPart ? (double)(now.QuadPart-g_t0.QuadPart)/(double)g_freq.QuadPart : 0.0;
  char prefix[128]{};snprintf(prefix,sizeof(prefix),"[%02u:%02u:%02u.%03u +%8.3fs tid=%lu] ",st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,el,GetCurrentThreadId());
  va_list copy;va_copy(copy,ap);int length=vsnprintf(nullptr,0,fmt,copy);va_end(copy);
  if(length<0 || length>1048576){CloseHandle(file);return;}
  std::vector<char> message((size_t)length+1);vsnprintf(message.data(),message.size(),fmt,ap);
  std::string record=std::string(prefix)+message.data()+"\n";DWORD written{};WriteFile(file,record.data(),(DWORD)record.size(),&written,nullptr);FlushFileBuffers(file);CloseHandle(file); }
void Log(const char* fmt,...){va_list ap;va_start(ap,fmt);WriteLog(fmt,ap);va_end(ap);}
void LogDebug(const char* fmt,...){if(!DebugLoggingEnabled())return;va_list ap;va_start(ap,fmt);WriteLog(fmt,ap);va_end(ap);}
}
