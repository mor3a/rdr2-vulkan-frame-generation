#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <detours.h>
#include <array>

namespace fgvk {
// Late cursor patches must enlist other threads so Detours can relocate an IP
// inside a replaced instruction. Failure leaves the optional hooks untouched.
// Never use this helper from DllMain or while holding the Windows loader lock.
class MenuHookTransaction {
  std::array<HANDLE,512> threads{};
  size_t count=0;
  bool active=false;
public:
  ~MenuHookTransaction(){if(active)DetourTransactionAbort();for(size_t i=0;i<count;++i)CloseHandle(threads[i]);}
  LONG Begin(){
    LONG result=DetourTransactionBegin();if(result!=NO_ERROR)return result;active=true;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE)return (LONG)GetLastError();
    THREADENTRY32 item{};item.dwSize=sizeof(item);
    BOOL more=Thread32First(snapshot,&item);
    if(!more){result=(LONG)GetLastError();CloseHandle(snapshot);return result;}
    const DWORD process=GetCurrentProcessId(),current=GetCurrentThreadId();
    for(;more;more=Thread32Next(snapshot,&item)){
      if(item.th32OwnerProcessID!=process || item.th32ThreadID==current)continue;
      if(count==threads.size()){result=ERROR_TOO_MANY_TCBS;break;}
      HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,item.th32ThreadID);
      if(!thread){result=(LONG)GetLastError();if(result==ERROR_INVALID_PARAMETER){result=NO_ERROR;continue;}break;}
      threads[count++]=thread;
    }
    if(!more && result==NO_ERROR){const auto error=GetLastError();if(error!=ERROR_NO_MORE_FILES)result=(LONG)error;}
    CloseHandle(snapshot);return result;
  }
  LONG Attach(void** original,void* replacement){return DetourAttach(original,replacement);}
  LONG Enlist(){for(size_t i=0;i<count;++i){LONG r=DetourUpdateThread(threads[i]);if(r!=NO_ERROR)return r;}return NO_ERROR;}
  LONG Commit(){active=false;return DetourTransactionCommit();}
  MenuHookTransaction()=default;
  MenuHookTransaction(const MenuHookTransaction&)=delete;
};
}
