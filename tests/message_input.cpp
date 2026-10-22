#include "message_input.h"
#include <cassert>
#include <cstdio>
static int handled=0;
static LRESULT Input(HWND,UINT msg,WPARAM,LPARAM){if(msg==WM_KEYDOWN){++handled;return 0;}return 1;}
static LRESULT CALLBACK Owner(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(){
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Owner;wc.lpszClassName=L"FGInputFixture";
 assert(RegisterClassW(&wc));HWND w=CreateWindowW(wc.lpszClassName,L"Fixture",0,0,0,20,20,nullptr,nullptr,wc.hInstance,nullptr);assert(w);
 const auto before=GetWindowLongPtrW(w,GWLP_WNDPROC);
 assert(!fgvk::menu_input::Install(nullptr,wc.hInstance,Input));assert(GetLastError()==ERROR_INVALID_PARAMETER);
 assert(fgvk::menu_input::Install(w,wc.hInstance,Input));assert(GetWindowLongPtrW(w,GWLP_WNDPROC)==before);
 const auto hook=fgvk::menu_input::hook;assert(fgvk::menu_input::Install(w,wc.hInstance,Input));assert(fgvk::menu_input::hook==hook);
 MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){}
 MSG nested{};nested.hwnd=w;nested.message=WM_KEYDOWN;nested.wParam=VK_INSERT;
 fgvk::menu_input::inCallback=true;
 fgvk::menu_input::QueueProcedure(0,PM_REMOVE,reinterpret_cast<LPARAM>(&nested));
 fgvk::menu_input::inCallback=false;
 assert(nested.message==WM_KEYDOWN && handled==0 && fgvk::menu_input::nestedCallbacks==1);
 assert(PostMessageW(w,WM_KEYDOWN,VK_INSERT,0));assert(PeekMessageW(&msg,w,0,0,PM_NOREMOVE));assert(handled==0);
 assert(PeekMessageW(&msg,w,0,0,PM_REMOVE));assert(handled==1);assert(msg.message==WM_NULL);
 assert(PostMessageW(w,WM_APP,7,9));assert(PeekMessageW(&msg,w,0,0,PM_REMOVE));assert(msg.message==WM_APP && msg.wParam==7 && msg.lParam==9);
 assert(GetWindowLongPtrW(w,GWLP_WNDPROC)==before);
 UnhookWindowsHookEx(fgvk::menu_input::hook);DestroyWindow(w);puts("Queued menu input: existing WndProc preserved, peek not consumed, owned key consumed once, unrelated messages forwarded: PASS");
}
