#include "../src/xess_output_pacing.h"
#include "../vendor/xemfg-reference/runtime_layout.h"
#include <cstdio>
namespace fgvk {void Log(const char*,...){} void LogDebug(const char*,...){} bool DebugLoggingEnabled(){return false;}}
int main(int argc,char** argv){
 if(argc!=2)return 2;
 auto module=LoadLibraryExA(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!module)return 3;
 try{
  const auto& layout=xess_layout::Get(module);layout.VerifyFunction(module,0x224b30);layout.VerifyFunction(module,0x21ee30);layout.VerifyFunction(module,0x21f730);
  if(!layout.Rva(0x21ef96) || !layout.Rva(0x157c3a0) || !layout.Rva(0x157a980))return 4;
  if(!fgvk::xess_pacing::Install(module))return 5;
  if(!fgvk::xess_pacing::Install(module))return 6;
  if(fgvk::xess_pacing::Bind(nullptr,nullptr) || fgvk::xess_pacing::Bind((void*)0x1234,(ID3D12Device*)0x1234))return 7;
  fgvk::xess_pacing::Reset(nullptr);fgvk::xess_pacing::Unbind(nullptr);
 }catch(const std::exception& e){std::printf("XeSS pacing layout failed: %s\n",e.what());return 1;}
 std::puts("XeSS bundled-DLL pacing witnesses, hook transaction/reuse and invalid-owner rejection PASS; no GPU context/game executed");return 0;
}
