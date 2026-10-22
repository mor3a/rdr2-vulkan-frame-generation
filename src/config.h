#pragma once
#include <cstdint>
#include <atomic>
#include "experiment_policy.h"
namespace fgvk {
bool ReadStabilityOption(const char* key,bool fallback);
void SaveStabilityOption(const char* key,bool enabled);
void SaveDepthSeparation(float value);
int ReadDLSSGPreset();
void SaveDLSSGPreset(int value);
void SaveExperimentSetting(const char* key,const char* value);
void SaveOutputFPSCap(uint32_t cap);
enum class FGProvider { Off, DLSSG, FSR3, XeSS, DLSSGBridge };
inline bool IsDX12Bridge(FGProvider provider){return provider==FGProvider::XeSS || provider==FGProvider::DLSSGBridge;}
// fgvk.ini next to fgvk.dll (written with defaults on first run). Read once at DLL attach.
struct Config {
  CameraSource cameraSource=CameraSource::Neutral;
  uint32_t cameraMaxAgeMs=50;
  bool cameraAudit=false;
  bool diagnostics=false;
  bool intervalLogging=false;
  uint32_t outputFPSCap=0;
  bool enabled = false; // explicit opt-in: experimental RDR2 port
  bool debugLogging = false;
  bool simulateUnsupportedAMD = false; // Hidden opt-in test; never written to public INI.
  FGProvider provider=FGProvider::DLSSG;
  bool nativeDLSSGBackend=true;
  bool experimentalRTX20=false;
  uint32_t applicationId = 0; // do not inherit another game's NGX identity

  uint32_t dlssgFrames = 1;      // generated frames per real frame: 1 = x2, 2 = x3, 3 = x4
  int      reflexMode  = 1;      // Native DLSS-G keeps Reflex active; 2 enables boost
  bool     reflexSleep = false;  // Native default; bridges own their SDK sleep independently
  bool     tagHudless  = false;  // SR output precedes the final backbuffer's color processing
  bool     tagUI       = false;  // tag a transparent UI color+alpha layer
  bool nativeHudProtection=true; // final-color snapshot before HUD; Native DLSS-G only
  bool antiTearing = false;
  bool antiGhostHistory = true;
  float depthSeparation = 10.f;  // sl::Constants minRelativeLinearDepthObjectSeparation; lower = stricter object separation at depth edges (anti-halo). 40 = Streamline default. INI: DepthSeparation
  bool     presentPacing = false; // optional extra GPU wait; off avoids duplicate serialization
  uint32_t fpsCap = 0;           // present-thread frame rate limit, 0 = unlimited
  bool     mvecScaleNormalized = true;  // send Streamline mvScale / mvec extent (it multiplies back by the extent); 0 = raw NGX values
  bool     developerMenuTest = false;   // hidden: only read when written manually as DeveloperMenuTest=1 in the INI; shows menu UI test controls
  uint32_t onAfterEvalFrames  = 60;   // DLSS-SR frames before DLSS-G turns on
  uint32_t offAfterIdleFrames = 3;   // presents without DLSS-SR before DLSS-G suspends
  uint32_t noDLSSGraceMs = 100; // tolerate short cross-thread input gaps before suspending
  // Hotkeys (Windows virtual-key codes, 0 = disabled). Defaults follow PureDark's INI: numpad * toggles
  // FG, End cycles x2/x3/x4.
  int keyToggleFG   = 0x6A;   // VK_MULTIPLY
  int menuKey = 0x2D;         // VK_INSERT, menu open/close key
  int keyCycleFrames= 0x23;   // VK_END
};
const Config& Cfg();
void LoadConfig();
// Runtime overrides (hotkeys). Read on the present thread; changes take effect next present.
struct Runtime {
  std::atomic<uint32_t> outputFPSCap{0};
  std::atomic<FGProvider> activeProvider{FGProvider::DLSSG};
  std::atomic<int> appliedPreset{-1};
  std::atomic<bool> fgUserOff{false};   // shared with the DLSS input recording thread
  uint32_t frames = 1;          // current generated-frames setting
  std::atomic<uint32_t> maxFrames{1}; // actual selected provider capability
  std::atomic<uint32_t> fpsCap{0};
  std::atomic<int> menuKey{0x2D};
  std::atomic<float> depthSeparation{10.f};
  bool presentPacing = false;
};
Runtime& Rt();
inline FGProvider ActiveProvider(){return Rt().activeProvider.load(std::memory_order_acquire);}
// Poll hotkeys (call once per present on the present thread). Returns true if anything changed.
bool PollHotkeys();
void SaveNextProvider(FGProvider provider);
bool LoadFPSCounter();
uint32_t LoadFPSCap();
void SaveMenuKey(int vk);
float LoadMenuScale();
void SaveMenuScale(float scale);
void SaveFPSCap(uint32_t cap);
void SaveFPSCounter(bool enabled);
bool PerformanceProbeRequested();
void EndPerformanceProbe();
}

