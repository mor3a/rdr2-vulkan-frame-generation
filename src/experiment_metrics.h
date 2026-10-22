#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fgvk::experiment {
struct Distribution {
  uint32_t samples{};
  double p50{}, p95{}, p99{}, unevenPercent{}, adjacentDifferenceMs{}; // milliseconds; uneven = >1.5x median
};
struct ThreadSample {
  DWORD id{};
  double cpuMsPerSecond{}; // one fully occupied logical CPU = 1000
  uint64_t cyclesPerSecond{};
  wchar_t name[48]{};
};
struct Snapshot {
  bool initialized{}, unavailable{};
  Distribution clientPresent;
  uint32_t threadCount{}; // all enumerated process threads; rates cover at most 512
  std::array<ThreadSample,8> threads{}; // largest CPU rates first; id=0 means unused
};
void SetEnabled(bool enabled); // call outside loader lock
bool Enabled();
void RecordPresent(); // successful application Present only, never generated scanout
Snapshot Read(); // cached; schedules at most one background sample per second

namespace detail {
inline bool ValidGap(double ms){return std::isfinite(ms) && ms>0 && ms<=500.;}
// Pure bounded calculation, also used by the host-only fixture.
inline Distribution Summarize(const double* values, size_t count){
  std::array<double,512> sorted{};
  Distribution out;
  for(size_t i=0;i<count && out.samples<sorted.size();++i)
    if(ValidGap(values[i]))sorted[out.samples++]=values[i];
  if(!out.samples)return out;
  double difference=0,previous=0;uint32_t pairs=0;for(size_t i=0;i<count;++i)if(ValidGap(values[i])){if(previous){difference+=std::abs(values[i]-previous);++pairs;}previous=values[i];}
  out.adjacentDifferenceMs=pairs?difference/pairs:0;
  std::sort(sorted.begin(),sorted.begin()+out.samples);
  auto percentile=[&](double p){return sorted[static_cast<size_t>(std::ceil(p*out.samples))-1];};
  out.p50=percentile(.50);out.p95=percentile(.95);out.p99=percentile(.99);
  uint32_t uneven=0;
  for(uint32_t i=0;i<out.samples;++i)if(sorted[i]>out.p50*1.5)++uneven;
  out.unevenPercent=100.*uneven/out.samples;
  return out;
}
}
}
