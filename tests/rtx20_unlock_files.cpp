#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cassert>
#include <string>
#include <cstdio>
#include "turing_unlock.h"
int main(int argc,char** argv){assert(argc==2);wchar_t root[1024]{};MultiByteToWideChar(CP_UTF8,0,argv[1],-1,root,1024);char detail[512]{};
 assert(fgvk::VerifyTuringUnlock040(root,detail,sizeof(detail)));
 assert(!fgvk::VerifyTuringUnlock040(L"Z:\\missing-unlock-fixture",detail,sizeof(detail)));
 assert(strstr(detail,"version.dll"));
 wchar_t temp[MAX_PATH]{},file[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);GetTempFileNameW(temp,L"t20",0,file);DeleteFileW(file);assert(CreateDirectoryW(file,nullptr));
 auto corrupt=std::wstring(file)+L"\\version.dll";auto h=CreateFileW(corrupt.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);assert(h!=INVALID_HANDLE_VALUE);DWORD written;assert(WriteFile(h,"corrupt",7,&written,nullptr));CloseHandle(h);
 assert(!fgvk::VerifyTuringUnlock040(file,detail,sizeof(detail)));assert(strstr(detail,"version.dll"));assert(DeleteFileW(corrupt.c_str()));assert(RemoveDirectoryW(file));
 puts("Pinned complete package accepted; missing/corrupted proxy rejected before load: PASS");}
