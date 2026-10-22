#pragma once
#include "runtime_errors.h"
#include <cstdio>
namespace fgvk {
struct MenuErrorPreview {
 std::array<bool,RuntimeErrors::Capacity> active{};
 void Set(RuntimeErrors::Code code){if(RuntimeErrors::Valid(code))active[(unsigned)code]=true;}
 void Clear(){active.fill(false);}
 RuntimeErrors::Snapshot Merge(RuntimeErrors::Snapshot real) const {
  for(unsigned i=0;i<RuntimeErrors::Capacity && real.count<RuntimeErrors::Capacity;++i){
   if(!active[i])continue;
   bool found=false;for(size_t j=0;j<real.count;++j)if((unsigned)real.entries[j].code==i)found=true;
   if(found)continue;
   auto code=(RuntimeErrors::Code)i;auto& e=real.entries[real.count++];
   e.code=code;e.id=RuntimeErrors::CodeId(code);e.title=RuntimeErrors::Title(code);e.action=RuntimeErrors::Action(code);
   snprintf(e.detail.data(),e.detail.size(),"Developer UI preview only; no runtime fault was injected.");
  }
  return real;
 }
};
}
