#pragma once
namespace fgvk {
inline constexpr const char* kModVersion="0.82";
// Detailed diagnostics are controlled by the INI DebugLogging option.
inline bool IsBetaBuild(){ const char* v=kModVersion; for(const char* p=v; *p; ++p){ if(p[0]=='b'&&p[1]=='e'&&p[2]=='t'&&p[3]=='a') return true; } return false; }
enum class UpdateState { NotStarted, NotConfigured, Checking, Current, Available, Unavailable };
void StartUpdateCheck(bool menuOpened=false);
UpdateState GetUpdateState();
const char* LatestUpdateVersion();
const char* UpdateDownloadURL();
void OpenUpdateDownload(bool developerPreview=false);
bool UpdateBrowserOpening();
bool UpdateBrowserFailed();
}
