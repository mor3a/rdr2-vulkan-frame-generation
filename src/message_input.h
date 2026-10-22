#pragma once
#include <windows.h>
#include <atomic>
namespace fgvk::menu_input {
using Handler=LRESULT(*)(HWND,UINT,WPARAM,LPARAM);
inline HHOOK hook{};inline HWND target{};inline Handler handler{};
inline std::atomic<unsigned> nestedCallbacks{0};
inline thread_local bool inCallback=false;
inline LRESULT CALLBACK QueueProcedure(int code,WPARAM remove,LPARAM data){
  if(code<0)return CallNextHookEx(hook,code,remove,data);
  // WH_GETMESSAGE does not use the callback's return value to dispatch MSG.
  // Leave a nested message intact, without re-entering the same hook chain.
  if(inCallback){nestedCallbacks.fetch_add(1,std::memory_order_relaxed);return 0;}
  struct Scope {Scope(){inCallback=true;}~Scope(){inCallback=false;}} scope;
  const LRESULT next=CallNextHookEx(hook,code,remove,data);
  if(code>=0 && remove==PM_REMOVE && data){
    auto* msg=reinterpret_cast<MSG*>(data);
    if(msg->hwnd==target && handler && handler(msg->hwnd,msg->message,msg->wParam,msg->lParam)==0){msg->message=WM_NULL;msg->wParam=0;msg->lParam=0;}
  }
  return next;
}
inline bool Install(HWND window,HMODULE module,Handler callback){
  if(hook){const bool same=target==window && handler==callback;if(!same)SetLastError(ERROR_ALREADY_EXISTS);return same;}
  DWORD process{};const DWORD thread=GetWindowThreadProcessId(window,&process);
  if(!window || !module || !callback || !thread || process!=GetCurrentProcessId()){SetLastError(ERROR_INVALID_PARAMETER);return false;}
  target=window;handler=callback;
  hook=SetWindowsHookExW(WH_GETMESSAGE,QueueProcedure,module,thread);
  return hook!=nullptr;
}
}
