#include "fg_preset.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
namespace fgvk {void Log(const char*,...) {}}
int main(int argc,char** argv){auto pid=argc>1?(uint32_t)strtoul(argv[1],nullptr,10):GetCurrentProcessId();auto s=fgvk::QueryFGPreset(pid);char name[32]{};printf("pid=%u NVAPI=%d flags=0x%llx preset=%u reported=%u name=%s\n",pid,s.result,(unsigned long long)s.feedback,s.preset,(unsigned)s.reported,s.reported?fgvk::FGPresetName(s.preset,name,sizeof(name)):"not reported");return s.result==0?0:1;}
