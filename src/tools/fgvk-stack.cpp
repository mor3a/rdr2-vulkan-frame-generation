// External RDR2 diagnostic collector. Enabled only by DebugLogging.
// Out-of-process on purpose: walking stacks from inside the frozen process risks deadlocking on
// the heap/loader locks the suspended threads may hold.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <shellapi.h>
#include "diagnostic_channel.h"
#pragma comment(lib, "dbghelp.lib")

typedef LONG (NTAPI *PFN_NtQueryInformationThread)(HANDLE, int, PVOID, ULONG, PULONG);
static void AppendBlock(const char* path,const std::string& block){
  HANDLE out=CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(out==INVALID_HANDLE_VALUE)return;DWORD written{};WriteFile(out,block.data(),(DWORD)block.size(),&written,nullptr);FlushFileBuffers(out);CloseHandle(out);
}
struct ExternalLog {
  std::string path,label;ULONGLONG position{};
  void Poll(const char* unified){
    HANDLE file=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return;
    LARGE_INTEGER size{};GetFileSizeEx(file,&size);if((ULONGLONG)size.QuadPart<position)position=0;
    LARGE_INTEGER at{};at.QuadPart=position;SetFilePointerEx(file,at,nullptr,FILE_BEGIN);char data[16384]{};DWORD bytes{};
    while(ReadFile(file,data,sizeof(data),&bytes,nullptr) && bytes){AppendBlock(unified,"\n[External "+label+" offset="+std::to_string(position)+"]\n"+std::string(data,bytes)+"\n");position+=bytes;}
    CloseHandle(file);
  }
};

static std::string ModuleAndOffset(HANDLE hp, DWORD64 addr, bool& isSystem){
  char buf[512];
  DWORD64 base = SymGetModuleBase64(hp, addr);
  IMAGEHLP_MODULE64 mi{}; mi.SizeOfStruct = sizeof(mi);
  std::string mod = "?";
  if(base && SymGetModuleInfo64(hp, base, &mi)) mod = mi.ModuleName;
  isSystem = (_stricmp(mod.c_str(),"ntdll")==0 || _stricmp(mod.c_str(),"KERNELBASE")==0 ||
              _stricmp(mod.c_str(),"kernel32")==0 || _stricmp(mod.c_str(),"win32u")==0 ||
              _stricmp(mod.c_str(),"user32")==0 || _stricmp(mod.c_str(),"combase")==0 ||
              _stricmp(mod.c_str(),"RPCRT4")==0 || _stricmp(mod.c_str(),"ucrtbase")==0);
  char symbuf[sizeof(SYMBOL_INFO)+512]{}; SYMBOL_INFO* si=(SYMBOL_INFO*)symbuf;
  si->SizeOfStruct=sizeof(SYMBOL_INFO); si->MaxNameLen=511; DWORD64 disp=0;
  if(SymFromAddr(hp, addr, &disp, si))
    snprintf(buf,sizeof(buf),"%s!%s+0x%llx", mod.c_str(), si->Name, (unsigned long long)disp);
  else
    snprintf(buf,sizeof(buf),"%s+0x%llx", mod.c_str(), (unsigned long long)(base ? addr-base : addr));
  return buf;
}

static int Dump(DWORD pid,const char* path,const fgvk::DiagnosticSnapshot* fault,unsigned sequence){
  FILE* out = tmpfile(); if(!out){AppendBlock(path,"[Collector] temporary stack buffer creation failed\n");return 2;}
  HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_SUSPEND_RESUME, FALSE, pid);
  if(!hp){AppendBlock(path,"[Collector] OpenProcess failed error="+std::to_string(GetLastError())+"\n");fclose(out);return 1;}
  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(out,"==== fgvk-stack pid=%lu at %02u:%02u:%02u.%03u ====\n", pid, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  fprintf(out,"snapshot=%u; local symbols only; unresolved private names reported as module+offset\n",sequence);
  SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_NO_PROMPTS|SYMOPT_FAIL_CRITICAL_ERRORS);
  // Never download symbols or inherit a network symbol-server environment.
  if(!SymInitialize(hp, ".", TRUE)) fprintf(out,"SymInitialize failed: %lu (module names may be missing)\n", GetLastError());
  if(fault){
    fprintf(out,"reason=%s reporting tid=%lu\n",fault->reason==1?"FIRST-CHANCE (may be handled)":"presentation stall/checkpoint",fault->threadId);
    if(fault->reason==1){bool system=false;auto location=ModuleAndOffset(hp,fault->context.Rip,system);fprintf(out,"exception=0x%08lx fault=%s RIP=0x%llx RSP=0x%llx\n",fault->exception.ExceptionCode,location.c_str(),(unsigned long long)fault->context.Rip,(unsigned long long)fault->context.Rsp);}
    if(fault->reason==1){const auto& c=fault->context;fprintf(out,"fault registers RAX=%llx RBX=%llx RCX=%llx RDX=%llx RSI=%llx RDI=%llx RBP=%llx R8=%llx R9=%llx R10=%llx R11=%llx R12=%llx R13=%llx R14=%llx R15=%llx EFLAGS=%lx\n",c.Rax,c.Rbx,c.Rcx,c.Rdx,c.Rsi,c.Rdi,c.Rbp,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15,c.EFlags);}
  }
  fflush(out);
  auto pNtQIT = (PFN_NtQueryInformationThread)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if(snap == INVALID_HANDLE_VALUE){AppendBlock(path,"[Collector] thread snapshot failed error="+std::to_string(GetLastError())+"\n");fclose(out);SymCleanup(hp);CloseHandle(hp);return 1; }
  THREADENTRY32 te{}; te.dwSize = sizeof(te);
  int dumped=0, skipped=0;
  for(BOOL ok = Thread32First(snap,&te); ok; ok = Thread32Next(snap,&te)){
    if(te.th32OwnerProcessID != pid) continue;
    HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if(!ht){ skipped++; continue; }
    std::string startMod = "?";
    if(pNtQIT){ PVOID start=nullptr; if(pNtQIT(ht, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) == 0 && start){ bool s; startMod = ModuleAndOffset(hp,(DWORD64)start,s); } }
    std::vector<DWORD64> pcs;
    if(SuspendThread(ht) != (DWORD)-1){
      CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
      if(GetThreadContext(ht,&ctx)){
        if(fault && fault->reason==1 && te.th32ThreadID==fault->threadId)ctx=fault->context;
        pcs.push_back(ctx.Rip);
        STACKFRAME64 sf{};
        sf.AddrPC.Offset=ctx.Rip; sf.AddrPC.Mode=AddrModeFlat;
        sf.AddrFrame.Offset=ctx.Rbp; sf.AddrFrame.Mode=AddrModeFlat;
        sf.AddrStack.Offset=ctx.Rsp; sf.AddrStack.Mode=AddrModeFlat;
        for(int i=0;i<96;i++){
          if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hp, ht, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
          if(!sf.AddrPC.Offset) break;
          pcs.push_back(sf.AddrPC.Offset);
        }
      }
      if(ResumeThread(ht)==(DWORD)-1)fprintf(out,"WARNING: ResumeThread tid=%lu failed: %lu\n",te.th32ThreadID,GetLastError());
    }
    CloseHandle(ht);
    std::vector<std::string> lines;
    for(auto pc : pcs){ bool sys=false; lines.push_back(ModuleAndOffset(hp,pc,sys)); }
    dumped++;
    fprintf(out,"-- tid=%lu start=%s frames=%zu\n", te.th32ThreadID, startMod.c_str(), pcs.size());
    for(auto& l : lines) fprintf(out,"     %s\n", l.c_str());
  }
  CloseHandle(snap);
  fprintf(out,"==== %d threads dumped, %d inaccessible ====\n\n", dumped, skipped);
  fflush(out);long bytes=ftell(out);rewind(out);std::string block(bytes>0?(size_t)bytes:0,'\0');if(!block.empty())fread(block.data(),1,block.size(),out);fclose(out);AppendBlock(path,block);
  SymCleanup(hp); CloseHandle(hp);
  return 0;
}

static int CollectorMain(int argc,char** argv){
  if(argc==3)return Dump((DWORD)strtoul(argv[1],nullptr,10),argv[2],nullptr,0);
  const bool debugRequested=argc==4 && !strcmp(argv[1],"--monitor-debug");
  if(argc!=4 || (strcmp(argv[1],"--monitor") && !debugRequested))return 2;
  DWORD pid=(DWORD)strtoul(argv[2],nullptr,10);wchar_t name[128]{};
  fgvk::DiagnosticName(name,128,L"mapping",pid);HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
  auto state=mapping?(fgvk::DiagnosticSnapshot*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(fgvk::DiagnosticSnapshot)):nullptr;
  fgvk::DiagnosticName(name,128,L"request",pid);HANDLE request=OpenEventW(SYNCHRONIZE,FALSE,name);
  fgvk::DiagnosticName(name,128,L"done",pid);HANDLE done=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);
  fgvk::DiagnosticName(name,128,L"ready",pid);HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);
  HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if(!state || !request || !done || !ready || !process)return 1;
  bool debugging=false,attachBreakpoint=true;
  if(debugRequested){debugging=DebugActiveProcess(pid)!=FALSE;if(debugging)DebugSetProcessKillOnExit(FALSE);
    AppendBlock(argv[3],debugging?"[Collector] external Windows debugger attached for fail-fast capture; exceptions remain unhandled unless the game handles them\n":"[Collector] debugger attach failed error="+std::to_string(GetLastError())+"; ordinary recording continues\n");}
  AppendBlock(argv[3],"[Collector] external diagnostic collector ready; pid="+std::to_string(pid)+". First-chance snapshots do not prove fatal crashes.\n");
  std::string root=argv[3];auto slash=root.find_last_of("\\/");root=slash==std::string::npos?"":root.substr(0,slash+1);
  ExternalLog script{root+"ScriptHookRDR2.log","ScriptHookRDR2"},streamline{root+"sl.log","legacy Streamline file"};
  FILETIME created{},exited{},kernel{},user{};GetProcessTimes(process,&created,&exited,&kernel,&user);
  ULARGE_INTEGER processStart{};processStart.LowPart=created.dwLowDateTime;processStart.HighPart=created.dwHighDateTime;
  std::vector<ExternalLog> otherLogs;
  auto discover=[&]{WIN32_FIND_DATAA found{};HANDLE scan=FindFirstFileA((root+"*.log").c_str(),&found);if(scan==INVALID_HANDLE_VALUE)return;
    do{std::string name=found.cFileName;
      if((found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || !_stricmp(name.c_str(),"RDR2VulkanFG.log") || !_stricmp(name.c_str(),"ScriptHookRDR2.log") || !_stricmp(name.c_str(),"sl.log") || name.find("previous")!=std::string::npos)continue;
      bool known=false;for(auto& item:otherLogs)if(item.path==root+name)known=true;if(known || otherLogs.size()>=128)continue;
      ULARGE_INTEGER modified{};modified.LowPart=found.ftLastWriteTime.dwLowDateTime;modified.HighPart=found.ftLastWriteTime.dwHighDateTime;
      ULONGLONG size=((ULONGLONG)found.nFileSizeHigh<<32)|found.nFileSizeLow;
      otherLogs.push_back(ExternalLog{root+name,name,modified.QuadPart>=processStart.QuadPart?0:size});
      AppendBlock(argv[3],"[Collector] monitoring external log "+name+" initial-offset="+std::to_string(otherLogs.back().position)+"\n");
    }while(FindNextFileA(scan,&found));FindClose(scan);
  };
  discover();
  HANDLE old=CreateFileA(streamline.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(old!=INVALID_HANDLE_VALUE){LARGE_INTEGER size{};if(GetFileSizeEx(old,&size))streamline.position=size.QuadPart;CloseHandle(old);}
  SetEvent(ready);HANDLE waits[2]{process,request};unsigned sequence=0,ticks=0;
  for(;;){
    if(debugging){DEBUG_EVENT event{};
      if(WaitForDebugEvent(&event,10)){
        DWORD disposition=DBG_CONTINUE;
        if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT){
          auto& error=event.u.Exception;auto code=error.ExceptionRecord.ExceptionCode;
          if(code==EXCEPTION_BREAKPOINT && attachBreakpoint && error.dwFirstChance){attachBreakpoint=false;AppendBlock(argv[3],"[Collector] debugger attach breakpoint acknowledged\n");}
          else {disposition=DBG_EXCEPTION_NOT_HANDLED;
            if(!error.dwFirstChance || code==0xC0000409 || code==0xC0000602){
              fgvk::DiagnosticSnapshot snapshot{};snapshot.reason=1;snapshot.threadId=event.dwThreadId;snapshot.exception=error.ExceptionRecord;snapshot.exception.ExceptionRecord=nullptr;snapshot.context.ContextFlags=CONTEXT_FULL;
              HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,event.dwThreadId);
              bool got=thread && GetThreadContext(thread,&snapshot.context);if(thread)CloseHandle(thread);
              char summary[256]{};snprintf(summary,sizeof(summary),"[Collector] DEBUG EXCEPTION code=0x%08lx firstChance=%lu tid=%lu address=%p subcode=%llu context=%d\n",code,error.dwFirstChance,event.dwThreadId,error.ExceptionRecord.ExceptionAddress,error.ExceptionRecord.NumberParameters?(unsigned long long)error.ExceptionRecord.ExceptionInformation[0]:0,(int)got);AppendBlock(argv[3],summary);
              if(got)Dump(pid,argv[3],&snapshot,++sequence);
            }
          }
        }else if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT){if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);}
        else if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT){if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);if(event.u.CreateProcessInfo.hThread)CloseHandle(event.u.CreateProcessInfo.hThread);if(event.u.CreateProcessInfo.hProcess)CloseHandle(event.u.CreateProcessInfo.hProcess);}
        else if(event.dwDebugEventCode==CREATE_THREAD_DEBUG_EVENT){if(event.u.CreateThread.hThread)CloseHandle(event.u.CreateThread.hThread);}
        ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition);
      }
    }
    DWORD result=WaitForMultipleObjects(2,waits,FALSE,debugging?0:250);if(++ticks%(debugging?25:4)==0 || result==WAIT_OBJECT_0)discover();script.Poll(argv[3]);streamline.Poll(argv[3]);for(auto& source:otherLogs)source.Poll(argv[3]);
    if(result==WAIT_OBJECT_0){DWORD exit{};GetExitCodeProcess(process,&exit);char summary[128]{};snprintf(summary,sizeof(summary),"[Collector] process exit code=0x%08lx (%lu); snapshots=%u\n",exit,exit,sequence);AppendBlock(argv[3],summary);break;}
    if(result==WAIT_FAILED){AppendBlock(argv[3],"[Collector] wait failed error="+std::to_string(GetLastError())+"\n");break;}
    if(result==WAIT_OBJECT_0+1){MemoryBarrier();fgvk::DiagnosticSnapshot snapshot=*state;
      if(snapshot.reason==3){
        if(!debugging){debugging=DebugActiveProcess(pid)!=FALSE;if(debugging){DebugSetProcessKillOnExit(FALSE);attachBreakpoint=true;}}
        AppendBlock(argv[3],debugging?"[Collector] late external Windows debugger attached before feature startup\n":"[Collector] late debugger attach failed error="+std::to_string(GetLastError())+"\n");
        SetEvent(done);InterlockedExchange(&state->busy,0);continue;
      }
      bool delay=snapshot.reason==1 && (snapshot.exception.ExceptionCode==0xC06D007F || snapshot.exception.ExceptionCode==0xC06D007E);
      if(sequence<16 || delay)Dump(pid,argv[3],&snapshot,++sequence);
      SetEvent(done);InterlockedExchange(&state->busy,0);
    }
  }
  if(debugging)DebugActiveProcessStop(pid);
  UnmapViewOfFile(state);CloseHandle(mapping);CloseHandle(request);CloseHandle(done);CloseHandle(ready);CloseHandle(process);return 0;
}

#ifdef FGVK_COLLECTOR_DLL
// Windows' own rundll32 hosts the collector outside the game. No executable
// helper is distributed; preserve the same monitor and crash-capture code.
extern "C" __declspec(dllexport) void CALLBACK CollectW(HWND,HINSTANCE,LPWSTR command,int){
  if(!command)return;
  std::wstring line=L"collector ";line+=command;
  int count=0;auto wide=CommandLineToArgvW(line.c_str(),&count);if(!wide)return;
  std::vector<std::string> args;args.reserve(count);
  bool valid=true;
  for(int i=0;i<count;++i){
    int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,nullptr,0,nullptr,nullptr);
    if(length<=0){valid=false;break;}
    std::string converted(length,'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,converted.data(),length,nullptr,nullptr)){valid=false;break;}
    converted.pop_back();args.push_back(std::move(converted));
  }
  LocalFree(wide);if(!valid)return;
  std::vector<char*> pointers;for(auto& arg:args)pointers.push_back(arg.data());
  CollectorMain((int)pointers.size(),pointers.data());
}
#else
int main(int argc,char** argv){return CollectorMain(argc,argv);}
#endif
