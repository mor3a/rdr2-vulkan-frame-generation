#include "fps_meter.h"
#include <cmath>
#include <cstdio>
int main(){
  fgvk::FPSMeter m;int failed=0;uint64_t now=100;
  auto check=[&](bool ok){if(!ok)++failed;};
  m.Present(now);
  auto run=[&](unsigned multiplier,bool telemetry){for(int i=0;i<50;++i){now+=20;if(telemetry)m.AddDisplayed(multiplier);m.Present(now);}};
  run(1,true);check(m.outputValid && std::abs(m.realFPS-50)<0.01f && std::abs(m.outputFPS-50)<0.01f);
  run(3,true);check(m.outputValid && std::abs(m.realFPS-50)<0.01f && std::abs(m.outputFPS-150)<0.01f);
  run(2,true);check(m.outputValid && std::abs(m.outputFPS-100)<0.01f);
  run(0,false);check(m.outputValid && std::abs(m.outputFPS-100)<0.01f); // one dropped window keeps last value
  run(0,false);run(0,false);check(!m.outputValid && std::abs(m.realFPS-50)<0.01f); // sustained loss clears it
  now+=10000;check(m.Present(now) && m.realFPS==0 && !m.outputValid); // pause publishes cleared values, not stale FPS
  run(3,true);
  check(m.outputValid && std::abs(m.realFPS-50)<0.01f && std::abs(m.outputFPS-150)<0.01f);
  m.ResetDisplayed();check(!m.outputValid && m.outputFPS==0 && m.displayed==0 && m.readings==0);
  std::printf("8 FPS sampling checks (1x/3x/2x/dropout/pause/reset): %d failures\n",failed);
  return failed?1:0;
}
