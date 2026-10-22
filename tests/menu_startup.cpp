#include "menu_startup.h"
#include <cstdio>
int main(){
  fgvk::MenuStartup state;int failed=0;
  auto check=[&](bool value){if(!value)++failed;};
  check(!state.ready);
  for(int i=0;i<300;++i)state.Presented(false,false);
  check(!state.ready);
  state.Presented(true,false);check(!state.ready);
  state.Presented(false,true);check(state.ready);
  state.Presented(false,true);check(state.ready);
  state.Reset();check(!state.ready);
  state.Presented(false,false);check(!state.ready);
  state.Presented(true,true);check(state.ready);
  std::printf("8 overlay startup/recreation checks: %d failures\n",failed);
  return failed?1:0;
}
