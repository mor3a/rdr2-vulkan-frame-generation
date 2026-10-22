#include "../vendor/xemfg-reference/sdk_pacing_dlssg.h"
#include <cstdio>
int main(){
 using Clock=sdk_pacing::cadence::BatchIntervalEstimator;
 int checks=0,failed=0;auto check=[&](bool b){++checks;if(!b)++failed;};
 for(uint32_t total=2;total<=4;++total){
  Clock c;Clock::Decision p{};int64_t start=1000000000;
  c.Observe(1,total,start);check(!c.Plan(1,total,1,start,p));
  for(uint32_t id=2;id<=240;++id){start+=24000000;c.Observe(id,total,start);
   for(uint32_t index=1;index<total;++index){check(c.Plan(id,total,index,start,p));check(p.interval==24000000/int64_t(total));check(p.deadline==start+p.interval);}}
  check(c.Samples()==6);check(!c.Plan(240,total,0,start,p));check(!c.Plan(240,total,total,start,p));
  check(c.Plan(240,total,1,start+200000000,p) && p.deadline<=start+200000000);
  c.Observe(242,total,start+24000000);check(!c.Plan(242,total,1,start+24000000,p));
  c.Observe(243,total,start+48000000);check(c.Plan(243,total,1,start+48000000,p));
  c.Observe(244,total,start+1048000000);check(!c.Plan(244,total,1,start+1048000000,p));
  c.Reset();check(!c.Plan(244,total,1,start,p));
 }
 Clock c;Clock::Decision p{};c.Observe(1,2,1000000000);c.Observe(2,2,1020000000);
 c.Observe(3,3,1040000000);check(!c.Plan(3,3,1,1040000000,p));
 c.Observe(4,3,1060000000);check(c.Plan(4,3,1,1060000000,p));
 c.Observe(4,3,1060000000);check(c.Samples()==1);
 std::printf("%d XeSS pacing checks (2x/3x/4x, late output, pause, gaps, transitions): %d failures\n",checks,failed);return failed?1:0;
}
