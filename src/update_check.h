#pragma once
namespace fgvk {
inline constexpr const char* kModVersion="0.30";
enum class UpdateState { NotStarted, NotConfigured, Checking, Current, Available, Unavailable };
void StartUpdateCheck();
UpdateState GetUpdateState();
const char* LatestUpdateVersion();
const char* UpdateDownloadURL();
void OpenUpdateDownload();
bool UpdateBrowserOpening();
bool UpdateBrowserFailed();
}
