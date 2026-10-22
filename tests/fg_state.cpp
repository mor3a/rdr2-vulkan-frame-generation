#include "fg_state.h"
#include <cstdio>
int main(){
  using namespace fgvk;int failures=0;
  auto check=[&](bool ok){if(!ok)++failures;};
  check(FGResultAccepted(0));check(FGResultAccepted(39));check(!FGResultAccepted(1));
  check(ClassifyFG(false,false,false,false,0)==FGState::Off);
  check(ClassifyFG(true,false,false,false,0)==FGState::Waiting);
  check(ClassifyFG(true,true,false,false,3)==FGState::EnabledUnmeasured);
  check(ClassifyFG(true,true,false,true,1)==FGState::EnabledUnmeasured);
  check(ClassifyFG(true,true,false,true,3)==FGState::Generating);
  check(ClassifyFG(true,true,true,true,3)==FGState::Error);
  check(ClassifyFG(false,true,true,true,2)==FGState::Error);
  std::printf("10 FG result and measured-state checks: %d failures\n",failures);
  return failures?1:0;
}
