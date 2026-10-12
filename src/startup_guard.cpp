#include "startup_guard.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
namespace fgvk {
static char marker[MAX_PATH]{};static bool pending=false;
bool BeginStartupGuard(){
  HMODULE self{};GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)&BeginStartupGuard,&self);
  char root[MAX_PATH]{};GetModuleFileNameA(self,root,MAX_PATH);auto slash=strrchr(root,'\\');if(!slash)return true;*(slash+1)=0;
  char ini[MAX_PATH]{};snprintf(ini,sizeof(ini),"%sRDR2VulkanFG.ini",root);
  if(GetPrivateProfileIntA("fgvk","SafeStartup",1,ini)==0 || Rt().fgUserOff)return true;
  snprintf(marker,sizeof(marker),"%sRDR2VulkanFG\\startup.pending",root);
  char expected[128]{};snprintf(expected,sizeof(expected),"%u|0.22",(unsigned)Cfg().provider);
  FILE* file=nullptr;char previous[128]{};
  if(fopen_s(&file,marker,"rb")==0 && file){fread(previous,1,sizeof(previous)-1,file);fclose(file);}
  if(strcmp(previous,expected)==0){Log("SafeStartup: previous startup interrupted with this provider/build; FG disabled for this run. Change FGProvider or set SafeStartup=0 to explicitly retry.");return false;}
  if(fopen_s(&file,marker,"wb")==0 && file){fwrite(expected,1,strlen(expected),file);fclose(file);pending=true;}
  else Log("SafeStartup: cannot write startup marker; guard unavailable");
  return true;
}
void CompleteStartupGuard(){if(pending){pending=false;DeleteFileA(marker);Log("SafeStartup: successful frame generation verified; startup marker cleared");}}
}
