// Derived entry discovery: dashdogy/MFG-Unlocker v1.5.0 (MIT).
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include "third_party/minhook/include/MinHook.h"
#include "dlssg_provider_policy.h"
namespace {
HMODULE Owner(const void* p) { HMODULE m{};GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)p,&m);return m; }
bool OwnedExecutable(HMODULE m,const void* p,size_t n){MEMORY_BASIC_INFORMATION b{};if(!VirtualQuery(p,&b,sizeof(b)) || b.AllocationBase!=m || b.State!=MEM_COMMIT || b.Type!=MEM_IMAGE || (b.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(b.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;auto x=(uintptr_t)p-(uintptr_t)b.BaseAddress;return x<b.RegionSize && n<=b.RegionSize-x;}
void* FindCachedReader(HMODULE module) noexcept
{
    __try
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96) return nullptr;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (imageSize < 4096 || imageSize > 1024u*1024u*1024u
            || reinterpret_cast<uintptr_t>(sections + nt->FileHeader.NumberOfSections) - base > imageSize) return nullptr;
        // mov ecx,FG preset key; call bool(uint32_t,uint32_t*); test al; jz.
        constexpr uint8_t prefix[]{0xb9,0xf1,0x1d,0xe4,0x10,0xe8};
        constexpr uint8_t suffix[]{0x84,0xc0,0x0f,0x84};
        constexpr uint8_t prologue[]{0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,
            0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x8b,0xd9};
        void* selected = nullptr;
        for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.VirtualAddress >= imageSize) continue;
            const size_t length = std::min<size_t>(section.Misc.VirtualSize, imageSize-section.VirtualAddress);
            const auto* start = reinterpret_cast<const uint8_t*>(base+section.VirtualAddress);
            for (size_t offset = 0; offset+18 <= length; ++offset)
            {
                const auto* call = start+offset;
                if (memcmp(call, prefix, sizeof(prefix)) || memcmp(call+10, suffix, sizeof(suffix))) continue;
                int32_t callOffset = 0, branchOffset = 0;
                memcpy(&callOffset, call+6, 4); memcpy(&branchOffset, call+14, 4);
                const uintptr_t target = reinterpret_cast<uintptr_t>(call+10)+callOffset;
                const uintptr_t branch = reinterpret_cast<uintptr_t>(call+18)+branchOffset;
                if (target < base || target-base >= imageSize || sizeof(prologue) > imageSize-(target-base)
                    || !OwnedExecutable(module, call, 18)
                    || !OwnedExecutable(module, reinterpret_cast<void*>(target), sizeof(prologue))
                    || !OwnedExecutable(module, reinterpret_cast<void*>(branch), 1)
                    || memcmp(reinterpret_cast<void*>(target), prologue, sizeof(prologue))) return nullptr;
                DWORD64 owner = 0;
                const auto* function = RtlLookupFunctionEntry(target, &owner, nullptr);
                const uintptr_t pdata = reinterpret_cast<uintptr_t>(function);
                if (!function || owner != base || pdata < base || pdata-base >= imageSize
                    || sizeof(*function) > imageSize-(pdata-base) || Owner(function) != module
                    || base+function->BeginAddress != target
                    || function->EndAddress <= function->BeginAddress || function->EndAddress > imageSize
                    || function->UnwindData >= imageSize
                    || (selected && selected != reinterpret_cast<void*>(target))) return nullptr;
                // 310.9.16 reads the key at two call sites (settings and their report); one reader only.
                selected = reinterpret_cast<void*>(target);
            }
        }
        return selected;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

using Reader=bool(*)(uint32_t,uint32_t*);
Reader original{};
HMODULE selected{};
std::mutex installLock;
std::atomic<uint32_t> observed{}, failure{}, requested{};
std::atomic<uint64_t> reads{};
std::atomic<bool> valid{};
bool Hook(uint32_t id,uint32_t* value){
 if(id!=0x10e41df1 || !value || Owner(_ReturnAddress())!=selected)return original(id,value);
 bool ok;
 auto preset=requested.load();
 if(preset>=1 && preset<=3){*value=preset;ok=true;}else ok=original(id,value);
 if(ok){observed.store(*value);valid.store(true);reads.fetch_add(1);}
 return ok;
}
}
void PreparePresetRuntime(HMODULE module){
 std::lock_guard<std::mutex> lock(installLock);
 if(selected==module || !module)return;
 wchar_t path[MAX_PATH]{};GetModuleFileNameW(module,path,MAX_PATH);
 dlssg_provider_policy::VersionTriplet version{};
 if(!dlssg_provider_policy::IsDlssgImplementationModule(module) || !dlssg_provider_policy::ReadProviderVersion(path,version) || version.major!=310 || version.minor!=9 || version.build!=16){failure=1;return;}
 void* target=FindCachedReader(module);if(!target){failure=2;return;}
 wchar_t ini[MAX_PATH]{};GetModuleFileNameW(nullptr,ini,MAX_PATH);auto slash=wcsrchr(ini,L'\\');if(!slash){failure=3;return;}wcscpy(slash+1,L"RDR2VulkanFG.ini");
 auto setting=GetPrivateProfileIntW(L"fgvk",L"DLSSGPreset",3,ini);if(setting>3){failure=4;return;}
 auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED){failure=5;return;}
 selected=module;requested=setting;
 if(MH_CreateHook(target,(void*)&Hook,(void**)&original)!=MH_OK){selected=nullptr;failure=6;return;}
 if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);selected=nullptr;failure=7;return;}
 failure=0;
}
extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockPresetSnapshot(uint32_t* preset,uint32_t* requestedValue,uint32_t* error,uint64_t* count){
 if(!preset || !requestedValue || !error || !count)return FALSE;
 *preset=observed.load();*requestedValue=requested.load();*error=failure.load();*count=reads.load();return valid.load();
}
// The host calls this only after retiring/releasing the old FG feature.
// A new provider read, rather than this request, establishes effective feedback.
extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockSetPreset(uint32_t preset){
 if(preset>3)return FALSE;
 std::lock_guard<std::mutex> lock(installLock);
 requested.store(preset);valid.store(false);observed.store(0);
 return selected && failure.load()==0;
}
