#include "experiment_metrics.h"
#include <cstdio>
#include <limits>
int main(){
  using namespace fgvk::experiment;
  int failed=0;
  auto check=[&](bool ok){if(!ok)++failed;};
  check(detail::Summarize(nullptr,0).samples==0);
  const double gaps[]={0,-1,501,std::numeric_limits<double>::quiet_NaN(),10,10,10,20};
  const auto d=detail::Summarize(gaps,8);
  check(d.samples==4 && d.p50==10 && d.p95==20 && d.p99==20 && d.unevenPercent==25);
  check(detail::ValidGap(500) && !detail::ValidGap(500.001));
  double ordered[100];for(int i=0;i<100;++i)ordered[i]=double(100-i);
  const auto p=detail::Summarize(ordered,100);
  check(p.samples==100 && p.p50==50 && p.p95==95 && p.p99==99 && p.unevenPercent==25);
  double bounded[600];for(double& value:bounded)value=16;
  const auto b=detail::Summarize(bounded,600);check(b.samples==512 && b.p99==16 && b.unevenPercent==0);
  std::printf("5 experiment distribution checks: %d failures\n",failed);
  return failed?1:0;
}
