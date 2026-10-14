#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fgvk {
// Bind only this plugin's delay imports. The game's import table and loaded
// legacy NvLowLatencyVk module remain untouched.
struct DelayDescriptor { uint32_t attributes,name,module,iat,intable,bound,unload,timestamp; };
struct LatencyBinding { unsigned imports{}; const char* error{}; };
using LatencyResolver=FARPROC (*)(HMODULE,LPCSTR);
inline bool BindLatencyImports(HMODULE plugin, HMODULE latency, LatencyBinding& result,
                               LatencyResolver resolve=GetProcAddress){
  auto base=reinterpret_cast<unsigned char*>(plugin);
  auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if(!base || dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0){result.error="invalid DOS header";return false;}
  auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
  if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC){result.error="invalid PE64 header";return false;}
  const size_t size=nt->OptionalHeader.SizeOfImage;
  auto within=[&](uint32_t offset,size_t bytes){return offset && offset<size && bytes<=size-offset;};
  const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
  if(!directory.VirtualAddress || !directory.Size)return true;
  if(!within(directory.VirtualAddress,directory.Size)){result.error="invalid delay directory";return false;}
  auto descriptors=reinterpret_cast<DelayDescriptor*>(base+directory.VirtualAddress);
  for(size_t i=0;i<directory.Size/sizeof(DelayDescriptor);++i){
    auto& d=descriptors[i];if(!d.name)break;
    if(!within(d.name,1) || !memchr(base+d.name,0,size-d.name)){result.error="invalid DLL name";return false;}
    if(_stricmp(reinterpret_cast<char*>(base+d.name),"NvLowLatencyVk.dll"))continue;
    if(d.attributes!=1 || !within(d.module,sizeof(HMODULE))){result.error="unsupported delay descriptor";return false;}
    std::vector<FARPROC> addresses;
    for(size_t index=0;;++index){
      size_t offset=index*sizeof(IMAGE_THUNK_DATA64);
      if(offset>UINT32_MAX || !within(d.intable,sizeof(IMAGE_THUNK_DATA64)+offset) || !within(d.iat,sizeof(IMAGE_THUNK_DATA64)+offset)){result.error="invalid delay thunks";return false;}
      auto thunk=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+d.intable)[index].u1.AddressOfData;
      if(!thunk)break;
      LPCSTR name{};
      if(IMAGE_SNAP_BY_ORDINAL64(thunk))name=MAKEINTRESOURCEA(IMAGE_ORDINAL64(thunk));
      else {
        if(thunk>UINT32_MAX || !within(static_cast<uint32_t>(thunk),sizeof(WORD)+1)){result.error="invalid import name";return false;}
        name=reinterpret_cast<char*>(base+thunk+sizeof(WORD));
        if(!memchr(name,0,size-thunk-sizeof(WORD))){result.error="unterminated import name";return false;}
      }
      auto address=resolve(latency,name);
      if(!address){result.error="bundled latency DLL is missing a required export";return false;}
      addresses.push_back(address);
    }
    if(addresses.empty()){result.error="empty latency import table";return false;}
    auto table=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+d.iat);
    auto module=reinterpret_cast<HMODULE*>(base+d.module);
    DWORD tableProtection{},moduleProtection{};
    if(!VirtualProtect(table,addresses.size()*sizeof(*table),PAGE_READWRITE,&tableProtection)){result.error="cannot protect delay IAT";return false;}
    if(!VirtualProtect(module,sizeof(*module),PAGE_READWRITE,&moduleProtection)){
      DWORD ignored{};VirtualProtect(table,addresses.size()*sizeof(*table),tableProtection,&ignored);
      result.error="cannot protect delay module slot";return false;
    }
    // Resolve every required export before changing any entry. This runs before
    // Vulkan device/plugin startup, with no concurrent users of these imports.
    for(size_t index=0;index<addresses.size();++index)table[index].u1.Function=reinterpret_cast<ULONG_PTR>(addresses[index]);
    *module=latency;
    DWORD ignored{};
    VirtualProtect(module,sizeof(*module),moduleProtection,&ignored);
    VirtualProtect(table,addresses.size()*sizeof(*table),tableProtection,&ignored);
    result.imports+=static_cast<unsigned>(addresses.size());
  }
  return true;
}
}
