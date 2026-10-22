#include "core_diagnostics.h"
#include <atomic>
namespace fgvk { namespace {std::atomic<bool> tracing{};}
bool SetCoreIntervalTracing(bool enabled){
 const auto module=GetModuleHandleW(L"RTX40MFGCore.dll");
 using Toggle=BOOL(WINAPI*)(BOOL);const auto toggle=module?(Toggle)GetProcAddress(module,"MfgUnlockSetIntervalLogging"):nullptr;
 if(!toggle || !toggle(enabled?TRUE:FALSE))return false;tracing=enabled;return true;
}
bool CoreIntervalTracing(){return tracing.load();}
experiment::Distribution ReadCoreIntervals(){
 static experiment::Distribution cached;static ULONGLONG last=0;
 if(!tracing)return {};
 const auto now=GetTickCount64();if(last && now-last<1000)return cached;last=now;
 const auto module=GetModuleHandleW(L"RTX40MFGCore.dll");
 using Read=uint32_t(WINAPI*)(double*,uint32_t);const auto read=module?(Read)GetProcAddress(module,"MfgUnlockReadIntervals"):nullptr;
 double values[512]{};cached=read?experiment::detail::Summarize(values,read(values,512)):experiment::Distribution{};return cached;
}
}
