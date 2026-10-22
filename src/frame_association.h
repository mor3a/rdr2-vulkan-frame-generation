#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>
namespace fgvk {
// Resolve secondary buffers at submission, then propagate through actual GPU
// queue ordering and binary semaphore dependencies; never use CPU timing.
class FrameAssociation {
 struct Command { uint32_t token=0; std::vector<uint64_t> children; };
 std::unordered_map<uint64_t,Command> commands;
 std::unordered_map<uint64_t,uint32_t> queues,signals;
 uint32_t retired=0;
 uint32_t Token(uint64_t c,unsigned depth=0)const {
  if(depth>16)return 0;
  auto i=commands.find(c);if(i==commands.end())return 0;
  uint32_t t=i->second.token;
  for(auto child:i->second.children)t=std::max(t,Token(child,depth+1));
  return t>retired?t:0;
 }
public:
 void Record(uint64_t c,uint32_t t){if(c && t)commands[c].token=t;}
 void Forget(uint64_t c){commands.erase(c);}
 void Execute(uint64_t c,const std::vector<uint64_t>& secondary){auto& v=commands[c].children;v.insert(v.end(),secondary.begin(),secondary.end());}
 uint32_t CommandToken(uint64_t c)const{return Token(c);}
 uint32_t Submit(uint64_t queue,const std::vector<uint64_t>& cs,const std::vector<uint64_t>& waits,const std::vector<uint64_t>& ss){
  uint32_t t=queues[queue];
  for(auto s:waits){auto i=signals.find(s);if(i!=signals.end()){t=std::max(t,i->second);signals.erase(i);}}
  for(auto c:cs)t=std::max(t,Token(c));
  queues[queue]=t;
  if(signals.size()>2048)signals.clear();
  for(auto s:ss)if(s)signals[s]=t;
  return t>retired?t:0;
 }
 uint32_t Present(const std::vector<uint64_t>& waits)const{
  uint32_t t=0;for(auto s:waits){auto i=signals.find(s);if(i!=signals.end())t=std::max(t,i->second);}return t>retired?t:0;
 }
 void Retire(uint32_t t){retired=std::max(retired,t);}
};
}
