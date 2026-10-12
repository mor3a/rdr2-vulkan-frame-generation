#pragma once
#include <cstdint>
#include <atomic>
namespace fgvk {
enum class FGProvider { Off, DLSSG, FSR3 };
// fgvk.ini next to fgvk.dll (written with defaults on first run). Read once at DLL attach.
struct Config {
  bool enabled = false; // explicit opt-in: experimental RDR2 port
  FGProvider provider=FGProvider::DLSSG;
  uint32_t applicationId = 0; // do not inherit another game's NGX identity

  uint32_t dlssgFrames = 1;      // generated frames per real frame: 1 = x2, 2 = x3, 3 = x4
  int      reflexMode  = 2;      // 0 off, 1 low latency, 2 low latency + boost (PureDark's default)
  bool     reflexSleep = false;  // RDR2/RTX3050 measured 500ms stalls with SL sleep; markers remain enabled
  bool     tagHudless  = false;  // SR output precedes the final backbuffer's color processing
  bool     tagUI       = false;  // tag a transparent UI color+alpha layer
  bool     presentPacing = false; // optional extra GPU wait; off avoids duplicate serialization
  bool     mvecScaleNormalized = true;  // send Streamline mvScale / mvec extent (it multiplies back by the extent); 0 = raw NGX values
  uint32_t onAfterEvalFrames  = 60;   // DLSS-SR frames before DLSS-G turns on
  uint32_t offAfterIdleFrames = 3;   // presents without DLSS-SR before DLSS-G suspends
  uint32_t noDLSSGraceMs = 100; // tolerate short cross-thread input gaps before suspending
  // Hotkeys (Windows virtual-key codes, 0 = disabled). Defaults follow PureDark's INI: numpad * toggles
  // FG, End cycles x2/x3/x4.
  int keyToggleFG   = 0x6A;   // VK_MULTIPLY
  int keyCycleFrames= 0x23;   // VK_END
};
const Config& Cfg();
void LoadConfig();
// Runtime overrides (hotkeys). Read on the present thread; changes take effect next present.
struct Runtime {
  std::atomic<bool> fgUserOff{false};   // shared with the DLSS input recording thread
  uint32_t frames = 1;          // current generated-frames setting
  bool presentPacing = false;
};
Runtime& Rt();
// Poll hotkeys (call once per present on the present thread). Returns true if anything changed.
bool PollHotkeys();
void SaveNextProvider(FGProvider provider);
bool LoadFPSCounter();
void SaveFPSCounter(bool enabled);
bool PerformanceProbeRequested();
void EndPerformanceProbe();
}
