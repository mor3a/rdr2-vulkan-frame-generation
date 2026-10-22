#include "../vendor/xemfg-reference/host_unlock.h"
#include <cstdio>
int main(int argc,char** argv){
 if(argc!=3)return 2;auto fg=LoadLibraryExA(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);auto ll=LoadLibraryExA(argv[2],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
 if(!fg || !ll)return 3;
 try{xess_host_unlock::Unlock patch(fg,ll,3,true);if(patch.Applied().size()<4)return 4;patch.Restore();if(!patch.Applied().empty())return 5;}
 catch(const std::exception& e){printf("XeMFG validation failed: %s\n",e.what());return 1;}
 FreeLibrary(fg);FreeLibrary(ll);puts("XeMFG: bundled DLL witnesses, 4x patch transaction and restoration PASS; no GPU context/game executed");
}
