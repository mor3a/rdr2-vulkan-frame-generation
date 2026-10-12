#include "menu.h"
#include "log.h"
#include "config.h"
#include "ampere_boot.h"
#include "fps_meter.h"
#include <windows.h>
#include <detours.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace fgvk {
static std::atomic<uint32_t> request{0},runtime{2},maximum{1},status{0};
static std::atomic<float> ratio{1},renderedFPS{0},displayedFPS{0};
static std::atomic<ULONGLONG> lastSample{0};
static std::atomic<bool> displayedValid{false},generationActive{false},open{false};
static std::atomic<bool> rendererAvailable{false};
static FPSMeter fpsMeter;
static HWND game{};static WNDPROC previous{};static bool initialized{},showFPS{};
static int nextProvider{};
struct Message {UINT id;WPARAM w;LPARAM l;};
static std::mutex inputMutex;static std::vector<Message> messages;
static decltype(&SetCursorPos) originalSetCursorPos=SetCursorPos;
static decltype(&ClipCursor) originalClipCursor=ClipCursor;
static POINT savedCursor{};static RECT desiredClip{};static bool hasClip{};
static BOOL WINAPI CursorPosition(int x,int y){if(open && GetForegroundWindow()==game)return TRUE;return originalSetCursorPos(x,y);}
static BOOL WINAPI CursorClip(const RECT* rect){
  if(open && GetForegroundWindow()==game){std::lock_guard<std::mutex> lock(inputMutex);hasClip=rect!=nullptr;if(rect)desiredClip=*rect;return originalClipCursor(nullptr);}
  return originalClipCursor(rect);
}
static void Toggle(bool value){
  if(open.exchange(value)==value)return;
  std::lock_guard<std::mutex> lock(inputMutex);
  if(value){GetCursorPos(&savedCursor);GetClipCursor(&desiredClip);hasClip=true;originalClipCursor(nullptr);}
  else if(GetForegroundWindow()==game){originalSetCursorPos(savedCursor.x,savedCursor.y);if(hasClip)originalClipCursor(&desiredClip);}
  else originalClipCursor(nullptr);
}
static LRESULT CALLBACK GameProcedure(HWND w,UINT m,WPARAM a,LPARAM b){
  if(m==WM_KEYDOWN && a==VK_INSERT && rendererAvailable){if(!(b&(1LL<<30)))Toggle(!open);return 0;}
  if(m==WM_KEYUP && a==VK_INSERT)return 0;
  if(m==WM_KILLFOCUS)Toggle(false);
  if(m==WM_KILLFOCUS || m==WM_SETFOCUS){std::lock_guard<std::mutex> lock(inputMutex);if(messages.size()<1024)messages.push_back({m,a,b});}
  if(m==WM_MOUSEMOVE || (m>=WM_LBUTTONDOWN && m<=WM_MOUSEHWHEEL) || m==WM_KEYDOWN || m==WM_KEYUP || m==WM_CHAR || m==WM_SYSKEYDOWN || m==WM_SYSKEYUP){
    if(open){std::lock_guard<std::mutex> lock(inputMutex);if(messages.size()<1024)messages.push_back({m,a,b});return 0;}
  }
  if(open && m==WM_INPUT)return DefWindowProcW(w,m,a,b);
  return CallWindowProcW(previous,w,m,a,b);
}
static BOOL CALLBACK FindGame(HWND w,LPARAM out){DWORD pid{};GetWindowThreadProcessId(w,&pid);wchar_t title[128]{};GetWindowTextW(w,title,128);if(pid==GetCurrentProcessId() && wcscmp(title,L"Red Dead Redemption 2")==0){*reinterpret_cast<HWND*>(out)=w;return FALSE;}return TRUE;}
bool MenuIsOpen(){return open;}
void MenuRendererAvailable(bool available){rendererAvailable=available;if(!available)Toggle(false);}
void StartMenu(){}
void SetFPSGenerationActive(bool active){generationActive.store(active);}
void RecordDisplayedFrames(uint32_t n){fpsMeter.AddDisplayed(n);}
void RecordGamePresent(){auto now=GetTickCount64();if(fpsMeter.Present(now)){renderedFPS=fpsMeter.realFPS;displayedFPS=fpsMeter.outputFPS;displayedValid=fpsMeter.outputValid;lastSample=now;}}
void UpdateMenuRuntime(bool off,uint32_t frames){runtime=(frames<<1)|(off?1:0);}
void UpdateMenuStats(uint32_t max,uint32_t s,float r){maximum=max;status=s;ratio=r;}
bool ConsumeMenuRequest(bool& off,uint32_t& frames){auto r=request.exchange(0);if(!r)return false;off=(r&1)!=0;frames=(r>>1)&7;return true;}
bool MenuInitializeUI(){
  if(initialized)return true;
  EnumWindows(FindGame,reinterpret_cast<LPARAM>(&game));if(!game)return false;
  IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();style.WindowPadding={20,16};style.FramePadding={10,6};style.ItemSpacing={10,8};style.WindowRounding=8;style.FrameRounding=5;style.WindowBorderSize=0;
  style.Colors[ImGuiCol_WindowBg]={.075f,.086f,.105f,.98f};style.Colors[ImGuiCol_FrameBg]={.13f,.15f,.18f,1};style.Colors[ImGuiCol_CheckMark]={.38f,.76f,.92f,1};style.Colors[ImGuiCol_Button]={.15f,.20f,.26f,1};
  io.ConfigWindowsMoveFromTitleBarOnly=true;
  if(!ImGui_ImplWin32_Init(game)){ImGui::DestroyContext();return false;}
  previous=(WNDPROC)SetWindowLongPtrW(game,GWLP_WNDPROC,(LONG_PTR)GameProcedure);
  if(!previous){ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();return false;}
  DetourTransactionBegin();DetourUpdateThread(GetCurrentThread());DetourAttach(&(PVOID&)originalSetCursorPos,(PVOID)CursorPosition);DetourAttach(&(PVOID&)originalClipCursor,(PVOID)CursorClip);
  auto result=DetourTransactionCommit();if(result)Log("Menu: cursor routing detours failed=%ld",result);
  showFPS=LoadFPSCounter();nextProvider=Cfg().provider==FGProvider::FSR3?1:0;initialized=true;
  Log("Menu ready: native Vulkan overlay, Insert toggles game UI; no secondary window");return true;
}
bool MenuHasDrawing(){return open || showFPS;}
void MenuBuildUI(){
  std::vector<Message> pending;{std::lock_guard<std::mutex> lock(inputMutex);pending.swap(messages);}
  for(auto& m:pending)ImGui_ImplWin32_WndProcHandler(game,m.id,m.w,m.l);
  ImGui_ImplWin32_NewFrame();ImGui::GetIO().MouseDrawCursor=open;ImGui::NewFrame();
  if(showFPS){
    char text[64]{};bool fresh=GetTickCount64()-lastSample.load()<2000;float real=renderedFPS,output=displayedFPS;
    if(!fresh)snprintf(text,sizeof(text),"-- FPS");
    else if(displayedValid && output>real*1.15f && !Rt().fgUserOff)snprintf(text,sizeof(text),"%.0f / %.0f FPS",real,output);
    else if(!displayedValid && generationActive && !Rt().fgUserOff)snprintf(text,sizeof(text),"%.0f / -- FPS",real);
    else snprintf(text,sizeof(text),"%.0f FPS",real);
    auto* draw=ImGui::GetForegroundDrawList();auto size=ImGui::CalcTextSize(text);ImVec2 p{ImGui::GetIO().DisplaySize.x-size.x-12,12};
    draw->AddText({p.x+1,p.y+1},IM_COL32(0,0,0,230),text);draw->AddText(p,IM_COL32(240,245,250,255),text);
  }
  if(open){
    ImGui::SetNextWindowPos({(ImGui::GetIO().DisplaySize.x-460)/2,(ImGui::GetIO().DisplaySize.y-430)/2},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({460,430},ImGuiCond_FirstUseEver);
    bool visible=true;ImGui::Begin("Frame Generation",&visible,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse);
    ImGui::TextDisabled("RDR2 / Vulkan");ImGui::TextWrapped("%s",GPUName());ImGui::Separator();
    auto pendingRequest=request.load();auto r=pendingRequest?(pendingRequest&15):runtime.load();bool enabled=(r&1)==0;int mode=int(r>>1)-1;
    if(ImGui::Checkbox("Frame generation",&enabled))request=0x100|((mode+1)<<1)|(enabled?0:1);
    const char* providers[]={"DLSS-G","FSR3 (native)","Off"};bool fsr=Cfg().provider==FGProvider::FSR3;int selection=!enabled?2:nextProvider;
    ImGui::SetNextItemWidth(280);
    if(ImGui::Combo("Provider",&selection,providers,3)){
      if(selection==2)request=0x100|((mode+1)<<1)|1;
      else{nextProvider=selection;SaveNextProvider(selection==1?FGProvider::FSR3:FGProvider::DLSSG);if(selection==(fsr?1:0))request=0x100|((mode+1)<<1);}
    }
    if(nextProvider!=(fsr?1:0))ImGui::TextColored({1,.76f,.3f,1},"Restart game to apply %s",nextProvider==1?"FSR3":"DLSS-G");else ImGui::TextDisabled("Active: %s",fsr?"FSR3":"DLSS-G");
    const char* modes[]={"2x","3x","4x"};if(fsr)mode=0;
    ImGui::BeginDisabled(fsr);ImGui::SetNextItemWidth(280);if(ImGui::Combo("Multiplier",&mode,modes,3))request=0x100|((mode+1)<<1)|(enabled?0:1);ImGui::EndDisabled();
    if(fsr)ImGui::TextDisabled("FSR3 supports 2x");
    ImGui::TextDisabled("FG pauses while this menu is open");
    if(ImGui::Checkbox("Show FPS counter",&showFPS))SaveFPSCounter(showFPS);
    ImGui::Separator();
    if(ImGui::CollapsingHeader("Details")){ImGui::TextWrapped("Unlock: %s",fsr?"not required":GPUUnlockStatus());ImGui::Text("Measured ratio: %.2fx / Status: %u",ratio.load(),status.load());if(!fsr)ImGui::Text("Driver maximum: %ux",maximum.load()+1);if(!fsr && GPUFamily()==GpuFamily::RTX20)ImGui::TextWrapped("RTX 20 support is experimental and hardware untested.");}
    if(ImGui::GetCursorPosY()<365)ImGui::SetCursorPosY(365);
    if(ImGui::Button("Close (Insert)",{130,30}))visible=false;
    ImGui::SameLine();ImGui::SetCursorPosX(ImGui::GetWindowWidth()-ImGui::CalcTextSize("made by mor3a").x-ImGui::GetStyle().WindowPadding.x);ImGui::TextDisabled("made by mor3a");
    ImGui::End();if(!visible)Toggle(false);
  }
  ImGui::Render();
}
}
