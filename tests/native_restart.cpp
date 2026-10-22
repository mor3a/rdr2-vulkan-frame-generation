#include "native_restart.h"
#include "preset_selection.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <array>
int main(){
  using namespace fgvk;
  NativeRestartState state;
  assert(!state.Pending() && !state.Failed());
  assert(!state.Request(4));
  for(unsigned cycle=0;cycle<1000;++cycle){
    assert(state.Request(cycle%4));
    assert(!state.Request((cycle+1)%4));
    assert(state.Next()==NativeRestartState::Action::Disable);
    state.Presented(true); // present alone cannot skip the successful Off transition
    assert(state.Next()==NativeRestartState::Action::Disable);
    state.Disabled(true);
    assert(state.Pending() && state.Next()==NativeRestartState::Action::None);
    state.Presented(false); // out-of-date/failed present cannot authorize a resource reset
    assert(state.Next()==NativeRestartState::Action::None);
    state.Presented(true);
    assert(state.Next()==NativeRestartState::Action::Rebuild && state.Preset()==cycle%4);
    state.Complete(true);
    assert(!state.Pending() && !state.Failed());
  }
  assert(state.Request(2));state.Disabled(false);
  assert(!state.Pending() && state.Failed());
  assert(state.Request(1));state.Disabled(true);state.Presented(true);state.Complete(false);
  assert(state.Failed());
  assert(state.Request(3));state.Disabled(true);state.Presented(true);state.Complete(true);
  assert(!state.Failed());
  NativeRestartState concurrent;std::array<std::thread,8> workers;std::atomic<unsigned> accepted{};
  for(auto& worker:workers)worker=std::thread([&]{if(concurrent.Request(3))++accepted;});
  for(auto& worker:workers)worker.join();assert(accepted==1 && concurrent.Preset()==3);
  for(int initial=0;initial<=3;++initial){
    PresetSelection preset(initial);preset.Applied(-1);assert(!preset.Pending());
    for(unsigned open=0;open<100;++open){preset.Applied(-1);assert(!preset.Pending());}
    const int next=(initial+1)%4;preset.Save(next);assert(preset.Pending());
    preset.Applied(initial);assert(preset.Pending());preset.Applied(next);assert(!preset.Pending());
    preset.Save(initial);assert(preset.Pending());preset.Save(next);assert(!preset.Pending());
  }
  puts("Native restart: 1000 staged cycles; failed presents never authorize release; failure/retry and 8 concurrent requests; preset opening/unchanged/unavailable feedback: PASS");
}
