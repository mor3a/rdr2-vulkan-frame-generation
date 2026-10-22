#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include "turing_unlock.h"
int main(int argc,char** argv){
 if(argc!=3)return 2;
 auto nvapi=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
 using Query=void*(__cdecl*)(unsigned);using Init=int(__cdecl*)();using Enum=int(__cdecl*)(void**,unsigned*);
 using Arch=int(__cdecl*)(void*,void*);struct ArchInfo{unsigned version,architecture,implementation,revision;};
 auto query=(Query)GetProcAddress(nvapi,"nvapi_QueryInterface");
 auto init=(Init)query(0x0150e828);auto enumerate=(Enum)query(0xe5ac921f);void* gpus[64]{};unsigned count{};
 if(init()!=0 || enumerate(gpus,&count)!=0 || !count)return 9;
 ArchInfo before{sizeof(ArchInfo)|0x10000u};auto archBefore=(Arch)query(0xd8265d24);int originalStatus=archBefore(gpus[0],&before);
 printf("NVAPI before unlock: status=%d architecture=0x%x\n",originalStatus,before.architecture);
 wchar_t directory[1024]{};MultiByteToWideChar(CP_UTF8,0,argv[1],-1,directory,1024);
 auto slash=wcsrchr(directory,L'\\');if(!slash)return 7;*slash=0;
 char error[512]{};
 if(!fgvk::LoadTuringUnlock040(directory,error,sizeof(error))){printf("%s\n",error);return 8;}
 auto dll=GetModuleHandleA(argv[1]);
 if(!dll){printf("proxy load failed: %lu\n",GetLastError());return 3;}
 if(!GetProcAddress(dll,"DlssgProxy_Name") || !GetProcAddress(dll,"DlssgProxy_Role"))return 4;
 query=(Query)GetProcAddress(nvapi,"nvapi_QueryInterface");
 ArchInfo after{sizeof(ArchInfo)|0x10000u};int unlockStatus=((Arch)query(0xd8265d24))(gpus[0],&after);
 printf("NVAPI before Streamline init: status=%d architecture=0x%x\n",unlockStatus,after.architecture);
 if(originalStatus==0 && (before.architecture==0x170 || before.architecture==0x160) && (unlockStatus!=0 || after.architecture!=0x1b0))return 10;
 // Trigger the exact provider load used by Streamline, without creating a GPU device.
 auto provider=LoadLibraryExA(argv[2],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
 if(!provider){printf("provider load failed: %lu\n",GetLastError());return 5;}
 if(!GetProcAddress(provider,"NVSDK_NGX_D3D12_CreateFeature"))return 6;
 if(fgvk::LoadTuringUnlock040(directory,error,sizeof(error)) || !strstr(error,"already loaded"))return 11;
 Sleep(100);
 printf("Pinned 0.4.0 private proxy and matching DLSS-G provider loaded in isolated process.\n");
 // Runtime hooks stay mapped for process lifetime, just as in the game.
 return 0;
}
