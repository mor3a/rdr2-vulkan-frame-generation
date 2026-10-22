#include "sr_transition.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <cstdio>
struct Params {int reset=0;float pre=2.f,scale=.5f;unsigned writes=0;bool missing=false;};
static int GetI(Params* p,const char*,int* out){if(p->missing)return 0;*out=p->reset;return 1;}
static int GetF(Params* p,const char* n,float* out){if(p->missing)return 0;*out=!strcmp(n,"DLSS.Pre.Exposure")?p->pre:p->scale;return 1;}
static void SetI(Params* p,const char*,int v){p->reset=v;++p->writes;}
static void SetF(Params* p,const char* n,float v){(!strcmp(n,"DLSS.Pre.Exposure")?p->pre:p->scale)=v;++p->writes;}
using Guard=fgvk::ScopedSrTransition<Params>;
int main(){
  fgvk::SrTransition history;
  assert(!history.Changed(0,1280,720) && !history.Changed(1,0,720));
  for(unsigned gen=1;gen<=1000;++gen){
    assert(history.Changed(gen,1280,720));
    history.Commit(gen,1280,720,false);assert(history.Changed(gen,1280,720));
    Params p;
    {Guard g(&p,true,GetI,GetF,SetI,SetF);assert(p.reset==1 && p.pre==2 && p.scale==.5f);}
    assert(p.reset==0 && p.writes==2); // including a failed eval's unwind
    history.Commit(gen,1280,720,true);
    for(unsigned frame=0;frame<30;++frame){
      assert(!history.Changed(gen,1280,720));
      Guard g(&p,false,GetI,GetF,SetI,SetF);assert(p.reset==0 && p.writes==2);
    }
    assert(history.Changed(gen,1114,626));
  }
  Params p;p.reset=1;
  {Guard g(&p,true,GetI,GetF,SetI,SetF);assert(!g.resetWritten && !p.writes);}
  assert(p.reset==1 && !p.writes);
  p.missing=true;
  {Guard g(&p,true,GetI,GetF,SetI,SetF);assert(!p.writes);}
  p.missing=false;p.reset=0;p.pre=0;p.scale=std::numeric_limits<float>::quiet_NaN();
  {Guard g(&p,true,GetI,GetF,SetI,SetF);assert(p.reset==1 && p.pre==1 && p.scale==1);g.Restore();assert(p.reset==0 && p.pre==0 && std::isnan(p.scale));}
  assert(p.writes==6); // explicit Restore + destructor must not double-restore
  p.pre=std::numeric_limits<float>::infinity();p.scale=-1;
  {Guard g(&p,true,GetI,GetF,SetI,SetF);assert(p.pre==1 && p.scale==1);}
  assert(std::isinf(p.pre) && p.scale==-1);
  puts("SR transition: 1000 feature incarnations, steady-state no writes, retry on failure, original reset/HDR preserved, invalid exposure scoped and restored: PASS");
}
