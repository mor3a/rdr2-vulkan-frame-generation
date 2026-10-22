// Runs the production Windows fetch/parser/state path, without loading the
// game or opening a browser. The second request checks refresh scheduling.
#include "../src/update_check.cpp"
#include <cstdarg>
namespace fgvk {
void Log(const char* format,...){va_list args;va_start(args,format);vprintf(format,args);va_end(args);puts("");}
const Config& Cfg(){static const Config c{};return c;}
}
int main(){
 using namespace fgvk;
 Worker(nullptr);
 auto s=GetUpdateState();if(s!=UpdateState::Available && s!=UpdateState::Current)return 1;
 if(s==UpdateState::Available && (!LatestUpdateVersion()[0] || !NexusURL(UpdateDownloadURL())))return 2;
 lastAttempt.store(GetTickCount64());StartUpdateCheck(true);if(checking.load())return 3;
 lastAttempt.store(GetTickCount64()-60001);StartUpdateCheck();
 const auto start=GetTickCount64();while(checking.load() && GetTickCount64()-start<15000)Sleep(20);
 if(checking.load())return 4;
 s=GetUpdateState();if(s!=UpdateState::Available && s!=UpdateState::Current)return 5;
 printf("Production WinHTTP Nexus fetch, label comparison, notification state/URL and repeated check PASS; installed=%s state=%u latest=%s\n",kModVersion,(unsigned)s,LatestUpdateVersion());
 return 0;
}
