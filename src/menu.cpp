#include "menu.h"
#include "xess_provider.h"
#include "experiment_metrics.h"
#include "core_diagnostics.h"
#include "camera_frame.h"
#include "render_reprojection.h"
#include "message_input.h"
#include "menu_hook_transaction.h"
#include "log.h"
#include "config.h"
#include "turing_unlock.h"
#include "ampere_boot.h"
#include "fps_meter.h"
#include "update_check.h"
#include "runtime_errors.h"
#include "slboot.h"
#include "native_restart.h"
#include "preset_selection.h"
#include "fg_preset.h"
#include "menu_error_preview.h"
#include "../vendor/intel-xess/inc/xess_fg/xefg_swapchain.h"
#include <cmath>
#include <windows.h>
#include <detours.h>
#include <atomic>
#include <mutex>
#include <array>
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace fgvk {
static std::atomic<uint32_t> request{0},runtime{2},maximum{1},status{0};
static std::atomic<float> ratio{1},renderedFPS{0},displayedFPS{0};
static std::atomic<ULONGLONG> lastSample{0};
static std::atomic<bool> displayedValid{false},generationActive{false},open{false};
struct MenuFonts {ImFont *body{},*bold{},*small{},*caption{},*title{},*heading{},*update{};};
static MenuFonts fontSizes[3];static ImFont* fpsFont{};
static ImGuiStyle unscaledStyle;static int scaleIndex=0;
static ImFont* boldFont{};
static bool devForceUpdate{},devVramWarning{},devTelemetryWarning{};
static MenuErrorPreview simulatedErrors;
static ImFont *smallFont{},*captionFont{},*titleFont{},*headingFont{},*updateFont{};
static ImVec4 HTMLColor(unsigned rgb,float alpha=1.0f){return {float((rgb>>16)&255)/255,float((rgb>>8)&255)/255,float(rgb&255)/255,alpha};}
static void SmallNote(const char* text){ImGui::PushFont(smallFont);ImGui::PushTextWrapPos(0);ImGui::TextDisabled("%s",text);ImGui::PopTextWrapPos();ImGui::PopFont();}

static const char* ErrorDescription(RuntimeErrors::Code code){return RuntimeErrors::Description(code);}
static std::atomic<bool> rendererAvailable{false};
static FPSMeter fpsMeter;
static const char* XeSSResultName(int32_t code){
  switch(code){
  case XEFG_SWAPCHAIN_RESULT_SUCCESS:return "OK";
  case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES:return "HISTORY NOT READY";
  case XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH:return "FRAME ID MISMATCH";
  case XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS:return "STATUS NOT READY";
  case XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH:return "INPUT SIZE MISMATCH";
  case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES:return "INSUFFICIENT INPUT";
  default:return code<0?"SDK ERROR":"SDK WARNING";
  }
}
static std::atomic<HWND> presentationWindow{nullptr};
void MenuBindWindow(void* window){presentationWindow.store(static_cast<HWND>(window));}
static HWND game{};static HHOOK messageHook{};static bool initialized{},showFPS{};
static int nextProvider{};static int fpsCapSetting{};static float menuScale{1};static bool scaleChanged{true};static std::atomic<bool> captureMenuKey{false};
static void ApplyMenuScale(){
  if(!scaleChanged)return;
  menuScale=scaleIndex==1?1.25f:scaleIndex==2?1.5f:1.0f;
  ImGui::GetStyle()=unscaledStyle;ImGui::GetStyle().ScaleAllSizes(menuScale);
  auto& f=fontSizes[scaleIndex];ImGui::GetIO().FontDefault=f.body;ImGui::GetIO().FontGlobalScale=1.0f;
  boldFont=f.bold;smallFont=f.small;captionFont=f.caption;titleFont=f.title;headingFont=f.heading;updateFont=f.update;
  scaleChanged=false;
}
struct Message {UINT id;WPARAM w;LPARAM l;};
static std::mutex inputMutex;
static std::array<Message,256> messages{};static size_t messageCount=0;
static void QueueInput(UINT id,WPARAM w,LPARAM l){
  std::lock_guard<std::mutex> lock(inputMutex);
  if(id==WM_MOUSEMOVE && messageCount && messages[messageCount-1].id==WM_MOUSEMOVE){messages[messageCount-1]={id,w,l};return;}
  if(messageCount<messages.size())messages[messageCount++]={id,w,l};
}
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
  if(value)StartUpdateCheck(true);
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
static LRESULT ProcessGameMessage(HWND w,UINT m,WPARAM a,LPARAM b){
  if(captureMenuKey && m==WM_KEYDOWN){if((int)a!=VK_ESCAPE)SaveMenuKey((int)a);captureMenuKey=false;return 0;}
  if(m==WM_KEYDOWN && (int)a==Rt().menuKey.load() && rendererAvailable){if(!(b&(1LL<<30)))Toggle(!open);return 0;}
  if(m==WM_KEYUP && (int)a==Rt().menuKey.load())return 0;
  if(m==WM_KILLFOCUS)Toggle(false);
  if(m==WM_KILLFOCUS || m==WM_SETFOCUS)QueueInput(m,a,b);
  if(m==WM_MOUSEMOVE || (m>=WM_LBUTTONDOWN && m<=WM_MOUSEHWHEEL) || m==WM_KEYDOWN || m==WM_KEYUP || m==WM_CHAR || m==WM_SYSKEYDOWN || m==WM_SYSKEYUP){
    if(open){QueueInput(m,a,b);return 0;}
  }
  if(open && m==WM_INPUT){DefWindowProcW(w,m,a,b);return 0;}
  return 1;
}
static BOOL CALLBACK FindGame(HWND w,LPARAM out){DWORD pid{};GetWindowThreadProcessId(w,&pid);wchar_t title[128]{};GetWindowTextW(w,title,128);if(pid==GetCurrentProcessId() && wcscmp(title,L"Red Dead Redemption 2")==0){*reinterpret_cast<HWND*>(out)=w;return FALSE;}return TRUE;}
static BOOL CALLBACK FindFallbackGame(HWND w,LPARAM out){
  DWORD process{};GetWindowThreadProcessId(w,&process);
  if(process!=GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w,GW_OWNER))return TRUE;
  RECT size{},previous{};GetClientRect(w,&size);auto& best=*reinterpret_cast<HWND*>(out);if(best)GetClientRect(best,&previous);
  if(size.right>=640 && size.bottom>=360 && (long long)size.right*size.bottom>(long long)previous.right*previous.bottom)best=w;
  return TRUE;
}
bool MenuIsOpen(){return open;}
void MenuRendererAvailable(bool available){rendererAvailable=available;if(!available)Toggle(false);}
void StartMenu(){}
void SetFPSGenerationActive(bool active){if(generationActive.exchange(active)!=active){fpsMeter.ResetDisplayed();displayedValid=false;}}
void RecordDisplayedFrames(uint32_t n){fpsMeter.AddDisplayed(n);}
void RecordGamePresent(){experiment::RecordPresent();auto now=GetTickCount64();if(fpsMeter.Present(now)){renderedFPS=fpsMeter.realFPS;displayedFPS=fpsMeter.outputFPS;displayedValid=fpsMeter.outputValid;lastSample=now;}}
void UpdateMenuRuntime(bool off,uint32_t frames){runtime=(frames<<1)|(off?1:0);}
void UpdateMenuStats(uint32_t max,uint32_t s,float r){maximum=max;status=s;ratio=r;}
bool ConsumeMenuRequest(bool& off,uint32_t& frames){auto r=request.exchange(0);if(!r)return false;off=(r&1)!=0;frames=(r>>1)&63;return true;}
bool MenuInitializeUI(){
  HWND selected=presentationWindow.load();DWORD pid{};
  if(!selected || !IsWindow(selected) || !GetWindowThreadProcessId(selected,&pid) || pid!=GetCurrentProcessId())selected=nullptr;
  if(initialized && (!selected || selected==game) && IsWindow(game))return true;
  if(initialized){
    // A fullscreen/settings transition can replace HWND or its input thread.
    // Rebind only the platform backend; font and renderer objects stay intact.
    Toggle(false);
    if(messageHook)UnhookWindowsHookEx(messageHook);
    messageHook=nullptr;menu_input::hook=nullptr;menu_input::target=nullptr;menu_input::handler=nullptr;
    if(ImGui::GetIO().BackendPlatformUserData)ImGui_ImplWin32_Shutdown();
    game=selected;if(!game)EnumWindows(FindGame,reinterpret_cast<LPARAM>(&game));
    if(!game)EnumWindows(FindFallbackGame,reinterpret_cast<LPARAM>(&game));
    if(!game)return false;
    if(!ImGui_ImplWin32_Init(game))return false;
    HMODULE self{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&ProcessGameMessage),&self);
    messageHook=menu_input::Install(game,self,ProcessGameMessage)?menu_input::hook:nullptr;
    if(messageHook)RuntimeErrors::ClearError(RuntimeErrors::Code::MenuInput);
    else RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInput,"Window changed; polling input fallback active",int(GetLastError()));
    Log("Menu input rebound to presentation HWND=%p thread=%lu",game,GetWindowThreadProcessId(game,nullptr));return true;
  }
  game=selected;
  if(!game)EnumWindows(FindGame,reinterpret_cast<LPARAM>(&game));
  if(!game)EnumWindows(FindFallbackGame,reinterpret_cast<LPARAM>(&game));
  if(!game){static bool noted=false;if(!noted){noted=true;Log("Menu waiting: game window with expected title not found; pid=%lu",GetCurrentProcessId());}return false;}
  IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
  // Pre-rasterize every supported size. Changing UI scale never stretches a small atlas.
  ImFontGlyphRangesBuilder glyphBuilder;glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
  glyphBuilder.AddChar(0x2197);glyphBuilder.AddChar(0x00B7);glyphBuilder.AddChar(0x26A0);
  static ImVector<ImWchar> glyphRanges;glyphBuilder.BuildRanges(&glyphRanges);
  wchar_t windowsPath[MAX_PATH]{};GetWindowsDirectoryW(windowsPath,MAX_PATH);
  char regular[MAX_PATH]{},bold[MAX_PATH]{};
  snprintf(regular,sizeof(regular),"%ls\\Fonts\\segoeui.ttf",windowsPath);
  snprintf(bold,sizeof(bold),"%ls\\Fonts\\segoeuib.ttf",windowsPath);
  auto load=[&](const char* path,float size){
    ImFontConfig cfg;cfg.SizePixels=std::round(size);cfg.OversampleH=1;cfg.OversampleV=1;cfg.PixelSnapH=true;cfg.GlyphRanges=glyphRanges.Data;
    ImFont* font=nullptr;if(GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES)font=io.Fonts->AddFontFromFileTTF(path,cfg.SizePixels,&cfg);
    return font?font:io.Fonts->AddFontDefault(&cfg);
  };
  for(int i=0;i<3;++i){float scale=1.25f*(i==1?1.25f:i==2?1.5f:1.0f);auto& f=fontSizes[i];
    f.body=load(regular,16*scale);f.bold=load(bold,16*scale);f.small=load(regular,13*scale);
    f.caption=load(regular,12*scale);f.title=load(regular,14*scale);f.heading=load(bold,15*scale);f.update=load(bold,14*scale);
  }
  fpsFont=load(regular,18); // FPS counter at 1.5x the original 12px
  ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();
  style.WindowPadding={16,14};style.FramePadding={10,7};style.ItemSpacing={8,8};style.ItemInnerSpacing={12,4};
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
  // Colors and geometry are taken from menu-preview-0.35.html.
  colors[ImGuiCol_WindowBg]=HTMLColor(0x0b0f16);colors[ImGuiCol_ChildBg]=HTMLColor(0x131a23);
  colors[ImGuiCol_TitleBg]=colors[ImGuiCol_TitleBgActive]=HTMLColor(0x121c29);
  colors[ImGuiCol_Border]=HTMLColor(0x408db5,97.0f/255);colors[ImGuiCol_Separator]=HTMLColor(0x408db5,72.0f/255);
  colors[ImGuiCol_Text]=HTMLColor(0xebf2fa);colors[ImGuiCol_TextDisabled]=HTMLColor(0xb3c4d9);
  colors[ImGuiCol_FrameBg]=HTMLColor(0x1c2938);colors[ImGuiCol_Button]=HTMLColor(0x213447);
  colors[ImGuiCol_ButtonHovered]=HTMLColor(0x2c5376);colors[ImGuiCol_Header]=HTMLColor(0x1f405c);
  colors[ImGuiCol_CheckMark]=colors[ImGuiCol_SliderGrab]=HTMLColor(0x59c7ff);
  unscaledStyle=style;
  const float configuredScale=LoadMenuScale();scaleIndex=configuredScale>=1.375f?2:configuredScale>=1.125f?1:0;scaleChanged=true;ApplyMenuScale();
  io.ConfigWindowsMoveFromTitleBarOnly=true;
  if(!ImGui_ImplWin32_Init(game)){ImGui::DestroyContext();return false;}
  HMODULE self{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&ProcessGameMessage),&self);
  const DWORD thread=GetWindowThreadProcessId(game,nullptr);
  messageHook=menu_input::Install(game,self,ProcessGameMessage)?menu_input::hook:nullptr;
  if(!messageHook){const auto error=GetLastError();RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInput,"WH_GETMESSAGE installation failed; keyboard/mouse polling enabled",(int)error);Log("Menu input hook failed: error=%lu thread=%lu; polling fallback active",error,thread);}
  else {RuntimeErrors::ClearError(RuntimeErrors::Code::MenuInput);Log("Menu input: WH_GETMESSAGE thread=%lu; game WndProc unchanged",thread);}
  static bool cursorAttempted=false;
  if(!cursorAttempted){
    cursorAttempted=true;LONG result;
    {MenuHookTransaction transaction;result=transaction.Begin();
      if(result==NO_ERROR)result=transaction.Attach((PVOID*)&originalSetCursorPos,(PVOID)CursorPosition);
      if(result==NO_ERROR)result=transaction.Attach((PVOID*)&originalClipCursor,(PVOID)CursorClip);
      if(result==NO_ERROR)result=transaction.Enlist();
      if(result==NO_ERROR)result=transaction.Commit();
    }
    if(result!=NO_ERROR){RuntimeErrors::ReportError(RuntimeErrors::Code::CursorHook,"Optional cursor detours were not committed; original cursor APIs retained",(int)result);Log("Menu cursor hooks declined safely: result=%ld",result);}
    else RuntimeErrors::ClearError(RuntimeErrors::Code::CursorHook);
  }
  showFPS=LoadFPSCounter();fpsCapSetting=(int)LoadFPSCap();Rt().fpsCap=(uint32_t)fpsCapSetting;experiment::SetEnabled(Cfg().diagnostics);SetCoreIntervalTracing(Cfg().intervalLogging);ImGui::GetIO().FontGlobalScale=1.0f;nextProvider=Cfg().provider==FGProvider::XeSS?1:0;initialized=true;
  RuntimeErrors::ClearError(RuntimeErrors::Code::MenuInit);StartUpdateCheck();StartFGPresetMonitor();
  Log("Menu ready: native Vulkan overlay, Insert toggles game UI; no secondary window");return true;
}
static void PollFallbackInput(){
  if(messageHook || !initialized)return;
  static bool keyDown=false;
  const bool foreground=GetForegroundWindow()==game;
  const int key=Rt().menuKey.load();const bool down=foreground && (GetAsyncKeyState(key)&0x8000)!=0;
  if(down && !keyDown && rendererAvailable)Toggle(!open);keyDown=down;
  if(!foreground){Toggle(false);return;}
  if(captureMenuKey){
    for(int k=VK_BACK;k<256;++k)if(k!=key && (GetAsyncKeyState(k)&0x8000)){if(k!=VK_ESCAPE)SaveMenuKey(k);captureMenuKey=false;break;}
  }
  const int buttons[]={VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
  static bool pressed[5]{};
  for(int i=0;i<5;++i){const bool current=open && (GetAsyncKeyState(buttons[i])&0x8000)!=0;if(current!=pressed[i])ImGui::GetIO().AddMouseButtonEvent(i,current);pressed[i]=current;}
}
bool MenuHasDrawing(){
  const auto nested=menu_input::nestedCallbacks.exchange(0,std::memory_order_relaxed);
  if(nested){RuntimeErrors::ReportError(RuntimeErrors::Code::MenuInputRecursion,"Nested WH_GETMESSAGE callbacks bypassed; host messages preserved",(int)nested);Log("UI-105: nested menu input callbacks bypassed=%u; game WndProc unchanged",nested);}
  PollFallbackInput();return open || showFPS;
}
void MenuResetUI(){
  MenuRendererAvailable(false);
  if(messageHook)UnhookWindowsHookEx(messageHook);
  messageHook=nullptr;menu_input::hook=nullptr;menu_input::target=nullptr;menu_input::handler=nullptr;
  if(ImGui::GetCurrentContext()){
    if(ImGui::GetIO().BackendPlatformUserData)ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
  }
  initialized=false;scaleChanged=true;
  std::lock_guard<std::mutex> lock(inputMutex);messageCount=0;
}
void MenuBuildUI(){
  StartUpdateCheck(); // Background request only when due; never waits for network.
  if(open && GetForegroundWindow()!=game)Toggle(false);
  std::array<Message,256> pending;size_t pendingCount=0;
  {std::lock_guard<std::mutex> lock(inputMutex);pendingCount=messageCount;std::copy_n(messages.begin(),pendingCount,pending.begin());messageCount=0;}
  for(size_t i=0;i<pendingCount;++i){auto& m=pending[i];ImGui_ImplWin32_WndProcHandler(game,m.id,m.w,m.l);}
  ApplyMenuScale();
  ImGui_ImplWin32_NewFrame();ImGui::GetIO().MouseDrawCursor=open;ImGui::NewFrame();
  if(showFPS){
    ImGui::PushFont(fpsFont);
    char text[64]{};bool fresh=GetTickCount64()-lastSample.load()<2000;float real=renderedFPS,output=displayedFPS;
    if(!fresh)snprintf(text,sizeof(text),"-- FPS");
    else if(displayedValid && output>real*1.15f && !Rt().fgUserOff)snprintf(text,sizeof(text),"%.0f / %.0f FPS",real,output);
    else if(!displayedValid && generationActive && !Rt().fgUserOff)snprintf(text,sizeof(text),"%.0f / -- FPS",real);
    else snprintf(text,sizeof(text),"%.0f FPS",real);
    auto* draw=ImGui::GetForegroundDrawList();auto size=ImGui::CalcTextSize(text);ImVec2 p{std::floor(ImGui::GetIO().DisplaySize.x-size.x-16),14};
    draw->AddRectFilled({p.x-6,p.y-3},{p.x+size.x+6,p.y+size.y+3},IM_COL32(5,10,17,195),8);
    draw->AddRect({p.x-6,p.y-3},{p.x+size.x+6,p.y+size.y+3},IM_COL32(90,180,240,85),8);
    draw->AddText({p.x+1,p.y+1},IM_COL32(0,0,0,230),text);draw->AddText(p,IM_COL32(240,245,250,255),text);ImGui::PopFont();
  }
  if(open){
    const float menuWidth=(std::min)(600*menuScale,ImGui::GetIO().DisplaySize.x-32);
    ImGui::SetNextWindowPos({std::floor((ImGui::GetIO().DisplaySize.x-menuWidth)/2),std::floor((ImGui::GetIO().DisplaySize.y-510*menuScale)/2)},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({menuWidth,0},{menuWidth,ImGui::GetIO().DisplaySize.y-32});
    ImGui::PushFont(titleFont);
    bool visible=true;ImGui::Begin("RDR2 Vulkan Frame Generation - 0.82",&visible,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopFont();
    ImGui::SetWindowFontScale(1.0f);
    if(headingFont)ImGui::PushFont(headingFont);
    ImGui::TextColored(HTMLColor(0x6bccff),"RDR2 VULKAN FRAME GENERATION");
    if(headingFont)ImGui::PopFont();

    ImGui::TextWrapped("%s",GPUName());
    SmallNote("Red Dead Redemption 2 | Vulkan");
    ImGui::Separator();
    if(GetUpdateState()==UpdateState::Available || (Cfg().developerMenuTest && devForceUpdate)){
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*menuScale,6*menuScale});
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,8*menuScale);
      ImGui::PushStyleColor(ImGuiCol_ChildBg,HTMLColor(0x211c12));
      ImGui::BeginChild("UpdateNotice",{0,40*menuScale},ImGuiChildFlags_AlwaysUseWindowPadding);
      auto screen=ImGui::GetWindowPos();auto extent=ImGui::GetWindowSize();
      ImGui::GetWindowDrawList()->AddRectFilledMultiColor(screen,{screen.x+extent.x,screen.y+extent.y},IM_COL32(57,45,18,136),IM_COL32(33,28,18,102),IM_COL32(33,28,18,102),IM_COL32(57,45,18,136));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{8*menuScale,5*menuScale});
      ImGui::PushFont(titleFont);ImGui::AlignTextToFramePadding();ImGui::TextColored(HTMLColor(0xffe199),"Update available");ImGui::PopFont();
      ImGui::SameLine(0,10*menuScale);
      ImGui::PushFont(smallFont);ImGui::TextColored(HTMLColor(0xcdbb91),"%s \xC2\xB7 Nexus Mods",LatestUpdateVersion()[0]?LatestUpdateVersion():"9.99 (test)");ImGui::PopFont();
      ImGui::SameLine();ImGui::SetCursorPosX(extent.x-130*menuScale);
      ImGui::PushFont(smallFont);ImGui::PushStyleColor(ImGuiCol_Button,{0,0,0,0});
      ImGui::PushStyleColor(ImGuiCol_Border,HTMLColor(0xbd954f,102.0f/255));ImGui::PushStyleColor(ImGuiCol_Text,HTMLColor(0xf3ce7b));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1.0f);
      ImGui::BeginDisabled(UpdateBrowserOpening());if(ImGui::Button("View update \xE2\x86\x97",{118*menuScale,28*menuScale}))OpenUpdateDownload(Cfg().developerMenuTest && devForceUpdate);ImGui::EndDisabled();
      ImGui::PopStyleVar();ImGui::PopStyleColor(3);ImGui::PopFont();
      ImGui::PopStyleVar();
      ImGui::EndChild();
      {auto mn=ImGui::GetItemRectMin(),mx=ImGui::GetItemRectMax();ImGui::GetWindowDrawList()->AddRectFilled(mn,{mn.x+3,mx.y},IM_COL32(237,191,83,255),2);}
      ImGui::PopStyleColor();ImGui::PopStyleVar(2);
      if(UpdateBrowserFailed())SmallNote("Could not open browser");
    }
    auto errors=RuntimeErrors::SnapshotErrors();
    if(Cfg().developerMenuTest)errors=simulatedErrors.Merge(errors);
    for(size_t i=0;i<errors.count;++i){const auto& error=errors.entries[i];
      if(error.code==RuntimeErrors::Code::CursorHook){
        ImGui::PushFont(smallFont);
        ImGui::TextColored({.91f,.78f,.48f,1},"%s: optional cursor routing unavailable",error.id);
        if(ImGui::IsItemHovered()){ImGui::BeginTooltip();ImGui::TextWrapped("%s (Windows %d). Frame generation is independent.",error.detail.data(),error.native);ImGui::EndTooltip();}
        ImGui::PopFont();continue;
      }
      ImGui::PushID((int)error.code);
      ImGui::PushStyleColor(ImGuiCol_ChildBg,HTMLColor(0x2b161a));
      ImGui::PushStyleColor(ImGuiCol_Border,HTMLColor(0xd75d68,136.0f/255));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*menuScale,8*menuScale});
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,8*menuScale);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8*menuScale,4*menuScale});
      ImGui::BeginChild("ErrorCard",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
      ImGui::PopStyleVar(3);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8*menuScale,4*menuScale});
      if(boldFont)ImGui::PushFont(boldFont);
      ImGui::TextColored({1,.57f,.61f,1},"\xE2\x9A\xA0  %s",error.title);
      if(boldFont)ImGui::PopFont();
      ImGui::PushFont(smallFont);
      ImGui::SameLine();ImGui::SetCursorPosX(ImGui::GetWindowWidth()-ImGui::CalcTextSize(error.id).x-30*menuScale);
      ImGui::PushStyleColor(ImGuiCol_Button,ImVec4{.65f,.21f,.27f,.35f});
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1.0f);
      ImGui::SmallButton(error.id);ImGui::PopStyleVar();ImGui::PopStyleColor();ImGui::PopFont();
      ImGui::TextWrapped("%s",ErrorDescription(error.code));
      ImGui::PushFont(smallFont);
      if(ImGui::TreeNode("Error details and next steps")){
        ImGui::PushTextWrapPos(0);ImGui::TextColored(HTMLColor(0xd4a5aa),"%s",RuntimeErrors::AffectsGeneration(error.code)?"Check generation status and details below.":"This affects the menu; frame generation is managed independently.");ImGui::PopTextWrapPos();
        ImGui::TextWrapped("%s",error.action);
        ImGui::TextWrapped("%s",error.detail.data());
        ImGui::Text("Native result: %d | Occurrences: %llu",error.native,(unsigned long long)error.count);
        ImGui::TextWrapped("Include RDR2VulkanFG.log in your report.");ImGui::TreePop();
      }
      ImGui::PopFont();ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor(2);ImGui::PopID();
    }
    bool generationError=false;for(size_t i=0;i<errors.count;++i)generationError|=RuntimeErrors::AffectsGeneration(errors.entries[i].code);
    const auto fg=GetFGSnapshot();const auto warnings=RuntimeErrors::GetWarnings() | (Cfg().developerMenuTest && devVramWarning?1u:0u) | (Cfg().developerMenuTest && devTelemetryWarning?2u:0u);
    ImGui::PushFont(captionFont);ImGui::PushTextWrapPos(0);
    if(warnings & (uint32_t)RuntimeErrors::Warning::VRAM)ImGui::TextColored({.91f,.78f,.48f,1},"VRAM budget exceeded. Consider reducing GPU memory usage.");
    if(warnings & (uint32_t)RuntimeErrors::Warning::Telemetry)ImGui::TextColored({.91f,.78f,.48f,1},"Displayed FPS measurement unavailable. See Details and log.");
    ImGui::PopTextWrapPos();ImGui::PopFont();
    auto pendingRequest=request.load();auto r=pendingRequest?(pendingRequest&127):runtime.load();bool enabled=(r&1)==0;int mode=int(r>>1)-1;
    ImGui::PushFont(smallFont);ImGui::TextColored(HTMLColor(0x6bccff),"GENERATION");ImGui::PopFont();ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{14*menuScale,12*menuScale});
      ImGui::BeginChild("GenerationCard",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
      ImGui::PopStyleVar();
    if(ImGui::Checkbox("Enable frame generation",&enabled))request=0x100|((mode+1)<<1)|(enabled?0:1);
    const char* stateText=!enabled?"OFF":generationError?"ERROR":Cfg().provider!=FGProvider::DLSSG?(generationActive?(displayedValid?"GENERATING":"MEASUREMENT UNAVAILABLE"):"WAITING"):
      fg.state==FGState::Generating?"GENERATING":fg.state==FGState::Error?"ERROR":fg.state==FGState::EnabledUnmeasured?(fg.measuredRatio>=.99f?"ENABLED - NO GAIN MEASURED":(warnings&(unsigned)RuntimeErrors::Warning::Telemetry)?"MEASUREMENT UNAVAILABLE":"MEASURING"):"WAITING";
    if(ActiveProvider()==FGProvider::XeSS && enabled && !generationError)stateText=generationActive.load()?(ratio.load()>1.f?"GENERATING":status.load()?XeSSResultName((int32_t)status.load()):"ENABLED - NATIVE FRAME"):"WAITING FOR INPUTS";
    ImGui::PushFont(captionFont);
    const float statusX=ImGui::GetWindowWidth()-ImGui::CalcTextSize(stateText).x-14*menuScale;
    if(statusX>ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x+12*menuScale){ImGui::SameLine();ImGui::SetCursorPosX(statusX);}
    ImGui::TextColored(!enabled?ImVec4{.7f,.77f,.85f,1}:generationError?ImVec4{1,.62f,.35f,1}:(stateText[0]=='G'?ImVec4{.4f,.87f,.59f,1}:ImVec4{.91f,.78f,.48f,1}),"%s",stateText);
    ImGui::PopFont();
    const char* providers[]={"DLSS-G","XeSS FG","Off"};
    const bool xess=ActiveProvider()==FGProvider::XeSS,bridge=ActiveProvider()==FGProvider::DLSSGBridge;
    const bool fsr=false,legacy=ActiveProvider()==FGProvider::DLSSG;
    const int currentProvider=xess?1:0;int selection=!enabled?2:nextProvider;
    ImGui::SetNextItemWidth((std::min)(310*menuScale,ImGui::GetContentRegionAvail().x*.68f));
    if(ImGui::Combo("Provider",&selection,providers,3)){
      if(selection==2)request=0x100|((mode+1)<<1)|1;
      else{nextProvider=selection;SaveNextProvider(selection==1?FGProvider::XeSS:FGProvider::DLSSGBridge);if(selection==currentProvider)request=0x100|((mode+1)<<1);}
    }
    const bool providerNeedsGameRestart=nextProvider!=currentProvider && (legacy || (Cfg().nativeDLSSGBackend && nextProvider==0));
    ImGui::PushFont(smallFont);
    if(nextProvider!=currentProvider){ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.76f,.3f,1));ImGui::TextWrapped(providerNeedsGameRestart?"Restart game to apply %s":"Restart FG below to apply %s",providers[nextProvider]);ImGui::PopStyleColor();}else ImGui::TextDisabled("Selected: %s",providers[currentProvider]);
    ImGui::PopFont();
    if((legacy || bridge) && nextProvider==currentProvider && enabled){
      static PresetSelection presetSelection(ReadDLSSGPreset());
      presetSelection.Applied(Rt().appliedPreset.load());
      int requestedPreset=presetSelection.Saved();
      const char* presetChoices[]={"NVIDIA profile / default","Preset A","Preset B","Preset C"};
      if(ImGui::Combo("Preset",&requestedPreset,presetChoices,4)){SaveDLSSGPreset(requestedPreset);presetSelection.Save(requestedPreset);}
      if(presetSelection.Pending()){ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,.76f,.3f,1.f));ImGui::TextWrapped(TuringUnlock040LoadedPreset()>=0?"Preset saved. Restart the game to apply this RTX 20 preset.":"Preset saved. Restart FG below to apply; confirm effective preset in the feedback below.");ImGui::PopStyleColor();}
      if(ImGui::TreeNode("Image stability")){
        float threshold=Rt().depthSeparation.load(std::memory_order_relaxed);
        ImGui::SetNextItemWidth(160*menuScale);
        if(ImGui::SliderFloat("Depth separation (Experimental)",&threshold,1.f,80.f,"%.1f",ImGuiSliderFlags_Logarithmic))Rt().depthSeparation=threshold;
        if(ImGui::IsItemDeactivatedAfterEdit())SaveDepthSeparation(threshold);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("NVIDIA's linear-depth separation heuristic. Smaller values distinguish closer depth surfaces.\nThis is a tuning experiment, not a guaranteed anti-ghosting fix. Applies to the next FG inputs.");
        SmallNote("Existing baseline: 10. NVIDIA default: 40. Compare visually before keeping a change.");
        ImGui::TreePop();
      }
      const auto preset=GetFGPresetSnapshot();char name[32]{};ImGui::PushFont(smallFont);
      if(preset.profileResult==0 && preset.profilePreset)ImGui::Text("NVIDIA profile: %s (requested)",FGPresetName(preset.profilePreset,name,sizeof(name)));
      else ImGui::TextDisabled("NVIDIA profile: no preset override reported");
      ImGui::Text("Loaded DLSS-G: %s",preset.dllVersion[0]?preset.dllVersion:"not loaded");
      if(preset.providerRead)ImGui::Text("DLSS-G preset: %s (provider read)",FGPresetName(preset.providerPreset,name,sizeof(name)));
      else if(preset.reported)ImGui::Text("DLSS-G preset: %s (driver)",FGPresetName(preset.preset,name,sizeof(name)));
      else ImGui::TextDisabled("DLSS-G preset: Not reported by driver");
      if(ImGui::IsItemHovered())ImGui::SetTooltip("Read-only NVIDIA runtime feedback, refreshed every 2 seconds.\nProfile changes may require restarting the game. No preset is inferred.");
      ImGui::PopFont();
    }
    if(IsDX12Bridge(ActiveProvider())){
      const bool pending=BridgeRestartPending();ImGui::BeginDisabled(pending || providerNeedsGameRestart);
      if(ImGui::Button(pending?"Restarting FG...":"Restart FG / Apply changes")){
        const auto selected=nextProvider==1?FGProvider::XeSS:FGProvider::DLSSGBridge;
        if(nextProvider!=currentProvider)request=0x100|((mode+1)<<1);
        RequestBridgeRestart(selected,ReadDLSSGPreset());
      }
      ImGui::EndDisabled();
      if(ImGui::IsItemHovered())ImGui::SetTooltip("Rebuilds the FG context and presentation through the game's normal swapchain recovery.\nApplies saved provider/preset settings. Brief recovery is expected; GPU/driver failures may require restarting the game.");
    }
    if(legacy){
      const bool pending=NativeRestartPending();ImGui::BeginDisabled(pending || providerNeedsGameRestart);
      if(ImGui::Button(pending?"Restarting FG...":"Restart FG / Apply changes"))RequestNativeRestart(unsigned(ReadDLSSGPreset()));
      ImGui::EndDisabled();
      if(ImGui::IsItemHovered())ImGui::SetTooltip("Resets Native DLSS-G resources and applies the saved preset.\nA brief pause is expected. Provider changes require restarting the game.");
    }
    const int modeCount=fsr?1:int((std::min)(xess?3u:IsDX12Bridge(Cfg().provider)?32u:5u,(std::max)(1u,maximum.load())));
    mode=(std::max)(0,(std::min)(mode,modeCount-1));
    ImGui::SetNextItemWidth((std::min)(310*menuScale,ImGui::GetContentRegionAvail().x*.68f));
    char selectedMode[32]{};snprintf(selectedMode,sizeof(selectedMode),xess&&mode>0?"%dx (Experimental)":"%dx",mode+2);
    ImGui::BeginDisabled(fsr);
    if(ImGui::BeginCombo("Multiplier",selectedMode)){
      for(int i=0;i<modeCount;++i){char label[32]{};snprintf(label,sizeof(label),xess&&i>0?"%dx (Experimental)":"%dx",i+2);if(ImGui::Selectable(label,mode==i)){mode=i;request=0x100|((mode+1)<<1)|(enabled?0:1);}if(mode==i)ImGui::SetItemDefaultFocus();}
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if(IsDX12Bridge(Cfg().provider))ImGui::TextDisabled("Runtime-reported maximum: %dx",modeCount+1);
    SmallNote("Opening this menu does not pause frame generation.");
    ImGui::EndChild();
    ImGui::Spacing();ImGui::PushFont(smallFont);ImGui::TextColored(HTMLColor(0x6bccff),"DISPLAY");ImGui::PopFont();ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{14*menuScale,12*menuScale});
      ImGui::BeginChild("DisplayCard",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
      ImGui::PopStyleVar();
    if(ImGui::Checkbox("Show FPS counter",&showFPS))SaveFPSCounter(showFPS);
    ImGui::SetNextItemWidth((std::min)(310*menuScale,ImGui::GetContentRegionAvail().x*.68f));if(ImGui::SliderInt("FPS limit",&fpsCapSetting,0,240,"%d")){if(fpsCapSetting<0)fpsCapSetting=0;SaveFPSCap((uint32_t)fpsCapSetting);}
    if(fpsCapSetting==0)ImGui::SameLine(),ImGui::TextDisabled("Unlimited");
    const char* scales[]={"100%","125%","150%"};ImGui::SetNextItemWidth(150*menuScale);
    if(ImGui::Combo("Menu scale",&scaleIndex,scales,3)){scaleChanged=true;SaveMenuScale(scaleIndex==1?1.25f:scaleIndex==2?1.5f:1.0f);}
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::PushFont(titleFont);
    const bool detailsOpen=ImGui::CollapsingHeader("Details");ImGui::PopFont();
    if(detailsOpen){
      ImGui::PushFont(smallFont);
      ImGui::TextDisabled("State");ImGui::SameLine(170*menuScale);ImGui::Text("%s",stateText);
      if(xess){ImGui::TextDisabled("XeSS SDK result");ImGui::SameLine(170*menuScale);ImGui::Text("%d (%s)",(int32_t)status.load(),XeSSResultName((int32_t)status.load()));}
      else if(legacy || fsr){ImGui::TextDisabled("Provider results");ImGui::SameLine(170*menuScale);ImGui::Text("Options %d / State %d",fg.optionsResult,fg.queryResult);
      ImGui::TextDisabled("State queries");ImGui::SameLine(170*menuScale);ImGui::Text("%llu OK / %llu warn / %llu failed",(unsigned long long)fg.querySuccess,(unsigned long long)fg.queryWarnings,(unsigned long long)fg.queryErrors);}
      if(bridge){ImGui::TextDisabled("Bridge result");ImGui::SameLine(170*menuScale);ImGui::Text("%d",(int32_t)status.load());}
      if(legacy){const auto preset=GetFGPresetSnapshot();ImGui::TextDisabled("Preset query");ImGui::SameLine(170*menuScale);ImGui::Text("NVAPI %d / flags 0x%llx",preset.result,(unsigned long long)preset.feedback);}
      ImGui::TextDisabled("Latest presented");ImGui::SameLine(170*menuScale);ImGui::Text("%u frames",IsDX12Bridge(Cfg().provider)?(uint32_t)(std::max)(0.f,ratio.load()):fg.lastPresented);
      ImGui::TextDisabled("Debug logging");ImGui::SameLine(170*menuScale);ImGui::TextUnformatted(DebugLoggingEnabled()?"Enabled":"Disabled");
      if(xess){
        ImGui::TextDisabled("Game frame rate");ImGui::SameLine(170*menuScale);ImGui::Text("%.0f FPS",renderedFPS.load());
        ImGui::TextDisabled("Output FPS (SDK)");ImGui::SameLine(170*menuScale);if(displayedValid)ImGui::Text("%.0f FPS",displayedFPS.load());else ImGui::TextUnformatted("--");
        ImGui::TextDisabled("Output source");ImGui::SameLine(170*menuScale);ImGui::TextUnformatted("XeSS queued frames");
      }else{ImGui::TextDisabled("Frame rate");ImGui::SameLine(170*menuScale);if(displayedValid)ImGui::Text("%.0f to %.0f FPS",renderedFPS.load(),displayedFPS.load());else ImGui::Text("%.0f FPS",renderedFPS.load());
        ImGui::TextDisabled("Output source");ImGui::SameLine(170*menuScale);ImGui::TextUnformatted(legacy?"Streamline Vulkan output":"DLSS-G bridge SDK output");}
      ImGui::TextDisabled("Measured ratio");ImGui::SameLine(170*menuScale);if(!legacy){if(displayedValid && renderedFPS.load()>0)ImGui::Text("%.2fx",displayedFPS.load()/renderedFPS.load());else ImGui::TextUnformatted("--");}else if(fg.measuredRatio>0)ImGui::Text("%.2fx",fg.measuredRatio);else ImGui::TextUnformatted("--");
      ImGui::TextDisabled("Active provider");ImGui::SameLine(170*menuScale);ImGui::Text("%s",providers[currentProvider]);
      ImGui::TextDisabled("FPS limit");ImGui::SameLine(170*menuScale);if(fpsCapSetting)ImGui::Text("%d FPS",fpsCapSetting);else ImGui::Text("Unlimited");
      if(!fsr){ImGui::TextDisabled("Max multiplier");ImGui::SameLine(170*menuScale);ImGui::Text("%dx",modeCount+1);}
      if(legacy && GPUFamily()==GpuFamily::RTX20)ImGui::TextWrapped("RTX 20 support is experimental and hardware untested.");
      ImGui::PopFont();
    }
    ImGui::Spacing();
    if(Cfg().developerMenuTest && ImGui::CollapsingHeader("Developer test")){
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{14*menuScale,12*menuScale});
      ImGui::BeginChild("DeveloperCard",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
      ImGui::PopStyleVar();
      ImGui::Checkbox("Simulate update notice",&devForceUpdate);
      if(ImGui::Button("Camera error")){simulatedErrors.Set(RuntimeErrors::Code::CameraMissing);}
      ImGui::SameLine();if(ImGui::Button("Input error")){simulatedErrors.Set(RuntimeErrors::Code::InputState);}
      ImGui::SameLine();if(ImGui::Button("Provider error")){simulatedErrors.Set(RuntimeErrors::Code::ProviderInit);}
      if(ImGui::Button("Present error")){simulatedErrors.Set(RuntimeErrors::Code::PresentFailed);}
      ImGui::SameLine();if(ImGui::Button("Stall error")){simulatedErrors.Set(RuntimeErrors::Code::GenerationStalled);}
      ImGui::Checkbox("VRAM warning",&devVramWarning);
      ImGui::SameLine();ImGui::Checkbox("Telemetry warning",&devTelemetryWarning);
      if(ImGui::Button("Clear simulated")){
        devForceUpdate=false;
        simulatedErrors.Clear();devVramWarning=false;devTelemetryWarning=false;
      }
      SmallNote("Simulated entries are marked as test data in the error details.");
      ImGui::EndChild();
    }
    if(captureMenuKey){ImGui::TextColored({1,.78f,.32f,1},"Press a key for the menu shortcut (Esc cancels)");}
    ImGui::PushFont(titleFont);
    char keyLabel[64]{};snprintf(keyLabel,sizeof(keyLabel),"Menu key: %s",KeyName(Rt().menuKey.load()));
    if(ImGui::Button(keyLabel,{190*menuScale,34*menuScale}))captureMenuKey=true;
    ImGui::PopFont();ImGui::PushFont(captionFont);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,{.13f,.34f,.55f,1});ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.18f,.46f,.72f,1});
    ImGui::SetCursorPosX(ImGui::GetWindowWidth()-ImGui::CalcTextSize("made by mor3a").x-16*menuScale);ImGui::TextDisabled("made by mor3a");
    ImGui::PopStyleColor(2);ImGui::PopFont();
    ImGui::End();if(!visible)Toggle(false);
  }
  ImGui::Render();
}
}

