#pragma once
#include <cstdint>
#include <cmath>
namespace fgvk {
// A recycled NGX handle does not identify a feature incarnation. Advance the
// generation on successful SR creation and acknowledge only successful evals.
struct SrTransition {
  uint64_t generation=0;
  uint32_t width=0,height=0;
  bool Changed(uint64_t next,uint32_t w,uint32_t h)const {
    return next && w && h && (next!=generation || w!=width || h!=height);
  }
  void Commit(uint64_t next,uint32_t w,uint32_t h,bool success){
    if(success && next && w && h){generation=next;width=w;height=h;}
  }
};
// The game's parameter object is borrowed for exactly one call. Never persist
// Reset or replace a valid HDR exposure, and leave absent parameters absent.
template<class Parameter> class ScopedSrTransition {
public:
  using GetInt=int(*)(Parameter*,const char*,int*);
  using GetFloat=int(*)(Parameter*,const char*,float*);
  using SetInt=void(*)(Parameter*,const char*,int);
  using SetFloat=void(*)(Parameter*,const char*,float);
  bool resetWritten=false,preExposureWritten=false,exposureScaleWritten=false;
  int originalReset=0;
  float originalPreExposure=1,originalExposureScale=1;
  ScopedSrTransition(Parameter* p,bool changed,GetInt gi,GetFloat gf,SetInt si,SetFloat sf)
      :params(p),setInt(si),setFloat(sf){
    if(!p || !changed)return;
    if(gi && si && gi(p,"Reset",&originalReset)==1 && originalReset==0){si(p,"Reset",1);resetWritten=true;}
    Repair("DLSS.Pre.Exposure",originalPreExposure,preExposureWritten,gf);
    Repair("DLSS.Exposure.Scale",originalExposureScale,exposureScaleWritten,gf);
  }
  ~ScopedSrTransition(){Restore();}
  void Restore(){
    if(resetWritten)setInt(params,"Reset",originalReset);
    if(preExposureWritten)setFloat(params,"DLSS.Pre.Exposure",originalPreExposure);
    if(exposureScaleWritten)setFloat(params,"DLSS.Exposure.Scale",originalExposureScale);
    resetWritten=preExposureWritten=exposureScaleWritten=false;
  }
  ScopedSrTransition(const ScopedSrTransition&)=delete;
  ScopedSrTransition& operator=(const ScopedSrTransition&)=delete;
private:
  Parameter* params;SetInt setInt;SetFloat setFloat;
  void Repair(const char* key,float& value,bool& written,GetFloat get){
    if(get && setFloat && get(params,key,&value)==1 && (!std::isfinite(value)||value<=0)){
      setFloat(params,key,1.f);written=true;
    }
  }
};
}
