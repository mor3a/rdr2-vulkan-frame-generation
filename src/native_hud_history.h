#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <algorithm>
namespace fgvk {
// The acquired image epoch, recorded copy and GPU submission token must all
// agree. A previous image's HUD-less capture is never a fallback.
class NativeHudHistory {
  struct Slot {uint64_t epoch=0;uint32_t token=0;bool recorded=false,submitted=false;};
  struct Copy {uint64_t image,epoch;};
  std::unordered_map<uint64_t,Slot> images;
  std::unordered_map<uint64_t,std::vector<Copy>> commands;
  uint64_t serial=0;
public:
  void Add(uint64_t image){images.emplace(image,Slot{});}
  void Remove(uint64_t image){images.erase(image);for(auto c=commands.begin();c!=commands.end();){auto& copies=c->second;copies.erase(std::remove_if(copies.begin(),copies.end(),[&](const Copy& copy){return copy.image==image;}),copies.end());if(copies.empty())c=commands.erase(c);else ++c;}}
  void Acquire(uint64_t image){auto i=images.find(image);if(i!=images.end()){i->second=Slot{};i->second.epoch=++serial;}}
  bool Record(uint64_t cmd,uint64_t image){
    auto i=images.find(image);if(!cmd || i==images.end() || !i->second.epoch || i->second.recorded)return false;
    i->second.recorded=true;commands[cmd].push_back({image,i->second.epoch});return true;
  }
  void Forget(uint64_t cmd){
    auto c=commands.find(cmd);if(c==commands.end())return;
    for(auto s:c->second){auto i=images.find(s.image);if(i!=images.end() && i->second.epoch==s.epoch && !i->second.submitted)i->second.recorded=false;}
    commands.erase(c);
  }
  void Execute(uint64_t primary,const std::vector<uint64_t>& secondary){
    std::vector<Copy> copies;
    for(auto c:secondary){auto i=commands.find(c);if(i!=commands.end())copies.insert(copies.end(),i->second.begin(),i->second.end());}
    if(!copies.empty()){auto& out=commands[primary];out.insert(out.end(),copies.begin(),copies.end());}
  }
  void Submit(uint64_t cmd,uint32_t token){
    auto c=commands.find(cmd);if(c==commands.end())return;
    for(auto s:c->second){auto i=images.find(s.image);if(i!=images.end() && i->second.epoch==s.epoch && i->second.recorded && !i->second.submitted){i->second.token=token;i->second.submitted=true;}}
  }
  bool Matches(uint64_t image,uint32_t token)const {
    auto i=images.find(image);return token && i!=images.end() && i->second.submitted && i->second.token==token;
  }
};
}
