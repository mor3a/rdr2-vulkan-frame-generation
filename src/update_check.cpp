// Public build: the Nexus runtime update checker is distributed separately.
#include "update_check.h"
namespace fgvk {
void StartUpdateCheck(){}
UpdateState GetUpdateState(){return UpdateState::NotConfigured;}
const char* LatestUpdateVersion(){return "";}
const char* UpdateDownloadURL(){return "";}
void OpenUpdateDownload(){}
bool UpdateBrowserOpening(){return false;}
bool UpdateBrowserFailed(){return false;}
}
