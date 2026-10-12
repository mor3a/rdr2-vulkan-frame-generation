#include "config.h"
#include "log.h"
#include "inimigrate.h"
#include "menu.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
namespace fgvk {
static Config g_cfg;
static Runtime g_rt;
static bool probeRequested{};
const Config& Cfg(){ return g_cfg; }
Runtime& Rt(){ return g_rt; }
static void IniPath(char* out,size_t n);
void SaveNextProvider(FGProvider provider){
  char path[MAX_PATH]{};IniPath(path,sizeof(path));
  WritePrivateProfileStringA("fgvk","FGProvider",provider==FGProvider::FSR3?"FSR3":"DLSSG",path);
  WritePrivateProfileStringA("fgvk","UserFGOff","0",path);
}
bool LoadFPSCounter(){char path[MAX_PATH]{};IniPath(path,sizeof(path));return GetPrivateProfileIntA("fgvk","ShowFPS",0,path)!=0;}
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
  if(gameFocused && cycle && g_cfg.provider==FGProvider::DLSSG){ g_rt.frames = g_rt.frames>=3 ? 1 : g_rt.frames+1; Log("hotkey: generated frames -> %u (x%u)", g_rt.frames, g_rt.frames+1); changed=true; }
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
    "; fgvk - experimental RDR2 Vulkan DLSS Frame Generation. Edit and restart the game.\n"
    "[fgvk]\n"
    "Enabled=0\n"
    "ApplicationId=0\n"
    "; Settings format; fgvk updates older files automatically. Do not edit.\n"
    "ConfigVersion=2\n"
    "; Generated frames per real frame: 1 = x2, 2 = x3, 3 = x4 (clamped to what the GPU supports)\n"
    "DLSSGFrames=1\n"
    "; Reflex: 0 = off, 1 = low latency, 2 = low latency + boost\n"
    "ReflexMode=2\n"
    "; Call slReflexSleep once per frame (1) - required by NVIDIA's Reflex checklist\n"
    "ReflexSleep=0\n"
    "; The DLSS-SR output precedes final color processing and is transient.\n"
    "; Leave it untagged; frame generation uses the final backbuffer for color.\n"
    "TagHUDLess=0\n"
    "; Feed a fully transparent UI color+alpha layer (0 = none)\n"
    "TagUI=0\n"
    "; While frame generation is on, wait (max 100 ms) for the frame's GPU work before marking PresentStart.\n"
    "; Smooths bursty multi-frame-generation pacing at a small fps cost. 0 = off\n"
    "PresentPacing=0\n"
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
    Log("config: updated fgvk.ini from format %d to %d: TagHUDLess=0 (orbit flickering fix), PresentPacing=1 unless set", was, kIniVersion);
  Config c;
  char provider[32]{};GetPrivateProfileStringA("fgvk","FGProvider","DLSSG",provider,sizeof(provider),path);
  if(_stricmp(provider,"FSR3")==0)c.provider=FGProvider::FSR3;
  else if(_stricmp(provider,"OFF")==0)c.provider=FGProvider::Off;
  else if(_stricmp(provider,"DLSSG")!=0){c.provider=FGProvider::Off;Log("Unknown FGProvider=%s; FG disabled",provider);}
  c.enabled = GetPrivateProfileIntA("fgvk","Enabled",0,path)!=0;
  if(c.provider==FGProvider::Off)c.enabled=false;
  char appid[32]{};
  GetPrivateProfileStringA("fgvk","ApplicationId","0",appid,sizeof(appid),path);
  c.applicationId = (uint32_t)strtoul(appid,nullptr,0);
  c.dlssgFrames = (uint32_t)GetPrivateProfileIntA("fgvk","DLSSGFrames",(int)c.dlssgFrames,path);
  if(c.dlssgFrames<1) c.dlssgFrames=1; if(c.dlssgFrames>3) c.dlssgFrames=3;
  if(c.provider==FGProvider::FSR3)c.dlssgFrames=1;
  c.reflexMode  = GetPrivateProfileIntA("fgvk","ReflexMode",c.reflexMode,path);
  if(c.reflexMode<0||c.reflexMode>2) c.reflexMode=2;
  c.reflexSleep = GetPrivateProfileIntA("fgvk","ReflexSleep",c.reflexSleep?1:0,path)!=0;
  c.tagHudless  = GetPrivateProfileIntA("fgvk","TagHUDLess",c.tagHudless?1:0,path)!=0;
  c.tagUI       = GetPrivateProfileIntA("fgvk","TagUI",c.tagUI?1:0,path)!=0;
  c.presentPacing = GetPrivateProfileIntA("fgvk","PresentPacing",c.presentPacing?1:0,path)!=0;
  c.mvecScaleNormalized = GetPrivateProfileIntA("fgvk","MvecScaleNormalized",c.mvecScaleNormalized?1:0,path)!=0;
  c.onAfterEvalFrames  = (uint32_t)GetPrivateProfileIntA("fgvk","OnAfterEvalFrames",(int)c.onAfterEvalFrames,path);
  c.offAfterIdleFrames = (uint32_t)GetPrivateProfileIntA("fgvk","OffAfterIdleFrames",(int)c.offAfterIdleFrames,path);
  c.noDLSSGraceMs=(uint32_t)GetPrivateProfileIntA("fgvk","NoDLSSGraceMs",100,path);if(c.noDLSSGraceMs>500)c.noDLSSGraceMs=500;
  if(c.onAfterEvalFrames<1) c.onAfterEvalFrames=1; if(c.offAfterIdleFrames<1) c.offAfterIdleFrames=1;
  char kb[32];
  auto key=[&](const char* k, int def){ GetPrivateProfileStringA("fgvk",k,"",kb,sizeof(kb),path); if(!kb[0]) return def; return (int)strtol(kb,nullptr,0); };
  c.keyToggleFG = key("KeyToggleFG", c.keyToggleFG);
  c.keyCycleFrames = key("KeyCycleFrames", c.keyCycleFrames);
  g_cfg=c;
  g_rt.presentPacing=c.presentPacing;
  g_rt.frames = c.dlssgFrames; g_rt.fgUserOff = GetPrivateProfileIntA("fgvk","UserFGOff",0,path)!=0;
  UpdateMenuRuntime(g_rt.fgUserOff,g_rt.frames);
  Log("config %s: DLSSGFrames=%u (x%u) ReflexMode=%d ReflexSleep=%d TagHUDLess=%d TagUI=%d PresentPacing=%d MvecScaleNormalized=%d OnAfterEvalFrames=%u OffAfterIdleFrames=%u keys: toggleFG=0x%x cycle=0x%x",
      path, c.dlssgFrames, c.dlssgFrames+1, c.reflexMode, (int)c.reflexSleep, (int)c.tagHudless, (int)c.tagUI, (int)c.presentPacing, (int)c.mvecScaleNormalized, c.onAfterEvalFrames, c.offAfterIdleFrames,
      c.keyToggleFG, c.keyCycleFrames);
}
}
