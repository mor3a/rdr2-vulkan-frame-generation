#include "config.h"
#include "update_check.h"
#include "log.h"
#include "inimigrate.h"
#include "menu.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
namespace fgvk {
static Config g_cfg;
static Runtime g_rt;
static bool probeRequested{};
const Config& Cfg(){ return g_cfg; }
Runtime& Rt(){ return g_rt; }
static void IniPath(char* out,size_t n);
void SaveNextProvider(FGProvider provider){
  char path[MAX_PATH]{};IniPath(path,sizeof(path));
  WritePrivateProfileStringA("fgvk","FGProvider",provider==FGProvider::XeSS?"XeSS":provider==FGProvider::Off?"OFF":"DLSSG",path);
  WritePrivateProfileStringA("fgvk","UserFGOff","0",path);
}
bool LoadFPSCounter(){char path[MAX_PATH]{};IniPath(path,sizeof(path));return GetPrivateProfileIntA("fgvk","ShowFPS",0,path)!=0;}
uint32_t LoadFPSCap(){char path[MAX_PATH]{};IniPath(path,sizeof(path));int v=GetPrivateProfileIntA("fgvk","FPSCap",0,path);return v<0?0u:v>480?480u:(uint32_t)v;}
bool ReadStabilityOption(const char* key,bool fallback){char path[MAX_PATH]{};IniPath(path,sizeof(path));return GetPrivateProfileIntA("fgvk",key,fallback?1:0,path)!=0;}
void SaveStabilityOption(const char* key,bool enabled){char path[MAX_PATH]{};IniPath(path,sizeof(path));WritePrivateProfileStringA("fgvk",key,enabled?"1":"0",path);}
void SaveDepthSeparation(float value){
  if(!(value>=1.f && value<=200.f))return;
  char path[MAX_PATH]{},text[32]{};IniPath(path,sizeof(path));snprintf(text,sizeof(text),"%.2f",value);
  WritePrivateProfileStringA("fgvk","DepthSeparation",text,path);g_rt.depthSeparation=value;
  Log("Experimental depth separation changed to %.2f (NVIDIA linear-depth heuristic)",value);
}
int ReadDLSSGPreset(){char path[MAX_PATH]{};IniPath(path,sizeof(path));int v=GetPrivateProfileIntA("fgvk","DLSSGPreset",3,path);return v>=0 && v<=3?v:0;}
void SaveDLSSGPreset(int value){char path[MAX_PATH]{},text[16]{};IniPath(path,sizeof(path));snprintf(text,sizeof(text),"%d",value);WritePrivateProfileStringA("fgvk","DLSSGPreset",text,path);}
void SaveExperimentSetting(const char* key,const char* value){char path[MAX_PATH]{};IniPath(path,sizeof(path));WritePrivateProfileStringA("fgvk",key,value,path);}
void SaveOutputFPSCap(uint32_t cap){char value[16]{};cap=(std::min)(1000u,cap);snprintf(value,sizeof(value),"%u",cap);SaveExperimentSetting("OutputFPSCap",value);g_rt.outputFPSCap=cap;}
void SaveFPSCap(uint32_t cap){char path[MAX_PATH]{};IniPath(path,sizeof(path));char value[16]{};snprintf(value,sizeof(value),"%u",cap);WritePrivateProfileStringA("fgvk","FPSCap",value,path);g_rt.fpsCap=cap;}
void SaveMenuKey(int vk){char path[MAX_PATH]{};IniPath(path,sizeof(path));char value[16]{};snprintf(value,sizeof(value),"0x%X",vk);WritePrivateProfileStringA("fgvk","MenuKey",value,path);g_rt.menuKey=vk;}
float LoadMenuScale(){char path[MAX_PATH]{};IniPath(path,sizeof(path));char value[16]{};GetPrivateProfileStringA("fgvk","MenuScale","1",value,sizeof(value),path);float v=(float)atof(value);return v<0.8f?0.8f:v>1.5f?1.5f:v;}
void SaveMenuScale(float scale){char path[MAX_PATH]{};IniPath(path,sizeof(path));char value[16]{};snprintf(value,sizeof(value),"%.2f",scale);WritePrivateProfileStringA("fgvk","MenuScale",value,path);}
void SaveFPSCounter(bool enabled){char path[MAX_PATH]{};IniPath(path,sizeof(path));WritePrivateProfileStringA("fgvk","ShowFPS",enabled?"1":"0",path);}
bool PerformanceProbeRequested(){return probeRequested;}
void EndPerformanceProbe(){probeRequested=false;char path[MAX_PATH]{};IniPath(path,sizeof(path));WritePrivateProfileStringA("fgvk","BenchmarkFG","0",path);}

// Edge-triggered key polling on the present thread. GetAsyncKeyState is cheap (no message pump).
static bool Pressed(int vk, bool& was){
  if(vk<=0) return false;
  bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  bool edge = down && !was; was = down; return edge;
}
bool PollHotkeys(){
  static bool wT=false, wC=false;
  bool changed=false;
  bool off=false;uint32_t frames=1;
  if(ConsumeMenuRequest(off,frames)){
    g_rt.fgUserOff=off;g_rt.frames=frames;
    Log("menu: FG=%d mode=x%u",!off,frames+1);changed=true;
  }
  bool gameFocused=false;auto foreground=GetForegroundWindow();DWORD foregroundPid{};if(foreground)GetWindowThreadProcessId(foreground,&foregroundPid);gameFocused=foregroundPid==GetCurrentProcessId();
  bool toggle=Pressed(g_cfg.keyToggleFG,wT),cycle=Pressed(g_cfg.keyCycleFrames,wC);
  if(gameFocused && toggle){ g_rt.fgUserOff = !g_rt.fgUserOff; Log("hotkey: FG %s by user", g_rt.fgUserOff?"OFF":"ON"); changed=true; }
  if(gameFocused && cycle && IsDX12Bridge(g_cfg.provider)){auto max=g_rt.maxFrames.load();g_rt.frames=g_rt.frames>=max?1:g_rt.frames+1;Log("hotkey: generated frames -> %u (x%u)",g_rt.frames,g_rt.frames+1);changed=true;}
  if(changed){char path[MAX_PATH]{},value[16]{};IniPath(path,sizeof(path));snprintf(value,sizeof(value),"%u",g_rt.frames);WritePrivateProfileStringA("fgvk","DLSSGFrames",value,path);WritePrivateProfileStringA("fgvk","UserFGOff",g_rt.fgUserOff?"1":"0",path);}
  UpdateMenuRuntime(g_rt.fgUserOff,g_rt.frames);return changed;
}

static void IniPath(char* out, size_t n){
  HMODULE self{}; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)&IniPath,&self);
  char p[MAX_PATH]{}; GetModuleFileNameA(self,p,MAX_PATH); char* s=strrchr(p,'\\'); if(s) *(s+1)=0;
  snprintf(out,n,"%sRDR2VulkanFG.ini",p);
}

static void WriteDefaults(const char* path){
  FILE* f=nullptr; if(fopen_s(&f,path,"w")!=0 || !f) return;
  fputs(
    "; RDR2 Vulkan Frame Generation 0.82. Edit advanced settings with the game closed.\n"
    "[fgvk]\n"
    "Enabled=1\n"
    "FGProvider=DLSSG\nDLSSGBackend=Native\nCameraSource=Neutral\nCameraMaxAgeMs=50\nCameraAudit=0\nDiagnostics=0\nIntervalLogging=0\nOutputFPSCap=0\n"
    "ApplicationId=0x60E8AB7\n"
    "; Settings format; fgvk updates older files automatically. Do not edit.\n"
    "ConfigVersion=2\nDebugLogging=0\nMenuKey=0x2D\nShowFPS=1\n"
    "; Generated frames per real frame: 1 = x2 through 5 = x6 (clamped to what the GPU supports)\n"
    "DLSSGFrames=1\n"
    "; Native Reflex: 1 = low latency, 2 = low latency + boost\n"
    "ReflexMode=1\n"
    "; Native Vulkan SDK wait: 0 = default, 1 = enable; restart the game after changes\n"
    "ReflexSleep=0\n"
    "; The DLSS-SR output precedes final color processing and is transient.\n"
    "; Leave it untagged; frame generation uses the final backbuffer for color.\n"
    "TagHUDLess=0\nNativeHUDProtection=1\n"
    "; Feed a fully transparent UI color+alpha layer (0 = none)\n"
    "TagUI=0\n"
    "; While frame generation is on, wait (max 100 ms) for the frame's GPU work before marking PresentStart.\n"
    "; Smooths bursty multi-frame-generation pacing at a small fps cost. 0 = off\n"
    "PresentPacing=0\n"
    "; Frame rate limit on the present thread in frames per second (0 = unlimited). Can also be set in the menu.\n"
    "FPSCap=0\n"
    "; Motion-vector scale handed to Streamline: 1 = the game's DLSS scale divided by the motion-vector buffer size\n"
    "; (Streamline multiplies it back by that size before DLSS-G sees it). 0 = raw values, 1500x too large; A/B only.\n"
    "MvecScaleNormalized=1\n"
    "; DLSS-G turns on after this many consecutive frames with a DLSS-SR evaluate (world rendering)\n"
    "OnAfterEvalFrames=60\n"
    "; ...and suspends after this many presents without one (menu / loading screen / video)\n"
    "OffAfterIdleFrames=3\n"
    "NoDLSSGraceMs=100\n"
    "; Hotkeys as Windows virtual-key codes (decimal or 0x hex; 0 disables). 106 = numpad *, 35 = End\n"
    "KeyToggleFG=0x6A\n"
    "KeyCycleFrames=0x23\n", f);
  fclose(f);
}

void LoadConfig(){
  char path[MAX_PATH]; IniPath(path,sizeof(path));
  probeRequested=GetPrivateProfileIntA("fgvk","BenchmarkFG",0,path)!=0;
  if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES) WriteDefaults(path);
  else if(int was = MigrateIni(path); was < kIniVersion)
    Log("config: updated fgvk.ini from format %d to %d: TagHUDLess=0 (orbit flickering fix), legacy INI migration applied", was, kIniVersion);
  Config c;
  char provider[32]{};GetPrivateProfileStringA("fgvk","FGProvider","DLSSG",provider,sizeof(provider),path);
  if(_stricmp(provider,"FSR3")==0){c.provider=FGProvider::DLSSGBridge;WritePrivateProfileStringA("fgvk","FGProvider","DLSSG",path);Log("FSR3 is unavailable; migrated to DLSS-G");}
  else if(_stricmp(provider,"XeSS")==0)c.provider=FGProvider::XeSS;
  else if(_stricmp(provider,"DLSSGBridge")==0)c.provider=FGProvider::DLSSGBridge;
  else if(_stricmp(provider,"DLSSG")==0)c.provider=FGProvider::DLSSGBridge;
  else if(_stricmp(provider,"OFF")==0)c.provider=FGProvider::Off;
  else if(_stricmp(provider,"DLSSG")!=0){c.provider=FGProvider::Off;Log("Unknown FGProvider=%s; FG disabled",provider);}
  char backend[32]{},cameraSource[32]{};
  GetPrivateProfileStringA("fgvk","DLSSGBackend","Native",backend,sizeof(backend),path);
  GetPrivateProfileStringA("fgvk","CameraSource","Neutral",cameraSource,sizeof(cameraSource),path);
  c.nativeDLSSGBackend=_stricmp(backend,"Bridge")!=0;
  if(c.provider==FGProvider::DLSSGBridge && c.nativeDLSSGBackend)c.provider=FGProvider::DLSSG;
  c.experimentalRTX20=GetPrivateProfileIntA("fgvk","EnableExperimentalRTX20",0,path)!=0;
  if(c.experimentalRTX20){
    c.nativeDLSSGBackend=false;
    if(c.provider==FGProvider::DLSSG)c.provider=FGProvider::DLSSGBridge;
    Log("RTX 20 opt-in: DLSS-G uses the D3D12 FG bridge; game renderer remains Vulkan");
  }
  c.cameraSource=ParseCameraSource(cameraSource);
  if(c.cameraSource==CameraSource::Sampled)c.cameraSource=CameraSource::Neutral;
  c.cameraMaxAgeMs=(std::max)(1u,(std::min)(250u,(uint32_t)GetPrivateProfileIntA("fgvk","CameraMaxAgeMs",50,path)));
  c.cameraAudit=GetPrivateProfileIntA("fgvk","CameraAudit",0,path)!=0;
  c.diagnostics=GetPrivateProfileIntA("fgvk","Diagnostics",0,path)!=0;
  c.intervalLogging=GetPrivateProfileIntA("fgvk","IntervalLogging",0,path)!=0;
  c.outputFPSCap=(std::min)(1000u,(uint32_t)GetPrivateProfileIntA("fgvk","OutputFPSCap",0,path));
  c.reflexSleep=false;c.reflexMode=1;
  c.enabled = GetPrivateProfileIntA("fgvk","Enabled",0,path)!=0;
  c.debugLogging = GetPrivateProfileIntA("fgvk","DebugLogging",0,path)!=0; // This public build defaults detailed diagnostics off.
  c.simulateUnsupportedAMD = GetPrivateProfileIntA("fgvk","SimulateUnsupportedAMD",0,path)!=0;
  if(c.provider==FGProvider::Off)c.enabled=false;
  char appid[32]{};
  GetPrivateProfileStringA("fgvk","ApplicationId","0",appid,sizeof(appid),path);
  c.applicationId = (uint32_t)strtoul(appid,nullptr,0);
  c.dlssgFrames = (uint32_t)GetPrivateProfileIntA("fgvk","DLSSGFrames",(int)c.dlssgFrames,path);
  if(c.dlssgFrames<1)c.dlssgFrames=1;const uint32_t configuredMax=c.provider==FGProvider::XeSS?32u:5u;if(c.dlssgFrames>configuredMax)c.dlssgFrames=configuredMax;
  if(c.provider==FGProvider::FSR3)c.dlssgFrames=1;
  c.reflexMode  = GetPrivateProfileIntA("fgvk","ReflexMode",c.reflexMode,path);
  if(c.reflexMode<0||c.reflexMode>2) c.reflexMode=1;
  c.reflexSleep = GetPrivateProfileIntA("fgvk","ReflexSleep",c.reflexSleep?1:0,path)!=0;
  c.tagHudless  = GetPrivateProfileIntA("fgvk","TagHUDLess",c.tagHudless?1:0,path)!=0;
  c.nativeHudProtection=GetPrivateProfileIntA("fgvk","NativeHUDProtection",1,path)!=0;
  c.tagUI       = GetPrivateProfileIntA("fgvk","TagUI",c.tagUI?1:0,path)!=0;
  c.antiTearing=ReadStabilityOption("AntiTearing",false);
  c.antiGhostHistory=ReadStabilityOption("AntiGhostHistory",true);
  { char v[32]{};GetPrivateProfileStringA("fgvk","DepthSeparation","",v,sizeof(v),path);if(v[0]){float f=(float)atof(v);if(f>=1.f&&f<=200.f)c.depthSeparation=f;} }
  Log("FG camera source: %s",CameraSourceName(c.cameraSource));
  Log("Image stability: AntiTearing=%d (FIFO), AntiGhostHistory=%d (input-history recovery)",(int)c.antiTearing,(int)c.antiGhostHistory);
  c.presentPacing = GetPrivateProfileIntA("fgvk","PresentPacing",c.presentPacing?1:0,path)!=0;
  c.fpsCap = (uint32_t)GetPrivateProfileIntA("fgvk","FPSCap",0,path); if(c.fpsCap>480) c.fpsCap=480;
  c.mvecScaleNormalized = GetPrivateProfileIntA("fgvk","MvecScaleNormalized",c.mvecScaleNormalized?1:0,path)!=0;
  c.developerMenuTest = GetPrivateProfileIntA("fgvk","DeveloperMenuTest",0,path)!=0;
  c.onAfterEvalFrames  = (uint32_t)GetPrivateProfileIntA("fgvk","OnAfterEvalFrames",(int)c.onAfterEvalFrames,path);
  c.offAfterIdleFrames = (uint32_t)GetPrivateProfileIntA("fgvk","OffAfterIdleFrames",(int)c.offAfterIdleFrames,path);
  c.noDLSSGraceMs=(uint32_t)GetPrivateProfileIntA("fgvk","NoDLSSGraceMs",100,path);if(c.noDLSSGraceMs>500)c.noDLSSGraceMs=500;
  if(c.onAfterEvalFrames<1) c.onAfterEvalFrames=1; if(c.offAfterIdleFrames<1) c.offAfterIdleFrames=1;
  char kb[32];
  auto key=[&](const char* k, int def){ GetPrivateProfileStringA("fgvk",k,"",kb,sizeof(kb),path); if(!kb[0]) return def; return (int)strtol(kb,nullptr,0); };
  c.keyToggleFG = key("KeyToggleFG", c.keyToggleFG);
  c.menuKey = key("MenuKey", c.menuKey); if(c.menuKey<=0 || c.menuKey>255) c.menuKey=0x2D;
  c.keyCycleFrames = key("KeyCycleFrames", c.keyCycleFrames);
  g_cfg=c;g_rt.activeProvider=c.provider;g_rt.outputFPSCap=c.outputFPSCap;
  if(c.provider==FGProvider::DLSSG)g_rt.appliedPreset=ReadDLSSGPreset();
  g_rt.presentPacing=c.presentPacing; g_rt.fpsCap=c.fpsCap; g_rt.menuKey=c.menuKey;g_rt.depthSeparation=c.depthSeparation;
  g_rt.frames = c.dlssgFrames; g_rt.fgUserOff = GetPrivateProfileIntA("fgvk","UserFGOff",0,path)!=0;
  UpdateMenuRuntime(g_rt.fgUserOff,g_rt.frames);
  Log("config %s: DLSSGFrames=%u (x%u) ReflexMode=%d ReflexSleep=%d TagHUDLess=%d TagUI=%d PresentPacing=%d FPSCap=%u MvecScaleNormalized=%d OnAfterEvalFrames=%u OffAfterIdleFrames=%u keys: toggleFG=0x%x cycle=0x%x",
      path, c.dlssgFrames, c.dlssgFrames+1, c.reflexMode, (int)c.reflexSleep, (int)c.tagHudless, (int)c.tagUI, (int)c.presentPacing, c.fpsCap, (int)c.mvecScaleNormalized, c.onAfterEvalFrames, c.offAfterIdleFrames,
      c.keyToggleFG, c.keyCycleFrames);
}
}
