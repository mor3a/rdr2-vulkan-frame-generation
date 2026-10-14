#include "menu.h"
#include "log.h"
#include "config.h"
#include "ampere_boot.h"
#include "fps_meter.h"
#include "update_check.h"
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
static int nextProvider{};static int fpsCapSetting{};static float menuScale{1};static bool scaleChanged{};static std::atomic<bool> captureMenuKey{false};
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
static const char* KeyName(int vk){
  switch(vk){
    case VK_INSERT:return "Insert";case VK_DELETE:return "Delete";case VK_HOME:return "Home";case VK_END:return "End";
    case VK_PRIOR:return "Page Up";case VK_NEXT:return "Page Down";case VK_PAUSE:return "Pause";case VK_SCROLL:return "Scroll Lock";
    case VK_MULTIPLY:return "Numpad *";case VK_F1:return "F1";case VK_F2:return "F2";case VK_F3:return "F3";case VK_F4:return "F4";
    case VK_F5:return "F5";case VK_F6:return "F6";case VK_F7:return "F7";case VK_F8:return "F8";case VK_F9:return "F9";
    case VK_F10:return "F10";case VK_F11:return "F11";case VK_F12:return "F12";
    default:break;
  }
  static char buffer[24]{};
  if((vk>='A' && vk<='Z') || (vk>='0' && vk<='9'))snprintf(buffer,sizeof(buffer),"%c",vk);
  else snprintf(buffer,sizeof(buffer),"VK 0x%X",vk);
  return buffer;
}
static LRESULT CALLBACK GameProcedure(HWND w,UINT m,WPARAM a,LPARAM b){
  if(captureMenuKey && m==WM_KEYDOWN){if((int)a!=VK_ESCAPE)SaveMenuKey((int)a);captureMenuKey=false;return 0;}
  if(m==WM_KEYDOWN && (int)a==Rt().menuKey.load() && rendererAvailable){if(!(b&(1LL<<30)))Toggle(!open);return 0;}
  if(m==WM_KEYUP && (int)a==Rt().menuKey.load())return 0;
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
  // Rasterize at the final integer pixel size instead of shrinking the font atlas.
  ImFontConfig fontConfig; fontConfig.SizePixels=16.0f;
  fontConfig.OversampleH=2; fontConfig.OversampleV=1; fontConfig.PixelSnapH=true;
  char fontPath[MAX_PATH]{};GetWindowsDirectoryA(fontPath,MAX_PATH);
  strcat_s(fontPath,"\\Fonts\\segoeui.ttf");
  if(!io.Fonts->AddFontFromFileTTF(fontPath,16.0f,&fontConfig))io.Fonts->AddFontDefault(&fontConfig);
  ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();
  style.WindowPadding={16,14};style.FramePadding={9,6};style.ItemSpacing={8,6};style.ItemInnerSpacing={6,4};
  style.WindowRounding=14;style.ChildRounding=12;style.FrameRounding=8;style.PopupRounding=10;style.ScrollbarRounding=8;style.GrabRounding=8;
  style.WindowBorderSize=1;style.FrameBorderSize=0;style.PopupBorderSize=1;
  auto& colors=style.Colors;
  colors[ImGuiCol_WindowBg]={.045f,.06f,.085f,.97f};colors[ImGuiCol_ChildBg]={.075f,.095f,.13f,.72f};colors[ImGuiCol_PopupBg]={.06f,.08f,.11f,.98f};
  colors[ImGuiCol_Border]={.25f,.55f,.78f,.38f};colors[ImGuiCol_FrameBg]={.11f,.145f,.19f,1};colors[ImGuiCol_FrameBgHovered]={.15f,.21f,.28f,1};colors[ImGuiCol_FrameBgActive]={.18f,.28f,.38f,1};
  colors[ImGuiCol_TitleBg]={.055f,.075f,.105f,1};colors[ImGuiCol_TitleBgActive]={.07f,.11f,.16f,1};colors[ImGuiCol_TitleBgCollapsed]={.05f,.07f,.10f,1};
  colors[ImGuiCol_CheckMark]={.35f,.78f,1,1};colors[ImGuiCol_SliderGrab]={.30f,.72f,1,1};colors[ImGuiCol_SliderGrabActive]={.48f,.85f,1,1};
  colors[ImGuiCol_Button]={.13f,.28f,.42f,1};colors[ImGuiCol_ButtonHovered]={.18f,.40f,.60f,1};colors[ImGuiCol_ButtonActive]={.22f,.52f,.78f,1};
  colors[ImGuiCol_Header]={.12f,.25f,.36f,1};colors[ImGuiCol_HeaderHovered]={.16f,.34f,.48f,1};colors[ImGuiCol_HeaderActive]={.20f,.43f,.60f,1};
  colors[ImGuiCol_Separator]={.25f,.55f,.78f,.28f};colors[ImGuiCol_Text]={.92f,.95f,.98f,1};colors[ImGuiCol_TextDisabled]={.70f,.77f,.85f,1};
  io.ConfigWindowsMoveFromTitleBarOnly=true;
  if(!ImGui_ImplWin32_Init(game)){ImGui::DestroyContext();return false;}
  previous=(WNDPROC)SetWindowLongPtrW(game,GWLP_WNDPROC,(LONG_PTR)GameProcedure);
  if(!previous){ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();return false;}
  DetourTransactionBegin();DetourUpdateThread(GetCurrentThread());DetourAttach(&(PVOID&)originalSetCursorPos,(PVOID)CursorPosition);DetourAttach(&(PVOID&)originalClipCursor,(PVOID)CursorClip);
  auto result=DetourTransactionCommit();if(result)Log("Menu: cursor routing detours failed=%ld",result);
  showFPS=LoadFPSCounter();fpsCapSetting=(int)LoadFPSCap();Rt().fpsCap=(uint32_t)fpsCapSetting;ImGui::GetIO().FontGlobalScale=1.0f;nextProvider=Cfg().provider==FGProvider::FSR3?1:0;initialized=true;
  StartUpdateCheck();
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
    auto* draw=ImGui::GetForegroundDrawList();auto size=ImGui::CalcTextSize(text);ImVec2 p{ImGui::GetIO().DisplaySize.x-size.x-24,18};
    draw->AddRectFilled({p.x-10,p.y-6},{p.x+size.x+10,p.y+size.y+6},IM_COL32(5,10,17,195),8);
    draw->AddRect({p.x-10,p.y-6},{p.x+size.x+10,p.y+size.y+6},IM_COL32(90,180,240,85),8);
    draw->AddText({p.x+1,p.y+1},IM_COL32(0,0,0,230),text);draw->AddText(p,IM_COL32(240,245,250,255),text);
  }
  if(open){
    ImGui::SetNextWindowPos({(ImGui::GetIO().DisplaySize.x-580)/2,(ImGui::GetIO().DisplaySize.y-510)/2},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({580,0},{580,ImGui::GetIO().DisplaySize.y-32});
    bool visible=true;ImGui::Begin("RDR2 Vulkan Frame Generation",&visible,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored({.42f,.80f,1,1},"RDR2 VULKAN FRAME GENERATION");
    ImGui::SameLine();ImGui::TextDisabled("- by Mor3a");
    ImGui::SameLine();ImGui::SetCursorPosX(ImGui::GetWindowWidth()-76);ImGui::TextDisabled("v0.30");
    ImGui::TextWrapped("%s",GPUName());
    ImGui::TextDisabled("Red Dead Redemption 2  |  Vulkan");
    ImGui::Separator();
    if(GetUpdateState()==UpdateState::Available){
      ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4{.22f,.15f,.05f,.92f});
      ImGui::BeginChild("UpdateCard",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
      ImGui::TextColored({1,.78f,.32f,1},"UPDATE AVAILABLE");
      ImGui::TextWrapped("Version %s is available on Nexus.",LatestUpdateVersion());
      if(UpdateDownloadURL()[0]){
        ImGui::BeginDisabled(UpdateBrowserOpening());
        if(ImGui::Button("Open Nexus page",{-1,28}))OpenUpdateDownload();
        ImGui::EndDisabled();
        if(UpdateBrowserFailed())ImGui::TextDisabled("Could not open browser");
      }
      else ImGui::TextDisabled("Check the Nexus mod page for the new release.");
      ImGui::EndChild();ImGui::PopStyleColor();ImGui::Spacing();
    }
    auto pendingRequest=request.load();auto r=pendingRequest?(pendingRequest&15):runtime.load();bool enabled=(r&1)==0;int mode=int(r>>1)-1;
    ImGui::TextColored({.42f,.80f,1,1},"GENERATION");ImGui::Separator();
    ImGui::BeginChild("GenerationCard",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
    if(ImGui::Checkbox("Enable frame generation",&enabled))request=0x100|((mode+1)<<1)|(enabled?0:1);
    ImGui::SameLine();ImGui::TextColored(enabled?ImVec4{.35f,.90f,.55f,1}:ImVec4{.95f,.45f,.40f,1},enabled?"ACTIVE":"OFF");
    const char* providers[]={"DLSS-G","FSR3 (native)","Off"};bool fsr=Cfg().provider==FGProvider::FSR3;int selection=!enabled?2:nextProvider;
    ImGui::SetNextItemWidth(280);
    if(ImGui::Combo("Provider",&selection,providers,3)){
      if(selection==2)request=0x100|((mode+1)<<1)|1;
      else{nextProvider=selection;SaveNextProvider(selection==1?FGProvider::FSR3:FGProvider::DLSSG);if(selection==(fsr?1:0))request=0x100|((mode+1)<<1);}
    }
    if(nextProvider!=(fsr?1:0))ImGui::TextColored({1,.76f,.3f,1},"Restart game to apply %s",nextProvider==1?"FSR3":"DLSS-G");else ImGui::TextDisabled("Active: %s",fsr?"FSR3":"DLSS-G");
    const char* modes[]={"2x","3x"};if(fsr)mode=0;
    ImGui::BeginDisabled(fsr);ImGui::SetNextItemWidth(280);if(ImGui::Combo("Multiplier",&mode,modes,2))request=0x100|((mode+1)<<1)|(enabled?0:1);ImGui::EndDisabled();
    if(fsr)ImGui::TextDisabled("FSR3 supports 2x");
    ImGui::TextDisabled("Generation pauses while this menu is open.");
    ImGui::EndChild();
    ImGui::Spacing();ImGui::TextColored({.42f,.80f,1,1},"DISPLAY");ImGui::Separator();
    ImGui::BeginChild("DisplayCard",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
    if(ImGui::Checkbox("Show FPS counter",&showFPS))SaveFPSCounter(showFPS);
    ImGui::SetNextItemWidth(280);if(ImGui::SliderInt("FPS limit",&fpsCapSetting,0,240,"%d")){if(fpsCapSetting<0)fpsCapSetting=0;SaveFPSCap((uint32_t)fpsCapSetting);}
    if(fpsCapSetting==0)ImGui::SameLine(),ImGui::TextDisabled("Unlimited");
    ImGui::EndChild();
    ImGui::Spacing();
    if(ImGui::CollapsingHeader("Details")){
      ImGui::TextDisabled("State");ImGui::SameLine(170);ImGui::Text("%s",!enabled?"Off":(generationActive?"Active":"Waiting for game frames"));
      ImGui::TextDisabled("Frame rate");ImGui::SameLine(170);if(displayedValid)ImGui::Text("%.0f to %.0f FPS",renderedFPS.load(),displayedFPS.load());else ImGui::Text("%.0f FPS",renderedFPS.load());
      ImGui::TextDisabled("Measured ratio");ImGui::SameLine(170);ImGui::Text("%.2fx",ratio.load());
      ImGui::TextDisabled("Active provider");ImGui::SameLine(170);ImGui::Text("%s",fsr?"FSR3":"DLSS-G");
      ImGui::TextDisabled("FPS limit");ImGui::SameLine(170);if(fpsCapSetting)ImGui::Text("%d FPS",fpsCapSetting);else ImGui::Text("Unlimited");
      if(!fsr){ImGui::TextDisabled("Max multiplier");ImGui::SameLine(170);ImGui::Text("%ux",maximum.load()+1);}
      if(!fsr && GPUFamily()==GpuFamily::RTX20)ImGui::TextWrapped("RTX 20 support is experimental and hardware untested.");
    }
    ImGui::Spacing();
    if(captureMenuKey){ImGui::TextColored({1,.78f,.32f,1},"Press a key for the menu shortcut (Esc cancels)");}
    char keyLabel[64]{};snprintf(keyLabel,sizeof(keyLabel),"Menu key: %s",KeyName(Rt().menuKey.load()));
    if(ImGui::Button(keyLabel,{190,34}))captureMenuKey=true;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,{.13f,.34f,.55f,1});ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.18f,.46f,.72f,1});
    if(ImGui::Button("Close",{110,34}))visible=false;
    ImGui::PopStyleColor(2);
    ImGui::End();if(!visible)Toggle(false);
  }
  ImGui::Render();
}
}
