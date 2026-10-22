#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include "update_check.h"
namespace fgvk { void Log(const char* fmt,...){va_list args;va_start(args,fmt);vprintf(fmt,args);va_end(args);puts("");} }
int main(){fgvk::StartUpdateCheck();for(int i=0;i<400;++i){auto s=fgvk::GetUpdateState();if(s!=fgvk::UpdateState::Checking){printf("state=%d latest=%s url=%s\n",int(s),fgvk::LatestUpdateVersion(),fgvk::UpdateDownloadURL());return s==fgvk::UpdateState::Available?0:1;}Sleep(50);}return 2;}
