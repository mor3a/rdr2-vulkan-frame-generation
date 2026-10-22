#include "update_check.h"
#include "log.h"
#include "update_manifest.h"
#include "update_source.h"
#include "config.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <objbase.h>
#include <atomic>
namespace fgvk {
static std::atomic<UpdateState> state{UpdateState::NotStarted};
static UpdateManifest manifest;
static SRWLOCK manifestLock=SRWLOCK_INIT;
static std::atomic<bool> checking{false};
static std::atomic<ULONGLONG> lastAttempt{0};
static std::atomic<bool> browserOpening{false},browserFailed{false};
static char browserURL[256]{}; // Published to the worker after acquiring browserOpening.
struct InternetHandle {HINTERNET h{};~InternetHandle(){if(h)WinHttpCloseHandle(h);}};
static bool FetchManifest(UpdateManifest& out){
  InternetHandle session{WinHttpOpen(L"RDR2VulkanFG/0.82",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
  if(!session.h || !WinHttpSetTimeouts(session.h,3000,3000,3000,3000))return false;
  InternetHandle connection{WinHttpConnect(session.h,L"api.nexusmods.com",INTERNET_DEFAULT_HTTPS_PORT,0)};if(!connection.h)return false;
  InternetHandle request{WinHttpOpenRequest(connection.h,L"POST",L"/v2/graphql",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};if(!request.h)return false;
  DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if(!WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)))return false;
  char query[512]{};snprintf(query,sizeof(query),"{\"query\":\"{ legacyModsByDomain(ids: [{gameDomain: \\\"reddeadredemption2\\\", modId: %u}]) { nodes { version modId status } } }\"}",kNexusModId);
  DWORD querySize=(DWORD)strlen(query);
  if(!WinHttpSendRequest(request.h,L"Content-Type: application/json\r\nCache-Control: no-cache\r\nPragma: no-cache\r\n",(DWORD)-1,query,querySize,querySize,0)||!WinHttpReceiveResponse(request.h,nullptr)){Log("Update check: HTTP send/receive failed, WindowsError=%lu",GetLastError());return false;}
  DWORD code=0,size=sizeof(code);if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX)||code!=200){Log("Update check: HTTP status=%lu WindowsError=%lu",code,GetLastError());return false;}
  char body[4097]{};DWORD used=0;ULONGLONG start=GetTickCount64();
  for(;;){DWORD read=0;char chunk[256];if(!WinHttpReadData(request.h,chunk,sizeof(chunk),&read))return false;if(!read)break;if(used+read>4096 || GetTickCount64()-start>10000)return false;
    for(DWORD i=0;i<read;++i)if(chunk[i]==0)return false;
    memcpy(body+used,chunk,read);used+=read;
  }
  if(!ParseNexusResponse(body,kNexusModId,out)){Log("Update check: invalid Nexus response: %.512s",body);return false;}return true;
}
static DWORD WINAPI Worker(void*){
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  Log("Update check: starting Nexus mod=%u installed=%s",kNexusModId,kModVersion);
  UpdateManifest result;if(!FetchManifest(result)){Log("Update check: Nexus request failed; retry scheduled (WindowsError=%lu)",GetLastError());const auto previous=state.load(std::memory_order_acquire);if(previous!=UpdateState::Available && previous!=UpdateState::Current)state.store(UpdateState::Unavailable,std::memory_order_release);checking.store(false,std::memory_order_release);return 0;}
  const bool differs=UpdateVersionDiffers(result.version,kModVersion);
  Log("Update check: Nexus=%s installed=%s mismatch=%u",result.version,kModVersion,(unsigned)differs);
  AcquireSRWLockExclusive(&manifestLock);manifest=result;
  state.store(differs?UpdateState::Available:UpdateState::Current,std::memory_order_release);
  ReleaseSRWLockExclusive(&manifestLock);checking.store(false,std::memory_order_release);return 0;
}
void StartUpdateCheck(bool menuOpened){
  if(!kNexusModId){state=UpdateState::NotConfigured;return;}
  const auto now=GetTickCount64(),last=lastAttempt.load(std::memory_order_relaxed);
  const ULONGLONG interval=menuOpened?5000:60000;
  if(last && now-last<interval)return;
  if(checking.exchange(true,std::memory_order_acq_rel))return;
  const auto recent=lastAttempt.load(std::memory_order_relaxed);
  if(recent && now-recent<interval){checking=false;return;}
  lastAttempt.store(now?now:1,std::memory_order_relaxed);
  const auto previous=state.load(std::memory_order_acquire);
  if(previous!=UpdateState::Available && previous!=UpdateState::Current)state=UpdateState::Checking;
  // The module already forbids explicit unload; pin it before a worker uses its code.
  static bool pinned=false;
  if(!pinned){HMODULE self{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Worker),&self)){state=UpdateState::Unavailable;checking=false;return;}pinned=true;}
  HANDLE thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);if(thread)CloseHandle(thread);else{state=UpdateState::Unavailable;checking=false;}
}
UpdateState GetUpdateState(){return state.load(std::memory_order_acquire);}
const char* LatestUpdateVersion(){static thread_local char value[32]{};AcquireSRWLockShared(&manifestLock);snprintf(value,sizeof(value),"%s",GetUpdateState()==UpdateState::Available?manifest.version:"");ReleaseSRWLockShared(&manifestLock);return value;}
const char* UpdateDownloadURL(){static thread_local char value[256]{};AcquireSRWLockShared(&manifestLock);snprintf(value,sizeof(value),"%s",GetUpdateState()==UpdateState::Available?manifest.url:"");ReleaseSRWLockShared(&manifestLock);return value;}
static DWORD WINAPI BrowserWorker(void*){
  const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
  wchar_t url[256]{};
  const bool converted=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,browserURL,-1,url,256)>0;
  const auto result=converted?reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",url,nullptr,nullptr,SW_SHOWNORMAL)):0;
  if(SUCCEEDED(com))CoUninitialize();
  browserFailed.store(result<=32);browserOpening.store(false);return 0;
}
void OpenUpdateDownload(bool developerPreview){
  char target[256]{};
  if(developerPreview && Cfg().developerMenuTest && kNexusModId)
    snprintf(target,sizeof(target),"https://www.nexusmods.com/reddeadredemption2/mods/%u",kNexusModId);
  else{
    snprintf(target,sizeof(target),"%s",UpdateDownloadURL());
    if(!NexusURL(target))return;
  }
  if(!NexusURL(target))return;
  if(browserOpening.exchange(true))return;
  memcpy(browserURL,target,sizeof(browserURL));
  browserFailed=false;
  HANDLE thread=CreateThread(nullptr,0,BrowserWorker,nullptr,0,nullptr);
  if(thread)CloseHandle(thread);else{browserFailed=true;browserOpening=false;}
}
bool UpdateBrowserOpening(){return browserOpening.load();}
bool UpdateBrowserFailed(){return browserFailed.load();}
}
