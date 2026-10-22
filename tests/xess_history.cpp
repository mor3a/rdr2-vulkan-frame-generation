// CPU-only camera bookkeeping regression. Does not load a GPU SDK or the game.
#include "../src/rdr_camera.cpp"
#include <cstdio>
namespace fgvk {
void Log(const char*,...) {}
const Config& Cfg(){static const Config config{};return config;}
}
int main(){
  using namespace fgvk;
  sl::Constants c{};
  if(!ApplyNeutralCamera(c,16.f/9,true,false,true) || c.reset!=sl::Boolean::eTrue)return 1;
  // Match the XeSS transaction: commit only recorded AND accepted SR input.
  CommitRdrCamera(true);
  for(unsigned n=0;n<240;++n){
    if(!ApplyNeutralCamera(c,16.f/9,true,false,true) || c.reset!=sl::Boolean::eFalse)return 2;
    CommitRdrCamera(true);
  }
  if(!ApplyNeutralCamera(c,16.f/9,true,true,true) || c.reset!=sl::Boolean::eTrue)return 3;
  CommitRdrCamera(true);
  if(!ApplyNeutralCamera(c,16.f/9,true,false,true) || c.reset!=sl::Boolean::eFalse)return 4;
  // Rejected SR must invalidate pending history; next successful frame recovers.
  CommitRdrCamera(false);
  if(!ApplyNeutralCamera(c,16.f/9,true,false,true) || c.reset!=sl::Boolean::eTrue)return 5;
  CommitRdrCamera(true);
  if(!ApplyNeutralCamera(c,16.f/9,true,false,true) || c.reset!=sl::Boolean::eFalse)return 6;
  CommitRdrCamera(true);
  puts("XeSS history: first frame, 240 continuous frames, cut, rejection and recovery PASS");
}
